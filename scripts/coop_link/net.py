"""Longshot Co-op Link's networking (from Navi, GPL-3.0): serverless P2P over a WebRTC data channel (free public STUN only, no relay).

Two data channels are used:
  "coop-tcp"  ordered + reliable. Carries the game's own TCP co-op stream through tunnel.TcpTunnel:
              game <-TCP 127.0.0.1-> Co-op Link <-WebRTC P2P-> friend's Co-op Link <-TCP 127.0.0.1-> friend's game
  "state"     small control channel (names + ping). Its optional UDP bridge (ports 27015/27016) is the legacy
              diagnostics path and is OFF unless legacy_udp=True.
"""
import asyncio, base64, json, re, time, zlib
from aiortc import RTCPeerConnection, RTCConfiguration, RTCIceServer, RTCSessionDescription
from tunnel import TcpTunnel

CODE_PREFIX = "LONGSHOT-"
# Free public STUN servers. aiortc only uses the first STUN server in a configuration, so Co-op Link tries them in turn
# and falls back to the next one if a PC could not discover its public address with the previous one.
STUN_SERVERS = ["stun:stun.l.google.com:19302", "stun:stun1.l.google.com:19302", "stun:stun.cloudflare.com:3478"]
STUN = STUN_SERVERS[0]                  # kept for older imports
TUNNEL_LABEL = "coop-tcp"
SAVE_LABEL = "coop-save"                # (a shared save's channel: not used by Co-op Link yet)


def encode_code(desc):
    raw = json.dumps({"s": desc.sdp, "t": desc.type}, separators=(",", ":")).encode()
    return CODE_PREFIX + base64.urlsafe_b64encode(zlib.compress(raw, 9)).decode().rstrip("=")


def decode_code(code, expect_type):
    code = "".join(code.split())
    if not code.startswith(CODE_PREFIX):
        raise ValueError("That isn't a valid code.")
    b = code[len(CODE_PREFIX):]
    try:
        d = json.loads(zlib.decompress(base64.urlsafe_b64decode(b + "=" * (-len(b) % 4))))
        desc = RTCSessionDescription(sdp=d["s"], type=d["t"])
    except Exception:
        raise ValueError("That code is damaged - copy it again, the whole thing.")
    if desc.type != expect_type:
        raise ValueError("Wrong kind of code (invite vs reply mixed up).")
    return desc


class _Bridge(asyncio.DatagramProtocol):
    def __init__(self, session):
        self.s = session

    def datagram_received(self, data, addr):
        self.s._from_mod(data)


class Session:
    def __init__(self, name="Player", in_port=27015, out_port=27016, use_stun=True,
                 on_event=None, turn_urls="", turn_username="", turn_credential="", legacy_udp=True):
        self.name, self.in_port, self.out_port = name, in_port, out_port
        self.legacy_udp = legacy_udp     # False: do not open the old UDP 127.0.0.1 bridge (co-op uses the TCP tunnel)
        self.tunnel = None               # the game's co-op tunnel (also in self.tunnels)
        self.tunnels = {}                # data-channel label -> TcpTunnel
        self.srflx = self.relay = 0
        self.use_stun = use_stun
        # Comma-separated TURN URLs; ICE checks direct and relayed candidates automatically.
        self.turn_urls = [u.strip() for u in (turn_urls or "").split(",") if u.strip()]
        self.turn_username = (turn_username or "").strip()
        self.turn_credential = turn_credential or ""
        self.on_event = on_event or (lambda e: None)
        self.pc = self.dc = self.transport = None
        self.role = None                 # "host" | "join"
        self.state = "idle"              # idle | waiting | connecting | connected | failed
        self.peer_name = None
        self.ping_ms = None
        self.tx = self.rx = 0
        self.tx_pps = self.rx_pps = 0
        self.last_mod = 0.0
        self._tasks = []
        self.connect_timeout = 30        # seconds after the host pastes the reply
        self.wait_timeout = 300          # joiner: how long to wait for the host to paste
        self._timer = None

    # ---- events -------------------------------------------------------------
    def _emit(self, kind, **kw):
        self.on_event({"kind": kind, **kw})

    def _set_state(self, st, msg=None, fatal=False):
        self.state = st
        self._emit("state", state=st, msg=msg, fatal=fatal)

    def _log(self, msg):
        self._emit("log", msg=msg)

    def _arm(self, seconds):
        self._disarm()
        self._timer = asyncio.ensure_future(self._timeout(seconds))

    def _disarm(self):
        if self._timer:
            self._timer.cancel(); self._timer = None

    async def _timeout(self, seconds):
        await asyncio.sleep(seconds)
        if self.state == "connected":
            return
        msg = (f"Couldn't connect directly after {seconds} seconds.\n"
               "Co-op Link uses STUN only (free, no relay), so this works only when both routers allow a direct "
               "peer-to-peer link. Most common causes:\n"
               "1) Windows Firewall blocked Co-op Link (Python) - when Windows asks, allow it on Private AND Public networks.\n"
               "2) One of you is behind CGNAT, a mobile/hotspot connection, or a strict 'symmetric' router. "
               "Those block direct connections and there is no relay to fall back on - try a different network "
               "(for example the other player's home Wi-Fi, not a phone hotspot).\n"
               "3) Mixed-up codes - create a NEW invite and use only the newest reply code.")
        if self.use_stun and self.srflx == 0:
            msg += "\nNote: your public address couldn't be found (STUN blocked), so internet play is unlikely."
        self.peer_name = self.ping_ms = None
        self._set_state("failed", msg, fatal=True)
        await self.close()

    def _describe(self, desc, who):
        sdp = desc.sdp
        host = len(re.findall(r" typ host", sdp))
        self.srflx = len(re.findall(r" typ srflx", sdp))
        self.relay = len(re.findall(r" typ relay", sdp))
        if self.relay:
            self._log(f"{who}: TURN relay candidate gathered")
        elif self.turn_urls:
            self._log(f"{who}: no TURN relay candidate gathered - check relay credentials and server reachability")
        if self.srflx:
            self._log(f"{who}: direct public-address candidate gathered")
        elif self.use_stun:
            self._log(f"{who}: no STUN public-address candidate; TURN may still allow a relayed connection")
        if not self.use_stun and not self.turn_urls:
            self._log(f"{who}: only local candidates available; internet connection is not configured")

    def snapshot(self):
        return {"state": self.state, "role": self.role, "name": self.name, "peer": self.peer_name,
                "ping": self.ping_ms, "tx_pps": self.tx_pps, "rx_pps": self.rx_pps,
                "mod_linked": (time.time() - self.last_mod) < 3.0 if self.last_mod else False,
                "tunnel": self.tunnel.snapshot() if self.tunnel else None}

    # ---- lifecycle ----------------------------------------------------------
    def configure_tunnel(self, role, listen_port=43400, game_port=43400, label=TUNNEL_LABEL):
        """Carry a TCP port through this session's WebRTC link (see tunnel.py): the game's co-op stream (TUNNEL_LABEL), and
        any others by their own label, each on a data channel of that name (a shared save: SAVE_LABEL)."""
        t = TcpTunnel(role, game_port=game_port, listen_port=listen_port, log=self._log)
        self.tunnels[label] = t
        if label == TUNNEL_LABEL:
            self.tunnel = t
        return t

    async def _new_pc(self, stun_url=None):
        await self.close()
        ice_servers = []
        if self.use_stun:
            ice_servers.append(RTCIceServer(urls=stun_url or STUN_SERVERS[0]))
        if self.turn_urls:
            if not self.turn_username or not self.turn_credential:
                raise ValueError("TURN server URL is set, but its username or credential is missing. Check the TURN settings.")
            for url in self.turn_urls:
                ice_servers.append(RTCIceServer(
                    urls=url,
                    username=self.turn_username,
                    credential=self.turn_credential
                ))
        cfg = RTCConfiguration(iceServers=ice_servers)
        self.pc = RTCPeerConnection(cfg)
        self.srflx = 0

        @self.pc.on("iceconnectionstatechange")
        async def _ice():
            self._log(f"ICE: {self.pc.iceConnectionState}")

        @self.pc.on("connectionstatechange")
        async def _():
            st = self.pc.connectionState
            if st == "connected":
                self._disarm()
                if self.tunnel and self.tunnel.role == "join":
                    try:
                        await self.tunnel.start()
                    except OSError as exc:
                        self._set_state("failed", f"Co-op Link connected to your friend, but could not open local port "
                                                  f"{self.tunnel.listen_port} for your game ({exc}). Close whatever uses it and try again.",
                                        fatal=True)
                        return
                for label, t in self.tunnels.items():   # (the others: a failure here is not the co-op link's)
                    if t is not self.tunnel and t.role == "join":
                        try:
                            await t.start()
                        except OSError as exc:
                            self._log(f"Could not open local port {t.listen_port} for {label} ({exc})")
                self._set_state("connected", "Connected")
            elif st in ("failed", "closed", "disconnected") and self.state in ("connecting", "connected"):
                msg = ("Direct connection failed - one of you is behind a strict NAT/CGNAT and there is no relay "
                       "(STUN-only mode). Try a different network."
                       if st == "failed" else "Friend disconnected")
                self.peer_name = None; self.ping_ms = None
                self._set_state("failed" if st == "failed" else "idle", msg)

        loop = asyncio.get_running_loop()
        if self.legacy_udp and self.transport is None:
            self.transport, _ = await loop.create_datagram_endpoint(
                lambda: _Bridge(self), local_addr=("127.0.0.1", self.in_port))
        self._tasks = [asyncio.ensure_future(self._ticker())]

    def _on_datachannel(self, ch):
        if ch.label in self.tunnels:
            self.tunnels[ch.label].attach(ch)
            return
        if ch.label in (TUNNEL_LABEL, SAVE_LABEL):   # (a tunnel this side did not set up: not the control channel)
            return
        self._attach(ch)

    def _attach(self, ch):
        self.dc = ch

        @ch.on("message")
        def on_message(msg):
            if isinstance(msg, bytes):
                self.rx += 1
                if self.transport and self.legacy_udp:
                    self.transport.sendto(msg, ("127.0.0.1", self.out_port))
                return
            try:
                m = json.loads(msg)
            except ValueError:
                return
            if "name" in m:
                self.peer_name = str(m["name"])[:24]
            if m.get("t") == "ping":
                self._send_text({"t": "pong", "ts": m["ts"], "name": self.name})
            elif m.get("t") == "pong":
                self.ping_ms = round((time.time() - m["ts"]) * 1000)

    def _send_text(self, obj):
        if self.dc and self.dc.readyState == "open":
            self.dc.send(json.dumps(obj))

    def _from_mod(self, data):
        self.last_mod = time.time()
        if self.dc and self.dc.readyState == "open" and len(data) <= 1200:
            self.dc.send(bytes(data))
            self.tx += 1

    async def _ticker(self):
        last = time.time()
        while True:
            await asyncio.sleep(1.0)
            now = time.time(); dt = max(now - last, 1e-3); last = now
            self.tx_pps, self.rx_pps = round(self.tx / dt), round(self.rx / dt)
            self.tx = self.rx = 0
            self._send_text({"t": "ping", "ts": now, "name": self.name})
            self._emit("tick", **self.snapshot())

    # ---- host ---------------------------------------------------------------
    async def host_create(self):
        self.role = "host"
        stuns = STUN_SERVERS if self.use_stun else [None]
        for i, stun in enumerate(stuns):
            await self._new_pc(stun)
            self._attach(self.pc.createDataChannel("state", ordered=False, maxRetransmits=0))
            for label, t in self.tunnels.items():
                t.attach(self.pc.createDataChannel(label, ordered=True))
            await self.pc.setLocalDescription(await self.pc.createOffer())   # waits for ICE gathering
            self._describe(self.pc.localDescription, "Invite")
            if self.srflx or not self.use_stun or i == len(stuns) - 1:
                break
            self._log(f"STUN server {stun} gave no public address - trying the next one...")
        self._set_state("waiting", "Invite ready - send it to your friend")
        return encode_code(self.pc.localDescription)

    async def host_accept(self, reply_code):
        desc = decode_code(reply_code, "answer")
        self._set_state("connecting", "Connecting...")
        self._arm(self.connect_timeout)
        await self.pc.setRemoteDescription(desc)

    # ---- joiner -------------------------------------------------------------
    async def join_create_reply(self, invite_code):
        offer = decode_code(invite_code, "offer")
        self.role = "join"
        stuns = STUN_SERVERS if self.use_stun else [None]
        for i, stun in enumerate(stuns):
            await self._new_pc(stun)
            self.pc.on("datachannel", self._on_datachannel)
            await self.pc.setRemoteDescription(offer)
            await self.pc.setLocalDescription(await self.pc.createAnswer())
            self._describe(self.pc.localDescription, "Reply")
            if self.srflx or not self.use_stun or i == len(stuns) - 1:
                break
            self._log(f"STUN server {stun} gave no public address - trying the next one...")
        self._set_state("waiting", "Reply ready - send it to the host and keep Co-op Link open until it connects")
        self._arm(self.wait_timeout)
        return encode_code(self.pc.localDescription)

    async def close(self):
        self._disarm()
        for t in self._tasks:
            t.cancel()
        self._tasks = []
        if self.pc:
            await self.pc.close()
        self.pc = self.dc = None
        self.peer_name = self.ping_ms = None
        self.tx_pps = self.rx_pps = 0

    async def shutdown(self):
        await self.close()
        for t in self.tunnels.values():
            await t.stop()
        self.tunnels, self.tunnel = {}, None
        if self.transport:
            self.transport.close(); self.transport = None
