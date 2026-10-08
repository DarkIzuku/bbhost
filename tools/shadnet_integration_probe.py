import argparse,json,secrets,shutil,socket,struct,subprocess,time,urllib.request
from pathlib import Path

args=argparse.ArgumentParser()
args.add_argument('--probe',type=Path,required=True)
args.add_argument('--server-dir',type=Path,required=True)
args.add_argument('--work-dir',type=Path,required=True)
options=args.parse_args()
base=options.work_dir.resolve().parent
source=options.server_dir.resolve()
root=options.work_dir.resolve()
assert (source/'shadnet.exe').is_file(),'Server executable missing'
root.mkdir(parents=True,exist_ok=False)
# Only runtime assets: never copy a deployment's databases, account files or config.
for pattern in ('*.exe','*.dll','worlds.cfg','scoreboards.cfg'):
    for asset in source.glob(pattern): shutil.copy2(asset,root/asset.name)
for folder in ('web','platforms','sqldrivers','tls','imageformats','styles','iconengines'):
    if (source/folder).is_dir(): shutil.copytree(source/folder,root/folder)
ports=(42313,42314,42315,42316)
for port in ports:
    with socket.socket() as s: s.bind(('127.0.0.1',port))
(root/'shadnet.cfg').write_text('''[Server]
Host=127.0.0.1
UnsecuredPort=42313
[Network]
Matching2Enabled=true
MatchingUdpPort=42314
WebApiPort=42315
[Bloodborne]
BootstrapEnabled=true
PublicBaseUrl=http://127.0.0.1:42315
ReferenceProxyEnabled=false
SeamlessCoop=false
[Website]
Enabled=true
Port=42316
RegistrationEnabled=true
ExternalAssetsEnabled=true
ExternalAssetsPath=web
[Accounts]
EmailValidated=false
[Stats]
Enabled=false
''',encoding='utf-8')
server_log=(root/'integration-server.log').open('wb')
server=subprocess.Popen([str(root/'shadnet.exe')],cwd=root,stdout=server_log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
clients=[]
udp_sockets=[]
try:
    api='http://127.0.0.1:42316'
    end=time.monotonic()+25
    while True:
        try:
            with urllib.request.urlopen(api+'/register',timeout=1) as r: assert r.status==200
            break
        except OSError:
            assert server.poll() is None,'isolated server failed to start'
            if time.monotonic()>end: raise
            time.sleep(.25)
    print('latest server: website registration page and local listeners ready',flush=True)
    accounts=[]
    def web(path,body):
        req=urllib.request.Request(api+path,data=json.dumps(body).encode(),headers={'Content-Type':'application/json','Origin':api})
        with urllib.request.urlopen(req,timeout=3) as r:
            return r.status,json.load(r),r.headers.get('Set-Cookie','')
    for i in range(2):
        username='Probe'+secrets.token_hex(4)
        password=secrets.token_urlsafe(18)
        status,reply,_=web('/api/register',{'username':username,'password':password,'confirmPassword':password})
        assert status==201,(status,reply)
        status,reply,cookie=web('/api/login',{'username':username,'password':password})
        assert status==200 and cookie,(status,reply)
        accounts.append((username,password))
    print('website: two temporary accounts created and signed in without Discord',flush=True)
    if options.probe:
        def start(account):
            p=subprocess.Popen([str(options.probe.resolve()),'tcp://127.0.0.1:42313'],cwd=base,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
            clients.append(p)
            def command(value):
                p.stdin.write(json.dumps(value)+'\n');p.stdin.flush()
                line=p.stdout.readline()
                while line and not line.strip(): line=p.stdout.readline()
                assert line,'native transport closed unexpectedly'
                result=json.loads(line);assert result['ok'],result.get('error','request failed');return result['reply']
            command({'action':'login','name':account[0],'password':account[1]})
            r=command({'path':'/mp/matching2/context_start'})
            assert r['Matching2Enabled']
            return command
        one,two=start(accounts[0]),start(accounts[1])
        def discover(account):
            s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('127.0.0.1',0));s.settimeout(3);udp_sockets.append(s)
            request=b'\xff'*4+b'\x01'+account[0].encode().ljust(16,b'\0')+b'\0'*4
            s.sendto(request,('127.0.0.1',42314))
            response,sender=s.recvfrom(128)
            assert sender==('127.0.0.1',42314) and len(response)==10 and response[:4]==b'\xff'*4
            assert socket.inet_ntoa(response[4:8])=='127.0.0.1' and struct.unpack('!H',response[8:10])[0]==s.getsockname()[1]
            return s.getsockname()[1]
        local_ports=[discover(a) for a in accounts]
        endpoint=one({'path':'/np/signaling/resolve','body':{'OnlineId':accounts[1][0]}})
        assert endpoint['Addr']=='127.0.0.1' and endpoint['Port']==local_ports[1],endpoint
        created=one({'path':'/mp/matching2/create_room','body':{'MaxMembers':5}})
        room=created['RoomId'];assert int(room)>0 and created['MemberId']>0
        joined=two({'path':'/mp/matching2/join_room','body':{'RoomId':room}})
        assert joined['RoomId']==room and len(joined['Members'])==2
        event=one({'path':'/np/events/poll'})
        assert event['HasEvent'] and event['Name']=='room_member_joined'
        cached=one({'path':'/mp/matching2/session_blob?SessionId='+room})
        assert len(cached['Members'])==2,'join event did not update native room cache'
        two({'path':'/mp/matching2/leave_room','body':{'SessionId':room}})
        event=one({'path':'/np/events/poll'})
        assert event['HasEvent'] and event['Name']=='room_member_left'
        cached=one({'path':'/mp/matching2/session_blob?SessionId='+room})
        assert len(cached['Members'])==1,'leave event did not update native room cache'
        joined=two({'path':'/mp/matching2/join_room','body':{'RoomId':room}})
        assert len(joined['Members'])==2
        assert one({'path':'/np/events/poll'})['Name']=='room_member_joined'
        one({'path':'/mp/matching2/kick_member','body':{'SessionId':room,'MemberId':joined['MemberId']}})
        assert two({'path':'/np/events/poll'})['Name']=='room_member_kicked'
        event=one({'path':'/np/events/poll'})
        assert event['Name']=='room_member_left' and event['Reason']=='kicked'
        assert len(one({'path':'/mp/matching2/session_blob?SessionId='+room})['Members'])==1
        assert not two({'path':'/mp/matching2/heartbeat','body':{'SessionId':room}})['InRoom']
        two({'path':'/mp/matching2/join_room','body':{'RoomId':room}})
        assert one({'path':'/np/events/poll'})['Name']=='room_member_joined'
        two({'path':'/mp/matching2/leave_room','body':{'SessionId':room}})
        assert one({'path':'/np/events/poll'})['Name']=='room_member_left'
        one({'path':'/mp/matching2/leave_room','body':{'SessionId':room}})
        print('native transport: website login, negotiation, UDP discovery, peer endpoint resolution, rooms, cache, kick/rejoin and leave passed',flush=True)
        for p in clients:
            p.stdin.close();assert p.wait(timeout=8)==0
    print('isolated server checks passed; production server and accounts untouched',flush=True)
finally:
    for s in udp_sockets: s.close()
    for p in clients:
        if p.poll() is None: p.kill();p.wait()
    if server.poll() is None: server.terminate();server.wait(timeout=10)
    server_log.close()
