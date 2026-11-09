# Multiplayer: System Link and internet play

Xbox Live no longer exists. Multiplayer instead uses the game's own **System
Link** mode, its LAN multiplayer. One player hosts a party and the others join
it. This covers co-op Campaign, Horde, Beast and private Versus. Ranked Versus
matchmaking ran on Epic's servers, which are gone, so it is not available.

Players can be on the same local network, or anywhere on the internet through
a **dedicated lobby server**, which anyone can run. Choose with **System-Link**
under **Multiplayer** in the settings app.

## Your profile

Every installation has its own player ID (XUID), generated on first start and
kept in `user/identity.toml`. Two players with the same ID cannot share a
party, so do not copy that file to another PC.

- Choose your **gamertag** in the settings app under **Multiplayer**: 1–15 letters,
  digits and single spaces, starting with a letter.
- Earlier versions used one built-in ID for everyone. On first start, saves and
  profile progress made with it are copied to your new ID. Gears signs player
  data with the owner's ID, so the copies are re-signed. The originals stay in
  `user/B13EBABEBABEBABE/` and `user/4D5308AB/profile/User/` as a backup.

## Playing on a local network

1. Set **System-Link** to **LAN** (the default).
2. In the game, open the main menu and press **Back** (Tab with keyboard
   controls) for **Party Options**, then choose **System Link**.
3. Host: choose **Create Party**. Others: **Refresh** and join the host's party
   from the System Link Browser.
4. The host chooses Campaign, Horde or another mode. Everyone in the party
   follows.

Whoever creates a party runs a small lobby server inside their game until they
leave the party; nothing has to be started or configured. Other games look for
it on the network when they browse. Allow this through a firewall if parties do
not appear:

- UDP 36360 on the PC of the player who hosts;
- broadcasts on the LAN to UDP 36360.

## Playing over the internet

1. Someone runs a lobby server (see below) and shares its address.
2. Every player sets **System-Link** to **Dedicated Server** and enters that
   address in **Dedicated server**. Use the same **Room** name; the default is
   `public`. Optionally set a **Room password**: the first player in a room sets
   it. If the server does not answer, the game behaves as in LAN mode.
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

## When a player leaves

A game that closes tells the room as it goes, so the others drop that player
within a moment. A game that dies without warning (killed, crashed, or cut off
from the network) is noticed after about 4 seconds without its keepalives. A
player who is cut off for that long and comes back has to rejoin the party.

## Running a lobby server

`gears3-lobby` is built with the game (`build/bin/gears3-lobby`,
or `gears3-lobby.exe` on Windows) and runs without the game or its data:

```sh
./build/bin/gears3-lobby               # UDP port 36360 on all interfaces
./build/bin/gears3-lobby --port 40000 --max-rooms 64
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
| `network_system_link` | `auto` | `lan`, `dedicated`, or `auto` (dedicated if a server is set; the settings app writes `lan` or `dedicated`) |
| `network_lobby_server` | empty | `host[:port]` of the dedicated server |
| `network_lan_lobby` | `true` | Host a server for a party and search the LAN; `false` with no dedicated server uses plain LAN sockets |
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
