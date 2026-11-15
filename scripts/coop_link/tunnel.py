"""TCP-over-WebRTC tunnel for the game's native co-op protocol (Longshot Co-op Link, from Navi, GPL-3.0).

The game's co-op is plain TCP text lines (host listens on 43400, joiners connect and send `H 1 <name> <game>`).
This module carries that TCP stream across a WebRTC data channel, so two players can connect directly
(STUN only, no relay) while the game itself keeps using its normal TCP code:

    HOST PC                                                     JOIN PC
    game (host, listens :43400)                                 game (join, connects to 127.0.0.1:<port>)
         ^  TCP 127.0.0.1                                            ^  TCP 127.0.0.1
         |                                                           |
    Link host side  <==== WebRTC data channel (DTLS) ====>  Link join side (local listener)

The tunnel is protocol-agnostic: it just moves bytes. The data channel is ordered and reliable, so TCP
semantics are preserved. One data channel carries any number of TCP streams (a joiner whose game drops and
reconnects simply opens a new stream).

Frame (one data-channel message):  type (1 byte) + stream id (4 bytes, big endian) + payload
    b"O"  open a stream (join -> host: host connects to the local game)
    b"D"  data for a stream
    b"C"  close a stream
"""
import asyncio
import struct
import time

CHUNK = 15000                  # stays under the 16 KB data-channel message size every WebRTC stack accepts
HIGH_WATER = 1 << 20           # pause reading the game socket when this much is queued in the channel
HOST_CONNECT_WINDOW = 180.0    # seconds the host side keeps trying to reach its local game (it may still be starting)
_HDR = struct.Struct(">cI")


class _Stream:
    def __init__(self, sid):
        self.id = sid
        self.inq = asyncio.Queue()      # bytes from the peer; None = peer closed
        self.writer = None
        self.tasks = []
        self.closed = False


class TcpTunnel:
    """role "host": connects each incoming stream to game_host:game_port.
    role "join": listens on 127.0.0.1:listen_port and forwards each local connection to the host."""

    def __init__(self, role, game_port=43400, listen_port=43400, game_host="127.0.0.1", log=None):
        if role not in ("host", "join"):
            raise ValueError("role must be host or join")
        self.role, self.game_port, self.listen_port, self.game_host = role, game_port, listen_port, game_host
        self._log = log or (lambda m: None)
        self.ch = None
        self.server = None
        self.streams = {}
        self._next_id = 1
        self.bytes_up = self.bytes_down = 0
        self.game_connected_at = 0.0     # last time a game socket was linked through the tunnel
        self._stopping = False

    # ---- channel -----------------------------------------------------------------------------
    def attach(self, channel):
        """Use this (ordered, reliable) data channel for tunnel frames."""
        self.ch = channel

        @channel.on("message")
        def _on_message(msg):
            if isinstance(msg, (bytes, bytearray)):
                self._on_frame(bytes(msg))

        @channel.on("close")
        def _on_close():
            self._close_all("data channel closed")

    def _send(self, kind, sid, payload=b""):
        ch = self.ch
        if ch is None or ch.readyState != "open":
            return False
        ch.send(_HDR.pack(kind, sid) + payload)
        return True

    # ---- join side: local listener -------------------------------------------------------------
    async def start(self):
        """Join side: open the local listener the game will connect to. Raises OSError if the port is taken."""
        if self.role != "join" or self.server is not None:
            return
        self.server = await asyncio.start_server(self._on_local_connection, "127.0.0.1", self.listen_port)
        self._log(f"Bridge: listening on 127.0.0.1:{self.listen_port} for your game")

    async def _on_local_connection(self, reader, writer):
        if self.ch is None or self.ch.readyState != "open":
            writer.close()
            return
        sid = self._next_id
        self._next_id += 1
        st = _Stream(sid)
        st.writer = writer
        self.streams[sid] = st
        self._send(b"O", sid)
        self.game_connected_at = time.time()
        self._log("Bridge: your game connected - opening the link to your friend")
        self._spawn(st, reader)

    # ---- frames -------------------------------------------------------------------------------
    def _on_frame(self, data):
        if len(data) < _HDR.size:
            return
        kind, sid = _HDR.unpack_from(data)
        payload = data[_HDR.size:]
        self.bytes_down += len(payload)
        if kind == b"O" and self.role == "host":
            st = _Stream(sid)
            self.streams[sid] = st
            st.tasks.append(asyncio.ensure_future(self._host_open(st)))
        elif kind == b"D":
            st = self.streams.get(sid)
            if st:
                st.inq.put_nowait(payload)
        elif kind == b"C":
            st = self.streams.get(sid)
            if st:
                st.inq.put_nowait(None)

    async def _host_open(self, st):
        deadline = time.time() + HOST_CONNECT_WINDOW
        warned = False
        reader = None
        while time.time() < deadline and not self._stopping and not st.closed:
            try:
                reader, writer = await asyncio.open_connection(self.game_host, self.game_port)
                st.writer = writer
                break
            except OSError:
                if not warned:
                    warned = True
                    self._log(f"Bridge: friend is here, waiting for your game to host on port {self.game_port}...")
                await asyncio.sleep(1.0)
        if st.writer is None:
            self._log(f"Bridge: could not reach your game on 127.0.0.1:{self.game_port}. Is it running and hosting? (If it closed, it may have crashed.)")
            self._send(b"C", st.id)
            self._drop(st)
            return
        self.game_connected_at = time.time()
        self._log("Bridge: friend's game linked to yours")
        self._spawn(st, reader)

    # ---- per-stream pumps -----------------------------------------------------------------------
    def _spawn(self, st, reader):
        st.tasks.append(asyncio.ensure_future(self._up(st, reader)))
        st.tasks.append(asyncio.ensure_future(self._down(st)))

    async def _up(self, st, reader):
        """game socket -> data channel"""
        try:
            while True:
                data = await reader.read(CHUNK)
                if not data:
                    break
                while self.ch is not None and self.ch.bufferedAmount > HIGH_WATER and not st.closed:
                    await asyncio.sleep(0.01)
                if not self._send(b"D", st.id, data):
                    break
                self.bytes_up += len(data)
        except (OSError, asyncio.CancelledError):
            pass
        finally:
            self._send(b"C", st.id)
            self._drop(st)

    async def _down(self, st):
        """data channel -> game socket"""
        try:
            while True:
                data = await st.inq.get()
                if data is None or st.writer is None:
                    break
                st.writer.write(data)
                await st.writer.drain()
        except (OSError, asyncio.CancelledError):
            pass
        finally:
            self._drop(st)

    def _drop(self, st):
        if st.closed:
            return
        st.closed = True
        self.streams.pop(st.id, None)
        if st.writer is not None:
            try:
                st.writer.close()
            except Exception:
                pass
        st.inq.put_nowait(None)
        for t in st.tasks:
            if t is not asyncio.current_task():
                t.cancel()

    def _close_all(self, why=""):
        had = bool(self.streams)
        for st in list(self.streams.values()):
            self._drop(st)
        if why and had and not self._stopping:
            self._log("Bridge: " + why)

    # ---- state / shutdown -----------------------------------------------------------------------
    def snapshot(self):
        return {"streams": len(self.streams), "up": self.bytes_up, "down": self.bytes_down,
                "game_linked": bool(self.streams), "listening": self.server is not None,
                "port": self.listen_port if self.role == "join" else self.game_port}

    async def stop(self):
        self._stopping = True
        self._close_all()
        if self.server is not None:
            self.server.close()
            try:
                await self.server.wait_closed()
            except Exception:
                pass
            self.server = None
        self.ch = None
