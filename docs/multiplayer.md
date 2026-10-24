# Multiplayer: System Link and internet play

Xbox Live no longer exists. Multiplayer instead uses the game's own **System
Link** mode, its LAN multiplayer. One player hosts a party and the others join
it. This covers co-op Campaign, Horde, Beast and private Versus. Ranked Versus
matchmaking ran on Epic's servers, which are gone, so it is not available.

Players can be on the same local network, or anywhere on the internet through
a **lobby server**, which anyone can run.

## Your profile

Every installation has its own player ID (XUID), generated on first start and
kept in `user/identity.toml`. Two players with the same ID cannot share a
party, so do not copy that file to another PC.

- Choose your **gamertag** in the settings app under **Online**: 1–15 letters,
  digits and single spaces, starting with a letter.
- Earlier versions used one built-in ID for everyone. On first start, saves and
  profile progress made with it are copied to your new ID. Gears signs player
  data with the owner's ID, so the copies are re-signed. The originals stay in
  `user/B13EBABEBABEBABE/` and `user/4D5308AB/profile/User/` as a backup.

## Playing on a local network

1. Leave **Lobby server** empty.
2. In the game, open the main menu and press **Back** (Tab with keyboard
   controls) for **Party Options**, then choose **System Link**.
3. Host: choose **Create Party**. Others: **Refresh** and join the host's party
   from the System Link Browser.
4. The host chooses Campaign, Horde or another mode. Everyone in the party
   follows.

The game uses UDP ports 1000 and 14000–14001. Ports below 1024 are moved to
10000 plus the port (11000) on every system, so no administrator rights are
needed and Linux and Windows players can mix. Allow these ports through a
firewall if parties do not appear:

- UDP 11000;
- UDP 14000–14001;
- broadcasts on the LAN.

## Playing over the internet

1. Someone runs a lobby server (see below) and shares its address.
2. Every player sets **Lobby server** to that address. Use the same **Room**
   name; the default is `public`. Optionally set a **Room password**: the first
   player in a room sets it.
3. In the game, use System Link exactly as on a LAN. The room behaves like one
   local network: parties of players in your room appear in the System Link
   Browser.

Players connect **directly** when possible. Each game probes the others'
public addresses, which works through most home routers without port
forwarding. When a direct path does not work, the lobby server relays the
traffic, which adds the round trip to the server. The game log reports which
path each player uses ("direct connection to … established").

All players need the same version of this port and of the game data,
including title updates.

### Frame-rate lock

While other players are connected, the frame rate is held at 60 FPS
(**Multiplayer frame-rate lock**, default 60), so every machine runs the game
at the same speed. Your normal limit returns about 15 seconds after the last
other player disconnects. 0 disables the lock.

## Running a lobby server

`squarehole-lobby` is built with the game (`build/bin/squarehole-lobby`,
or `squarehole-lobby.exe` on Windows) and runs without the game or its data:

```sh
./build/bin/squarehole-lobby               # UDP port 36360 on all interfaces
./build/bin/squarehole-lobby --port 40000 --max-rooms 64
```

Forward the UDP port on your router to the machine running it. Players then
use your public address, or `address:port` for a port other than 36360.

- Rooms are separate: players only see others in the same room, and rooms
  never mix different games.
- A room holds up to 16 players and disappears when its last player leaves.
- Players who stop responding for 15 seconds are removed.
- The server only forwards traffic between members of a room. It keeps no
  accounts and stores nothing.
- Traffic is plain UDP: the encryption the Xbox console provided is not
  reproduced. Only play with people you trust, and use a room password for
  private games.

## Settings reference

| Setting | Default | Meaning |
|---|---|---|
| `user_gamertag` | `User` | Name shown to other players |
| `network_enabled` | `true` | Off: the game reports no network cable |
| `network_lobby_server` | empty | `host[:port]` of a lobby server; empty for LAN only |
| `network_lobby_room` | `public` | Room on the lobby server |
| `network_lobby_password` | empty | Room password |
| `network_frame_rate_lock` | `60` | FPS limit while others are connected; 0 disables |
| `network_address` | empty | Your LAN address; empty detects it |
| `network_broadcast_peers` | empty | Comma-separated addresses that get System Link broadcasts directly |

### Several copies on one PC (testing)

Give each copy its own loopback address and list the others as broadcast
peers. For example, start one copy with `--network_address=127.0.0.2
--network_broadcast_peers=127.0.0.3` and the other with
`--network_address=127.0.0.3 --network_broadcast_peers=127.0.0.2`. Use
different `user/` folders so the copies have different player IDs.

With a lobby server on the same PC (`--network_lobby_server=127.0.0.1`), no
other settings are needed.

## Not supported

- Xbox Live features: ranked matchmaking, leaderboards, achievements syncing,
  friends, invites and voice chat.

Not tested yet: joining a game that is already in a level (drop-in co-op).
Joining in the lobby before the host starts is tested.
