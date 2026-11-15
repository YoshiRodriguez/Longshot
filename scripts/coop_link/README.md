# Longshot Co-op Link

Play Longshot's co-op (the Insert menu's **Network** tab) over the internet **without port forwarding, a VPN or a paid
relay**. Co-op Link connects the PCs directly with WebRTC (free public STUN servers) and carries the game's co-op
connection through that link. Up to **4 players**: one host and 3 friends.

Ported from Navi (GPL-3.0), where it is the Online tab's Internet mode.

## Start it

Double-click `Co-op Link.cmd` (it installs `aiortc` the first time), or:

```
python -m pip install -r scripts/coop_link/requirements.txt
python scripts/coop_link/coop_link.py
```

Pick your **game folder** (the one with the game's exe and `mods`), your **name** and your **tunic**.

## Host

1. **Create invite code** and send it to a friend (Discord, any chat).
2. Paste the friend's **reply code** and press **Connect**.
3. In the game: **Insert menu → Network**: Play as **Host**, Port **43400**, then **Connection**.
4. For each more friend (up to 3): **Create invite code** again, a new invite for each. Friends who are connected stay.

## Join

1. Paste the host's **invite code**, press **Create reply code**, and send the reply back to the host.
2. Once it says *Connected to the host*: **Insert menu → Network**: Play as **Join**, Host address **127.0.0.1**,
   the Port it says (43400 unless that port was taken), then **Connection**.

Keep Co-op Link open while you play.

## When it does not connect

- **Windows Firewall:** allow Python on Private **and** Public networks when Windows asks.
- **Strict networks:** CGNAT, phone hotspots and some "symmetric" routers block direct links. There is no relay to
  fall back on: try another network.
- **Codes:** each invite works once, for one friend. Use only the newest reply.
- **Same build:** everyone must run the same Longshot build: the game turns a different build away and says so.
