# shadNet integration audit (2026-10-07)

Inspected read-only: `DarkIzuku/shadNet` main
`059b9e1fe7886acb7a346ff1264779be6cf72ecb` and
`codex/bloodborne-stats-experimental`
`34ef2e4bab83d2f3c9d4c573f0ab9ab78c258e1b`. Neither was modified.

## Current compatibility

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
- Both checked branches contain JSON WebAPI/stats routes, no browser signup
  page or bbhost `/auth/*` service. The user indicates a web signup is part of
  their deployed server. Its deployed URL/source version is not yet known;
  do not infer a login mechanism or request/send credentials to a guessed URL.

The launcher provides Offline and custom native server profiles, plus an
explicit account-page URL. shadNet is not advertised as a working preset.
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
