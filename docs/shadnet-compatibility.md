# Native shadNet compatibility (2026-10-08)

The WPF launcher supports a custom shadNet TCP endpoint, a separate game
WebAPI URL, the website/account-page URL, Online ID and the native P2P port.
Registration opens the website integrated in the server. There is no Discord
account flow and no HunterDream server preset. Configuration remains native
bbhost TOML; switching servers selects a distinct account profile.

## Current server provenance

The complete integrated website and Bloodborne bootstrap are present in
`DarkIzuku/shadnet-p2p`, commit
`b72a4ce3a480ab8e9d6a5c38aa3f7d3aa5becc68`. The latest TCP keepalive fix,
`2f5f5d71bb077e6386ae01f68f6993014bcaee5c`, was applied onto that complete
base on `bbhost-compatibility-web-v1`, resulting in
`63aacc5ab636f4b0eb88289985b31d8db24bd718`.

Do not replace this with an older `DarkIzuku/shadNet` branch or a TCP-only
branch that omits the website/bootstrap. The Windows server build and its
10 checks passed in Actions run 37741152448. The running user deployment,
its configuration, accounts and database have not been replaced or stopped.

Default services are TCP 31313, matching UDP 31314, game WebAPI 31315 and
website 31316. These endpoints are editable, including HTTPS reverse proxies.
Website HttpOnly sessions are independent of game Bearer tokens.

## Implemented client boundary

`src/net/shadnet.cpp` adapts bbhost's existing account/session requests to the
server's v1 TCP framing and protobuf messages. It does not import the shadPS4
networking stack. Existing Matching2 callbacks, signaling and native P2P
sockets remain in use. Native bbhost servers retain their original transport.

Implemented: website-account login, game token retrieval, capability query,
context start, room creation/join/leave/kick, room membership events, local
session cache, heartbeat and peer endpoint resolution. shadNet UDP discovery
uses the existing P2P socket; ordinary STUN and bbhost relay support remain
available on the native transport.

Passwords travel through the launcher's private stdin pipe, never command-line
arguments. Remembered Windows credentials use DPAPI and are bound to the
selected account/server profile. A server change cannot silently reuse another
server's token. Certificate verification remains configurable and enabled by
default for TLS endpoints.

The compatibility boundary is protocol major v1, not a server executable SHA.
Unknown additive protobuf fields are ignored. Optional capabilities default
conservatively when absent. Additive server fixes need no client rebuild;
breaking authentication or wire-format changes require compatible server
support or a client update. Arbitrary incompatible server versions are not
promised to work.

## Verification and remaining checks

Two isolated native transport clients authenticated with temporary accounts
created and logged in through the latest server's integrated website. They
passed capability negotiation, context start, room create/join, membership
notifications and leave. Expanded checks exercise UDP discovery, peer endpoint
resolution, session-cache updates, kick and rejoin. Those checks exposed a
kick-cause mismatch: Matching2 KICKOUT_ACTION is 2, now covered by a wire
regression test. The corrected e198c62 Windows diagnostic passed the expanded full test against server 63aacc5, including kick/rejoin and cache updates.

`tools/shadnet_integration_probe.py` copies only executable/runtime assets into
a new test directory, starts its own server on loopback ports 42313-42316,
creates temporary accounts and stops only its own child processes. It never
copies or modifies a deployment's account database or configuration:

```text
python tools/shadnet_integration_probe.py --server-dir CLEAN_SERVER_PACKAGE --probe shadnet_transport_probe.exe --work-dir NEW_TEST_DIRECTORY
```

These transport tests do not certify two running games, summoning, invasions,
bloodstains, ghosts or WAN NAT traversal. Those require end-to-end game tests
and, for WAN, two hosts. Reachability alone is never reported as successful
multiplayer. There is no implemented UPnP toggle advertised in the launcher.