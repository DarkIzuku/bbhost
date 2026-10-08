# shadNet integration audit (2026-10-07)

Inspected read-only: `DarkIzuku/shadNet` main
`059b9e1fe7886acb7a346ff1264779be6cf72ecb` and
`codex/bloodborne-stats-experimental`
`34ef2e4bab83d2f3c9d4c573f0ab9ab78c258e1b`. Neither was modified.

## Recovered integrated website source

The user's website exists in local work that was not pushed to those two
remote branches. Recovered read-only from the earlier 2026-08-21 workspace,
`work/shadnet-online-features`, branch `bloodborne-bootstrap`, HEAD
`115adf2f47e824b17698c7179e41213ca6e9e37a` plus uncommitted changes. The
editable frontend is also present at `D:/CODEX/web`. Neither copy,
its history, configuration, databases nor binaries was modified here.

Provenance of the actual recovered working files (SHA-256):

- `src/bloodborne_website.cpp`:
  `1404a8da845fb40627521404fbed13eb4a7b70c9b248b74dd70aacf03266e11d`.
- `documentation/bloodborne-website.md`:
  `df14f48fc27c302db066d38c9949ff2d59bf9c49a39f22569e40222fc59ac51b`.
- `src/webapi_routes_bloodborne_bootstrap.cpp`:
  `e03c5c2a5a80f78d5b305c1446cc57588913ce7cef180244443b96c9efdb6c8f`.

The Hunter's Requiem is a Qt HTTP listener inside the server process,
normally on port 31316; the game WebAPI remains on 31315. It serves
`/register`, `/login`, `/account`, and JSON `/api/register`, `/api/login`,
`/api/account`. Registration calls the same `RegisterShadNetAccount`
service as TCP account creation. The website uses an independent HttpOnly
cookie and CSRF value: a web session is not a bbhost game token. The launcher
can build the registration link from the user's server origin; its port,
path and HTTPS proxy URL remain editable and persist in native TOML.

## Current game compatibility

Changing bbhost's server URL to shadNet does not implement multiplayer:

- shadNet authenticates/registers over its TCP protocol (15-byte header,
  protocol version 1, protobuf Login/Create messages). Default ports are
  TCP 31313, matching UDP 31314, and WebAPI TCP 31315. Matching2 is disabled
  in its default configuration. See `documentation/protocol.md`,
  `src/cmd_account.cpp`, `src/config.cpp`.
- bbhost authenticates through JSON `/auth/account/create`, `/auth/recover`,
  `/auth/device/*` and requires `ResKind`, `Token`, `OnlineId` replies.
  Matchmaking uses JSON `/mp/matching2/context_start`, `/create_room`,
  `/join_room`, `/heartbeat` and `/np/events/poll`. See `net/account.cpp`
  and `net/session.cpp`. Existing STUN, P2P and relay code must be retained.
- shadNet's `/v1/users`, `/v1/sessions` and presence WebAPI is not this API;
  its Bearer token is resolved through its own database. `/status` is a
  service-status response, not an account-creation page.
- The two published branches contain no browser signup page. The recovered
  local source above DOES contain it, as the user described. It still has no
  bbhost `/auth/*` account service or JSON `/mp/matching2/*` contract. Its
  Bloodborne bootstrap/world-data extensions do not implement that contract.
  Do not submit game credentials to the independent web-cookie endpoints.

The launcher provides Offline and custom native server profiles, plus an
explicit account-page URL and a link helper for shadNet's integrated website.
shadNet is not advertised as a working game/multiplayer preset yet.
Discord is disabled. Account creation in this frontend opens the configured
page, never a local shadNet client registration command.

## Server versions without a client release per update

The intended compatibility boundary is a stable protocol, not a repository
commit/version string. A shadNet gateway must preserve bbhost's JSON contract
while mapping accounts, room lifecycle, events, signaling endpoints and relay
to the deployed shadNet protocol. Keep this at `net/session`/server boundaries;
do not transplant shadPS4's networking stack or change the engine hooks.

The gateway should negotiate a protocol major version and optional capabilities,
ignore additive fields, preserve v1 behavior, and default missing optional
features conservatively. bbhost already accepts old heartbeat replies missing
`InRoom` (`server_heartbeat`). Do not pin a server build SHA, auto-download
executable code from servers, or claim all versions are interchangeable.

Once that gateway contract is implemented and tested, server bug fixes and
additive upgrades can retain client compatibility. A breaking wire/auth change
still requires a gateway compatibility implementation or a client update.
Required validation: account lifecycle, two clients, room create/join/leave,
summons/invasions, WebAPI, event replay/ack, STUN, NAT/relay and disconnects.
No live deployment or multiplayer compatibility was tested in this stage.

## Launcher connection controls

The WPF Online page now exposes native WebAPI, `online.online_id`, P2P UDP
port and optional advertised IPv4/STUN endpoint. Its expandable network
section separates `online.np_server` (bbhost account/matching API) and
`online.auth_server` (optional separate account service), and exposes native
certificate validation and account-required switches. Native P2P, signaling,
NAT traversal and relay implementations are unchanged. There is no cosmetic
UPnP switch without a working native implementation, and no shadPS4 network
stack is imported.

Applying a custom server writes native TOML and reloads it. Account profiles
are named from the WebAPI, matching and auth bases together, so changing the
account service cannot silently reuse a different server's stored token.
Explicit API bases also avoid native fallback appending `:18671` to an
authority that already includes a custom WebAPI port. Applying a profile is
reported as configuration, not a successful account/server connection.

The import button reads the Bloodborne `host_overrides.json` mapping for
`https://ss4.scej-network.jp:20443`, validates its http/https target, fills the
WebAPI address and derives the editable account-page link. It never modifies
the JSON or activates online automatically. Real user addresses/credentials
are not included in the repository or default settings.

This import does not implement the distinct shadNet TCP login endpoint shown
in shadPS4's settings. The browser manages web passwords; bbhost retains its
native linked-account/recovery-code flow for compatible servers. The game
compatibility adapter described above is still required for shadNet.
Read-only local checks of the supplied WebAPI status and registration URL
received connection-refused errors; no account credentials were sent.
