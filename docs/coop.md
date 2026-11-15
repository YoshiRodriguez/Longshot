# Co-op

**Over the internet without port forwarding:** [Co-op Link](../scripts/coop_link/README.md) (`scripts/coop_link/`)
connects the PCs directly (WebRTC, free public STUN) and carries the game's co-op connection; up to 4 players.

Two to four players, each in their own copy of the game on their own PC, play one adventure together: what one finds,
all hold; what one has done, is done for all. You see the other players as Links in your game, fight the same enemies
and can travel to each other. It is turned on in the menu's **Network** tab and is off until you connect.

This is new and **experimental**. Read [What is not done or not tested](#what-is-not-done-or-not-tested) before you
rely on it, and play on a save slot you can afford to lose: what the others find and do is written into your save.

You need your own copy of the game, as always. Nothing here comes with game files, and the two data files co-op makes
(below) are made from your own game and stay in your game folder.

## What you need

- **Windows.** The link uses Windows' sockets; on Linux the tab is there but nothing connects yet.
- **The same version of Hookshot for everyone.** The players' games only check that they speak the same co-op
  messages, not that they are the same build.
- **A network path to the host.** One player hosts, the others join the host's address on TCP port **43400** (the
  Port row changes it). On one home network that works as it is. Friends in different homes need **Radmin VPN**
  (free): the game only opens a plain TCP link, with no relay and nothing to get through a router. The steps are in
  [Linking up, step by step](#linking-up-step-by-step).
- **Co-op's two data files, made once per game folder** (and again after a rebuild that replaces the folder). The
  build does not make them yet; from a clone, with the pipeline's Python packages installed:

  ```powershell
  python -I nx2/coop_flags.py  "C:\Games\Hookshot"    # which save flags are shared: coop\flags.bin
  python -I nx2/coop_bodies.py "C:\Games\Hookshot"    # the other players' bodies: mods\00-coop
  ```

  Without `coop\flags.bin` items are still shared, but progress flags are not. Without `mods\00-coop` the other
  players are drawn as the pause menu's Link, who stands stiff and slides, and there is no PvP.

## Linking up, step by step

This is the way every session so far was played: friends in different homes, joined through Radmin VPN.

**Once, on every player's PC**

1. Build Hookshot from your own copy of the game. Everyone must be on the same version of Hookshot.
2. Make co-op's two data files (the two commands above). Do it again after a rebuild that replaces the game folder.
3. Install **Radmin VPN** (free, from radmin-vpn.com), start it, and switch it on with the large button.

**Once, for the group**

4. The host, in Radmin VPN: **Network > Create Network**. Pick a network name and a password and send both to your
   friends in private.
5. Everyone else, in Radmin VPN: **Network > Join Network**, and type that name and password. Each player now sees the
   others in the Radmin window.
6. Every player has an address in the Radmin window, a number that starts with `26.`. The one that matters is the
   **host's**. The joiners right-click the host's name there and copy the address. Treat it like a password: do not
   show it on a stream. Hookshot keeps it off the screen for that reason.

**Every time you play**

7. Everyone starts Radmin VPN first and checks that the host shows as online in it. Then start Hookshot and load your
   save (a slot you can afford to lose). With a randomizer, everyone must be on the same seed.
8. **The host goes first.** Open the menu (Insert or F1), go to **Network**, set **Play as** to **Host**, set
   **Your name**, then press Enter on **Connection**. The first time, Windows Firewall may ask about Hookshot: allow
   it, or nobody can reach you.
9. **Then each joiner.** In **Network**, set **Play as** to **Join**. On **Host address** press Enter, paste the
   host's Radmin address with Ctrl+V, and press Enter again. Leave **Port** at 43400 unless the host changed theirs.
   Set **Your name**, then press Enter on **Connection**.
10. **Status** turns to Connected and names who is there, and the top left corner of the screen says the same. After a
    moment everyone holds the same items and progress. The other players are Links with their name over their head;
    if you are in different maps, press Enter on a player's **Go to player** row.
11. PvP is off until two players both turn **PvP** on.
12. To stop, press Enter on **Connection** again, or close the game. A game always starts not connected, so steps 8
    and 9 are done each time; the name, the address and the port are remembered.

**If it does not connect**

- **Status** says why. Read it first.
- Is the host connected before the joiners try? A joiner who was early presses Enter on **Connection** again.
- Does Radmin VPN show the host as online for the joiner? If not, the trouble is in Radmin, not in Hookshot.
- Did the joiner paste the **host's** address, not their own?
- Is **Port** the same number for everyone?
- Did the host allow Hookshot when Windows Firewall asked? If the question was closed or refused, allow Hookshot under
  "Allow an app through Windows Firewall". Do not turn the firewall or the antivirus off.
- Is everyone on the same version of Hookshot?

On one home network Radmin is not needed: the joiners type the host's address on that network instead. Other virtual
networks (Tailscale, ZeroTier) or a port forwarded to the host work the same way, but have not been tried.

## Playing

Open the menu (Insert or F1) and go to **Network**.

| Row | What it does |
|---|---|
| Play as | **Host** or **Join**. One player hosts; everyone else joins |
| Host address | For Join. Press Enter, type the host's address (or paste it with Ctrl+V), press Enter again. It is kept off the screen: only the last letter typed shows |
| Port | The port the host listens on and the others connect to, 43400 unless changed. The same number for everyone |
| Your name | What the others see. Press Enter, type (letters and digits, up to 16), press Enter again. Esc leaves it as it was |
| Connection | Press Enter to connect, and again to disconnect. A game always starts not connected |
| Status | Connecting, Connected (who, and where they are), or Not connected and why |
| Go to player | One row for each other player. In the same map you are stood beside them; in another map the game loads theirs first, arriving at that map's way in nearest to them (`coop\starts.txt`, made by `nx2/coop_starts.py`; without it, the teleport's own list of 31 maps). It waits for a cutscene or a conversation to end |
| PvP | Players can hurt each other. Only between two players who both have it on. While it is on, players are also solid to each other: you cannot walk through another player (`HOS_COOP_COLLIDE=1` makes players solid without PvP too, `=0` never) |

The name and the host's address are kept in `coop.txt` in the game folder; Play as, Port and PvP in
`navi_options.ini` (`play_as`, `port`, `pvp`).

While you are connected the top left corner of the screen says so, with each other player's name and the map they are
on, and a line for a few seconds when something happens ("Ryan found Hookshot"). Each other player has their name over
their head, and how far away they are once they are 10 m or more away. A player on your map who is out of the picture (behind you, beside you, or too far to read) has their name pinned to the edge of the screen with an arrow, the way to turn to see them: a compass. None of it is drawn over the game's own menus.

### What is shared

| | |
|---|---|
| **Items** | Every lasting item: weapons, tunics, boots, songs, stones, medallions, upgrades, trade items. Heart containers, heart pieces, empty bottles and Gold Skulltula tokens, one for one. Rupees and ammunition picked up off the ground |
| **Progress** | The game's lasting save flags: doors opened, switches, people talked to, dungeons cleared. Progress only adds up: nothing is ever put back for anyone |
| **Chests and pickups** | A chest opened, a heart piece or rupee taken, a Gold Skulltula killed, a rock blown up. A chest another player opened shows open the next time you load its map |
| **Small keys** | A key found or used by one is one more or one less for all |
| **Enemies** | An enemy's life is one for all: your hit takes the same off it in every game, and it dies for everyone. It also stands in the same place for everyone (below) |

**The world** (Navi, after daurosk's oot-remake-multiplayer `docs/world.md`): placed objects are shared by their
placement hash, while players are on the same map. Broken for everyone: pots, crates, liftable and bombable rocks,
cracked walls and floors, ice pillars, beehives, spider webs, red ice, grass tufts. Taken for everyone: small keys lying
on the floor and Gold Skulltula tokens. Opened for everyone: small-key door locks (never while your Link stands at one).
Moved for everyone: push and pull blocks (with the fix that stops a block snapping back to its old place on the next
push), carried pots and rocks, rolling boulders, the Deku Tree's moving and falling floors, the Fire Temple's hammer
floors, stairs and pillars. Field grass cut with the sword or a spin attack is cut for everyone too. A player who arrives
later finds it all as the others left it. Not shared: signposts cut by the sword, the drops of a broken pot (each game
drops its own). `HOS_COOP_WORLD=0` turns the world part off. Logs: `coop: world:` and `coop: grass:`.

**Each player's own:** hearts left, what a bottle holds, the things the game itself puts back (pots, grass, shop
stock), and which age Link is.

A player who joins later is told everything the others hold and have done, and tells them the same: after a moment all
are level. Nothing is changed in your game while a cutscene plays or a map loads; it waits.

**How the others see you (Co-op Link's Tunic, or `look=` in `coop.txt`):** green (classic), white, red, blue, purple, orange, cyan, yellow,
pink, black, or Dark Link (adult; a young Link who chose him is drawn black). Your own Link stays as he is: the colour is
what the others see. It is `look=` in the game folder's `coop.txt`, said to the others with your place. Made by
`nx2/coop_looks.py` when co-op is built: the cloth's tile texture recoloured, Link's model again naming it, and a body for
each. Not recoloured yet: the parts with their own textures (the young Link's collar and cap knot, the adult's hood top and
trims stay green). Logs: `coop: look:`.

**Your own Link in your colour**: Co-op Link puts your colour's tile, made by the co-op build into
`mods/00-coop/coop_self/`, in the mod as the game's own `Tex/Angin_Tile_A_Alb` (green: none). A player who chose green is
then drawn with green bodies of their own (`Angin_Tile_0_Alb`), so they never wear your colour on your screen.

**What is in their hands** (Navi, from daurosk's oot-remake-multiplayer, GPL-3.0): each game says with its place which of
its equipment slots are in hand (0 the sword, 3 the held item, 4 the shield), and with its gear the held item's name. The
other games put the body's pieces in hand or away by the game's own change of state (0x80e54c40: 1 in hand, 2 away),
checked by the slot's request byte and asked again after 250 ms. `HOS_COOP_HANDS=0` turns it off. Logs: `coop: hands:`.

**The party panel** (Navi): at the left, under the game's hearts, each other player's name, hearts (said as `C <life> <max>`
in quarter hearts; the most is Link's Life component's, beside the life's) and where they are. Logs: `coop: hearts:`.

## How it works

For contributors. The code is `host/oot_coop.c`, with a long comment at its head and one before each part; this is the
map.

| Part | Where |
|---|---|
| The link, what is shared, the other players' bodies, enemies | `host/oot_coop.c` |
| The Network tab, the corner of the screen, the names | `gpu/vk/vk_menu.cpp` (`coop_*`, `draw_coop`, `draw_coop_labels`) |
| The option ids the two talk through | `gpu/vk/oot_coop_opts.h` |
| The lifted game's calls into co-op | `nx2/oot_patches.py` (`patch_coop_pickup`, `patch_coop_life`) |
| The runtime's calls | `host/nn_vi.c` (every frame shown: `oot_coop_frame`), `host/hos_main.c` (Link's update: `oot_coop_player`; `hos_oot_travel`), `host/oot_menu.c` (`hos_oot_menu_in_front`) |
| The data builders | `nx2/coop_flags.py`, `nx2/coop_bodies.py`, `nx2/byml_rw.py` |
| The game-free check | `tests/test_oot_coop.py` |

### The link

Text lines over TCP. The host listens; each player who joins sends `H 1 <name> <game>` and from then on plain
messages. The host passes every message on to the others as `<from> <message>` (0 is the host itself), so three or
four players need no more connections. `<game>` is a word for a changed game that all must share (a randomizer's seed,
say); this code only ever says `-`, the game as it is. The sockets live on one thread of their own; the game's side
talks to it through two queues.

| Line | Meaning |
|---|---|
| `W <id> <name> <game>` | Host to the player who joined: your number, and who hosts |
| `J <name> <game>`, `L`, `R <name>` | A player is here, left, changed their name |
| `P <map> <x> <y> <z> <yaw> <child> <busy> <pvp>` | Where I am, 30 times a second |
| `T <slot> <command>`, `Y <words> <base64>` | What my Link's animation slots play, and the animation inputs that changed |
| `I <item> <count> [q]` | I picked this up (`q`: said while levelling with a new player, not a find) |
| `F <hash> ...`, `K <key> ...` | These save flags have left the value a new file starts them with; these per-object flags are on |
| `N <hash> <change>`, `V <hash> <value>`, `M <quarters>` | A small-key count went up or down; (the host, levelling) it is this; (levelling) my heart containers and pieces |
| `D <player> <quarters>`, `O <player>` | PvP: my hit reached that player; that player's hearts ran out |
| `U <enemy> <x> <y> <z> <life>` | My hit took this much life off that enemy |
| `Q <enemy> <x> <y> <z> <distance> <place> <commands> [<inputs>]` | I lead that enemy: where it stands and what it plays, 15 times a second |

### The other players' bodies

`nx2/coop_bodies.py` makes four actors from the player's own game files, as a mod in `mods/00-coop`:
`CoopPlayer_Child` and `CoopPlayer_Adult` are the pause menu's Link (`PouchPlayer_*`) given Link's own animation graph,
his sounds and effects, and a sword and a shield; `CoopFoe_*` is the same body as something the game's combat can hit,
with the life, hit, lock-on and target parts read from Dark Link's files. A body is moved to where its player says and
plays the same animation commands with the same inputs, so it moves as its player moves. With PvP on for both, the
other player is a `CoopFoe`: your sword, seeds and arrows hit it the game's own way, and the hit is told to them.

A new actor needs entries in the game's resource size table, so the mod carries the game's table with those entries
added, and copies of `PouchPlayer`'s rows in the two actor tables.

### Items, flags and keys

`nx2/oot_patches.py` puts a call first in the pouch's add by name (`f_8112d570`): every pickup comes through it, and a
lasting or counted one is told to the others, who are given it the way the debug tools' Inventory page gives items.
Flags are not hooked: twice a second the shared ones (`coop/flags.bin`: the game's Bool flags that are kept in the save
and never reset) are compared with what they were, and one that has left the value a new file starts with is told.
The per-object flags (chests, pickups) and the dungeons' small-key counts go the same way.

### One enemy for everyone

Each game still runs its own copy of every enemy. Two things make it one enemy:

- **One life.** `nx2/oot_patches.py` puts two calls in the Life component's update (`f_804bd8c0`): one at its head, one
  where it takes life away. Life your own hit takes is told to the others (`U`), who give their copy the same damage
  through the game's own life message, so it dies there as it does to a hit of your own. The same enemy in two games
  is the same name first seen at the same place.
- **One place.** One game leads each enemy: it alone decides where the enemy goes and what it does, and says so (`Q`).
  The others move their copy there and have it play the same animation commands. The nearest player's game leads, so
  an enemy fights whoever is closest, as it does alone: a follower whose Link has been two metres nearer than the
  leader's for half a second takes over, and a leader that stops saying it is followed for 0.7 s more before the next
  game takes it from where it stands.

**Bosses** keep their leader: a boss fight is a string of states only the leader's copy is in. A follower whose own
attack hits the boss without taking life (a seed in Gohma's eye) takes the boss over, because what a boss does about
such a hit is decided by the copy that leads it: the next shot then stuns it as it does alone. It says so for six
seconds, and the leader that hears it lets go. A hit that takes life changes nothing: the life is shared.

## Switches

In `longshot_env.txt` or the environment, as the other switches
([development guide](development.md#switches-for-testing-and-debugging)). Co-op itself has no switch: it is the
Network tab's Connection row.

| Switch | What it does |
|---|---|
| `HOS_COOP_ENEMY_PLACE=0` | Enemies are not kept in one place: each game moves its own copies. Their life is still shared |
| `HOS_COOP_BOSS=0` | Bosses are left to each game (no leader, no hand-over). Their life is still shared |
| `HOS_COOP_MIRROR` | `1` (or `mirror=1` in `coop.txt` for one start): mirror mode. Your own place, moves and gear come back as another player, "Mirror", whose body stands 1.5 m to your right and does all you do: how the others see you, tested alone. Only those lines come back (no flags, items, world or hits). Logs: `coop: mirror:` |
| `HOS_COOP_CHECKS` | (Navi) The item and move checks, on unless `0`: each move your Link starts with what is equipped and what changed in his equipment user, and each move the other players' bodies were asked to play and whether they played it. Kept in the game folder's `coop_checks.txt` (new at each start) and as `coop: check:` log lines |
| `HOS_COOP_GEAR` | What the other players' bodies wear: unset puts on their sword, shield, mask, tunic and boots (read from the game's `PlayerStatus.EquipWeapon`, `EquipShield`, `EquipHeadArmor`, `EquipArmor`, `EquipBoot`; a mask, tunic or boots taken off makes the body again without it; Dark Link holds his own dark sword and shield); `basic` only the sword and shield; `0` nothing (bodies keep the sword and shield they are made with). Logs: `coop: gear:` |
| `HOS_COOP_TEST=1` | Test runs: the link also takes the test lines `G` (travel to a map, stand at a place), `A` (play an animation on the bodies), `Z` (make an actor beside Link), `E` and `S` (equipment on the bodies). Off, those lines are ignored |
| `HOS_COOP_FOLLOW=1` or `2` | With `HOS_COOP_TEST=1`: this game never leads an enemy (1), or leads only a boss it took over by its own hit (2) |
| `HOS_COOP_AUTO=host` or `join` | Test runs: connected from the start, without the Connection row |
| `HOS_COOP_PORT=<port>` | Test runs: the port, over the Port row |

## What is not done or not tested

- **Not built or run on this version of Hookshot.** Co-op was written on Hookshot v2.0.14 and v2.1.0. This port to the
  current code was checked for compile errors file by file and passes the game-free tests; it has not been built into a
  game and started.
- **Shared enemies and bosses have not been played by two real games.** Their one life, their one place, who leads
  and the hand-over were tried with one game and a stand-in for the second player (a script on the link that says what
  a second game would, a recorded fight among it).
- **What two real players have played** (over the internet, on the v2.0.14 build): shared items, flags and keys, the
  other players' bodies, and the first PvP, which went by reach. On v2.1.0 these, and PvP through the game's own
  combat (lock-on, the hit effects), were tried with one game and the stand-in only.
- **Bosses: only Gohma has been tried from the second player's side**, and only that way. Every other boss is
  untested as a follower. `HOS_COOP_BOSS=0` turns the boss part off.
- **Enemies the game makes while you play** (not placed by the map) are matched between games by their name and the
  place they first appear. Where the games make them at different places or times, each game has its own, with its own
  life.
- **Adult Link.** What only the adult has (his body as another player, his items, the adult dungeons' enemies and
  bosses) has not been checked on its own.
- **Linux.** The link is Windows-only. Elsewhere the code builds to stand-ins that do nothing.
- **The data files are made at the end of each game build** (`nx2/make_game.py`: `coop_flags.py`, `coop_bodies.py`,
  `coop_starts.py`); a step that fails is said in the build's log and the build goes on. `mods/00-coop` carries the resource size table and the two actor tables: another
  mod that carries the same files and sorts after it wins, and the bodies are then missing.
- **The game protocol has no built-in authentication or encryption.** A direct LAN connection is plain TCP. For
  remote play, use **Co-op Link** (`scripts/coop_link/`, below: WebRTC, encrypted with DTLS, invite and reply codes swapped
  privately) or a private encrypted VPN such as Tailscale. Either protects traffic in transit, but the game still trusts
  every peer it is connected to.
- **The same build:** the hello carries the build's source fingerprint; a game of another build is turned away, with a word on both sides.
- **The Port row** steps one number at a time; there is no typing a port yet.
- **PvP** tells the other player the hit and its damage; it is not judged by the player who is hit, so it trusts both
  games.
