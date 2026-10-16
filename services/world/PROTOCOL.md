# Kast World Protocol

**Protocol version: 2** (`kProtocolVersion` in `protocol/messages.h`, sent in `Hello`). Version 2 added `kind` and `template` to `Spawn`; a version 1 client gets `protocol_mismatch`.

The encoder and decoder live in `protocol/`, a static library with no Asio, database or ECS dependency, so a game client can reuse it.

## Transport

- **Connection:** TLS (1.2 or later) over TCP. Default port 7777.
- **Dev certificate:** with no certificate configured, the server generates a self-signed one with SANs `localhost`, `world` and `127.0.0.1`. Clients trust it by loading that file as their CA.
- **Byte order:** every integer is little-endian.
- **Floats:** `f32` is IEEE-754, written as its u32 bits. NaN and infinity are malformed.

### Frame

```
+----------------+----------------+----------------------+
| u16 length     | u16 opcode     | payload (length B)   |
+----------------+----------------+----------------------+
```

- **`length`:** the number of payload bytes, not counting the 4-byte header.
- **Size limit:** a frame whose `length` is over `MAX_FRAME_BYTES` (default 4096) gets `frame_too_large`. The server checks this from the header, before reading the payload.
- **Exact payload:** the payload must decode to exactly `length` bytes. Short fields, a string that runs past the end, trailing bytes, or a bad enum value all mean `malformed`.

### Field types

| Type | Encoding |
|---|---|
| `u8` / `u16` / `u32` / `u64` | Unsigned, little-endian |
| `f32` | IEEE-754 single precision, as u32 bits; must be finite |
| `str` | `u16` byte length + that many bytes (UTF-8 where noted) |
| `vec3` | `f32 x, f32 y, f32 z`. x/z is the ground plane; y is height |

## Handshake

```
client                                 server
  |---- TCP connect ----------------------->|   handshake deadline starts (HANDSHAKE_TIMEOUT_MS, 10 s)
  |<=== TLS handshake =====================>|
  |---- Hello{protocolVersion} ------------>|   mismatch -> Disconnect{protocol_mismatch}
  |---- Auth{accessToken} ----------------->|   JWT checked against auth's JWKS (README "Security")
  |<--- AuthOk{accountId, role} ------------|   or Disconnect{invalid_token | token_expired | service_unavailable}
  |                                         |   deadline cleared
  |---- CharListReq / CharCreate / ... ---->|   lobby: character requests
  |<--- CharList / CharCreated / CharError -|
  |---- EnterWorld{charId} ---------------->|
  |<--- EnterWorldOk{entityId, pos, ...} ---|   in world
  |---- Move / Say ------------------------>|
  |<--- Spawn / Despawn / EntityMove / ... -|
```

- **Deadline:** the whole handshake must finish within the deadline: TLS, `Hello` and `Auth`. Otherwise the server sends `Disconnect{handshake_timeout}`. A client that never starts TLS is simply closed, because there is no secure channel to send a reason on.
- **Message order:**
  - Before `AuthOk`, only `Hello` and then `Auth` are accepted. Any other message gets `Disconnect{unexpected_message}`, and so does any message sent while the token is being verified.
  - In the lobby, `Move` and `Say` are ignored.
  - In the world, a character request is answered with `CharError{already_in_world}`.
- **Token lifetime:** the token is checked only at the handshake. The session then lasts until disconnect (see README, "Ban gap").
- **Duplicate login:** a second login for an account that is already online kicks the old session with `Disconnect{logged_in_elsewhere}` and saves its character first.

### Heartbeat

- **Server pings:** the server sends `Ping{nonce}` every `PING_INTERVAL_MS` (default 20 s). The client must answer `Pong{nonce}`.
- **Client pings:** a client may also send `Ping`, and the server answers `Pong`.
- **Idle timeout:** a connection that sends nothing for `IDLE_TIMEOUT_MS` (default 60 s) gets `Disconnect{idle_timeout}`.

### Abuse limits

- **Message rate:** each connection is rate limited to `MSG_RATE_PER_SEC` messages per second (default 60), with bursts up to `MSG_BURST` (default 120).
  - Messages over the limit are dropped.
  - A sender that stays over the limit for `MSG_RATE_GRACE_MS` (default 2 s) without recovering gets `Disconnect{rate_limited}`.
- **Character requests:** one at a time per client. A request sent while another is in flight gets `CharError{rate_limited}`.
- **Send buffer:** a client that stops reading and lets more than `MAX_SEND_BUFFER_BYTES` pile up is dropped without a reason.
- **Connection limit:** a connection beyond `MAX_CONNECTIONS` completes TLS, then gets `Disconnect{server_full}`.

## Messages

C = client to server, S = server to client. "Payload" lists fields in wire order.

| Opcode | Name | Dir | Payload |
|---|---|---|---|
| `0x0001` | Hello | C | `u16 protocolVersion` |
| `0x0002` | Auth | C | `str accessToken` (auth-service JWT) |
| `0x0003` | Ping | C/S | `u32 nonce` |
| `0x0004` | Pong | C/S | `u32 nonce` (echoes the Ping) |
| `0x0010` | CharListReq | C | *(empty)* |
| `0x0011` | CharCreate | C | `str name` |
| `0x0012` | CharDelete | C | `u64 charId` |
| `0x0013` | EnterWorld | C | `u64 charId` |
| `0x0020` | Move | C | `vec3 pos, f32 heading, u32 clientTimeMs` |
| `0x0021` | Say | C | `str text` (UTF-8) |
| `0x0102` | AuthOk | S | `u64 accountId, u8 role` (0 player, 1 gm, 2 admin) |
| `0x0103` | Disconnect | S | `u16 reason` (code table below); the server closes after sending it |
| `0x0110` | CharList | S | `u8 count`, then `count` × CharInfo |
| `0x0111` | CharCreated | S | CharInfo |
| `0x0112` | CharDeleted | S | `u64 charId` |
| `0x0113` | EnterWorldOk | S | `u32 entityId, vec3 pos, f32 heading` |
| `0x0114` | CharError | S | `u16 requestOpcode, u16 code`; the request failed, the connection stays open |
| `0x0120` | Spawn | S | `u32 entityId, str name, vec3 pos, f32 heading, u8 kind` (0 player, 1 npc), `str template` (an NPC's client template; empty for players) |
| `0x0121` | Despawn | S | `u32 entityId` |
| `0x0122` | EntityMove | S | `u32 entityId, vec3 pos, f32 heading` |
| `0x0123` | Correction | S | `vec3 pos` (snap back to the last valid position) |
| `0x0124` | ChatMsg | S | `str fromName, str text` |
| `0x0125` | ComponentUpdate | S | `u32 entityId, str name, str json` (a Lua component's full value, see below) |
| `0x0126` | ComponentRemove | S | `u32 entityId, str name` |
| `0x0127` | ServerEvent | S | `str name, str json` (a named event a Lua system sent, see below) |

**CharInfo:** `u64 id, str name, vec3 pos`.

Any other opcode, including a server-to-client opcode sent by a client, gets `Disconnect{unknown_opcode}` or `Disconnect{unexpected_message}`.

### Characters

- **Name rules:** `CharCreate` names are 3–16 ASCII letters (`name_invalid` otherwise).
- **Uniqueness:** names are unique per realm, case-insensitively (`name_taken`). A deleted character's name stays reserved.
- **Character limit:** at most `MAX_CHARS_PER_ACCOUNT` (default 10) live characters per account per realm (`char_limit`).
- **Spawn point:** new characters start at the spawn point (default: centre of the zone, y = `SPAWN_Y`).
- **Ownership:** `CharDelete` (a soft delete) and `EnterWorld` only work on the caller's own live characters. Anything else gets `not_found`, whether the character doesn't exist, belongs to another account or is deleted.
- **Database down:** every character request answers `CharError{service_unavailable}` while the database is down.

### Movement

- **Sending moves:** the client sends `Move` with its new position. The server validates it on the next tick (20 Hz).
- **Bounds:**
  - x must be in `[0, ZONE_SIZE_X]` and z in `[0, ZONE_SIZE_Z]`.
  - y is clamped to `[MIN_Y, MAX_Y]` and does not count toward speed.
- **Speed:**
  - **Budget:** distance on the x/z plane is paid from a budget. It refills at `MAX_SPEED + SPEED_TOLERANCE` m/s (default 10 + 1) of server time and is capped at `MOVE_BURST_SECONDS` (default 1 s) of travel.
  - **Effect:** packet timing jitter is absorbed, but sustained speed can never exceed the limit.
  - **Client clock:** `clientTimeMs` is informational. The server only trusts its own clock.
- **Rejected moves:** the move is dropped, the client gets one `Correction` per tick with its last valid position, and observers see nothing.
- **Accepted moves:** go to observers in range as `EntityMove`, never back to the mover.

### Interest management

- **Grid:** a uniform grid with `AOI_CELL_SIZE` m cells (default 64). A player sees every other player and every NPC within `AOI_VIEW_RADIUS` m (default 128) on the x/z plane.
- **Updates:** each tick, a player gets `Spawn` for anyone who came into range and `Despawn` for anyone who left, logged out or (an NPC) was destroyed.
- **NPCs:** server-side entities that Lua systems create, move and destroy (README, "Lua components and systems"). Their `Spawn` has `kind` 1 and the `template` the system chose; a client shows them however it likes (the engine clones the scene entity of that name). Their moves arrive as `EntityMove`, like a player's. NPCs see nothing themselves.
- **Entity ids:** `entityId` is valid only while the entity exists. A later entity may reuse its slot with a different id.

### Say chat

- **Delivery:** `Say` goes to the sender and to everyone in range as `ChatMsg{fromName, text}`.
- **Size:** the text must be valid UTF-8 and at most `CHAT_MAX_BYTES` (default 256) bytes.
- **Control characters:** C0, DEL and C1 are stripped.
- **Rate limit:** `CHAT_RATE_COUNT` per `CHAT_RATE_WINDOW_MS` (default 5 per 10 s). Rejected messages count toward it.
- **Rejected messages** (invalid, too long or over the rate) are dropped silently.

### Lua components

Game-defined Lua components (README, "Lua components and systems") reach clients as these two messages. They are
server-authoritative: there is no client-to-server component message, and a client that sends one gets
`Disconnect{unexpected_message}`.

- **`ComponentUpdate`:** the component's full value as a JSON object, in the engine's scene-file encoding:
  - fields in definition order, floats as the shortest decimal that reads back as the same `f32`, ints as integers;
  - `vec3` / `quat` as arrays `[x, y, z]` / `[x, y, z, w]`, enums by name, arrays as JSON arrays.
- **`ComponentRemove`:** the entity no longer has the component.
- **Who gets them,** by the definition's `replicate` key:
  - `"owner"`: only the owning player's session. An NPC has no owner, so nobody.
  - `"aoi"`: the owner and every session that currently sees the entity.
  - `"none"` (the default): never sent.
- **When:** at most once per component per tick (20 Hz), after the Lua systems ran, and only if the value differs from what was last sent. Unchanged components are never resent.
  - The owner gets every replicated component it has on the first tick after `EnterWorldOk`.
  - A viewer gets every `"aoi"` component of an entity on the tick that entity comes into view, after its `Spawn`. That full set replaces the tick's changes for that viewer.
- **Ordering:** for any entity, `ComponentUpdate` / `ComponentRemove` come after its `Spawn` (or the owner's `EnterWorldOk`) and never after its `Despawn`.
- **Size:** a component whose frame would pass the `u16` length limit (65535-byte payload) is not sent. The server logs an error once, until the value fits again.
- **Bandwidth:** each change is sent in full. A component that changes every tick (e.g. regenerating health) costs one message per tick for each recipient.

### Server events

A server Lua system sends a named event to clients with `kast.sendTo`, `kast.sendNearby` or `kast.broadcast` (README,
"Lua components and systems", Events). Each recipient gets one `ServerEvent`. It is server-to-client only: a client
that sends one gets `Disconnect{unexpected_message}`.

- **`name`:** the non-empty name the system chose, e.g. `player.left`. The engine publishes it as `server.<name>`.
- **`json`:** the payload as compact JSON: any plain-data value (`null` when the system sent none). Bytes in Lua
  strings that aren't UTF-8 are replaced with U+FFFD.
- **Who gets it:**
  - `sendTo(e, ...)`: the session of character `e`. An NPC has no session: nobody.
  - `sendNearby(e, ...)`: `e`'s session and every session that currently sees `e` (the `"aoi"` audience). For an NPC,
    the sessions that see it.
  - `broadcast(...)`: every player in the zone.
  - The audience is taken when the event is sent, not when the system queued it. A character no longer in the world
    gets nothing, and neither does anyone around it.
- **When:** once per tick, after `ComponentUpdate` / `ComponentRemove`, in the order the systems queued them. So a
  client already has the `Spawn` and the latest component values of any entity the event refers to. Events queued in
  `onLeave` go out at the despawn, before that character's `Despawn`; events queued in `onEnter` go out on the next tick.
- **Ordering:** no `ServerEvent` about or to an entity comes before its `Spawn` (or the owner's `EnterWorldOk`) or
  after its `Despawn`.
- **Size:** the frame must fit the `u16` length limit: `4 + name + json` bytes at most 65535. A bigger event is
  rejected by the Lua call with an error; nothing is sent.
- **Bandwidth:** each event is one message per recipient. `broadcast` reaches every player, so one call costs N
  messages in a zone of N players, and a call per player costs N².

## Codes

Used in `Disconnect.reason` (D) and `CharError.code` (E).

| Value | Name | Where | Meaning |
|---|---|---|---|
| 0 | `ok` | | |
| 1 | `protocol_mismatch` | D | `Hello.protocolVersion` is not 2 |
| 2 | `malformed` | D | Payload doesn't decode exactly (short, trailing bytes, bad enum, NaN) |
| 3 | `unknown_opcode` | D | Opcode not in the table |
| 4 | `frame_too_large` | D | Frame `length` over `MAX_FRAME_BYTES` |
| 5 | `rate_limited` | D, E | Message rate exceeded for too long; or a character request already in flight |
| 6 | `handshake_timeout` | D | No `AuthOk` within `HANDSHAKE_TIMEOUT_MS` |
| 7 | `idle_timeout` | D | Nothing received for `IDLE_TIMEOUT_MS` |
| 8 | `invalid_token` | D | Token unparsable, not RS256, unknown `kid`, bad signature, wrong `iss`/`aud`, bad claims |
| 9 | `token_expired` | D | Token `exp` passed (beyond `JWT_CLOCK_SKEW_SECONDS`) |
| 10 | `service_unavailable` | D, E | JWKS can't be fetched (D); database unavailable (E) |
| 11 | `logged_in_elsewhere` | D | The same account logged in again |
| 12 | `server_shutdown` | D | Graceful shutdown |
| 13 | `server_full` | D | `MAX_CONNECTIONS` reached |
| 14 | `unexpected_message` | D | Valid message in the wrong state, e.g. `Move` before `Auth` |
| 20 | `name_taken` | E | Name in use in this realm (any case, including deleted characters) |
| 21 | `name_invalid` | E | Not 3–16 ASCII letters |
| 22 | `char_limit` | E | Account already has `MAX_CHARS_PER_ACCOUNT` live characters |
| 23 | `not_found` | E | No such live character on this account |
| 24 | `already_in_world` | E | Character request sent while in the world |
