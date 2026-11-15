"""Longshot Co-op Link: play co-op over the internet with no port forwarding, VPN or paid relay.

The game's co-op (host/oot_coop.c, the Insert menu's Network tab) is plain TCP: the host listens on 43400 and the others
connect to it. Across the internet that needs a forwarded port. Co-op Link instead connects the PCs directly with WebRTC
(free public STUN, no relay) and carries the game's TCP connection through that link (tunnel.py):

    host's game :43400 <- Co-op Link (host) <== WebRTC ==> Co-op Link (join) -> 127.0.0.1:43400 <- friend's game

Two codes are swapped with each friend (an invite from the host, a reply back), since there is no server to do it.
One host takes up to 3 friends (4 players), each with an invite of their own. From Navi's Internet tab (GPL-3.0).

    python scripts/coop_link/coop_link.py          (needs: python -m pip install -r scripts/coop_link/requirements.txt)
"""
import asyncio
import json
import os
import pathlib
import queue
import re
import socket
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from net import Session   # noqa: E402  (needs aiortc)
except ImportError as exc:
    _root = tk.Tk(); _root.withdraw()
    messagebox.showerror("Longshot Co-op Link", "Co-op Link needs aiortc. In a terminal, run:\n\n"
                         "python -m pip install -r scripts/coop_link/requirements.txt\n\n(%s)" % exc)
    sys.exit(1)

GAME_PORT = 43400          # (the game's co-op port: the Network tab's Port row)
MAX_FRIENDS = 3            # (friends one host takes: 4 players; the game itself takes up to 8)
SETTINGS = pathlib.Path(os.environ.get("APPDATA") or pathlib.Path.home()) / "LongshotCoopLink" / "settings.json"
LOOKS = [("green", "Green (classic)"), ("white", "White"), ("red", "Red"), ("blue", "Blue"), ("purple", "Purple"),
         ("orange", "Orange"), ("cyan", "Cyan"), ("yellow", "Yellow"), ("pink", "Pink"), ("black", "Black"),
         ("dark", "Dark Link (adult)")]


def free_loopback_port(preferred=GAME_PORT):
    """A port the join side can listen on at 127.0.0.1: the game's own 43400 if free, else the next free one."""
    for port in [preferred] + list(range(preferred + 1, preferred + 20)):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.bind(("127.0.0.1", port))
            return port
        except OSError:
            continue
        finally:
            s.close()
    raise OSError("No free local port was found for Co-op Link.")


def write_coop_txt(game_dir, name, host, look):
    """The game's co-op settings (host/oot_coop.c reads coop.txt at its start): the name, the address to join, the look."""
    name = re.sub(r"[^A-Za-z0-9_-]", "", name or "Player")[:16] or "Player"
    path = pathlib.Path(game_dir) / "coop.txt"
    tmp = path.with_suffix(".txt.new")
    tmp.write_text("# Written by Longshot Co-op Link for the game's Network tab.\r\nname=%s\r\nhost=%s\r\nlook=%s\r\n"
                   % (name, host, look), encoding="ascii", newline="")
    os.replace(tmp, path)
    apply_own_look(game_dir, look)


def apply_own_look(game_dir, look):
    """Your own Link in your tunic colour: the colour's tile the build made (mods/00-coop/coop_self/<colour>.bntx.zs,
    nx2/coop_looks.py) put in the co-op mod under the game's own name; green takes it away. Read at the game's start."""
    mod = pathlib.Path(game_dir) / "mods" / "00-coop"
    target = mod / "romfs" / "Tex" / "Angin_Tile_A_Alb.bntx.zs"
    colour = "black" if look == "dark" else look
    source = mod / "coop_self" / ("%s.bntx.zs" % colour)
    if colour == "green" or not source.is_file():
        if target.exists():
            target.unlink()
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    data = source.read_bytes()
    if not target.exists() or target.read_bytes() != data:
        tmp = target.with_name(target.name + ".new")
        tmp.write_bytes(data)
        os.replace(tmp, target)


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Longshot Co-op Link")
        self.geometry("760x640")
        self.minsize(640, 560)
        try:
            self.s = json.loads(SETTINGS.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            self.s = {}
        self.q = queue.Queue()
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, name="CoopLink-Async", daemon=True).start()
        self.session = None        # (the newest link: the pending invite, or the joiner's one)
        self.friends = []          # (the host's friends already connected)
        self._build()
        self.protocol("WM_DELETE_WINDOW", self._close)
        self.after(200, self._pump)

    # ---- the window ----------------------------------------------------------------------------------------------
    def _build(self):
        pad = dict(padx=10, pady=4)
        top = ttk.Frame(self); top.pack(fill="x", **pad)
        ttk.Label(top, text="Game folder").grid(row=0, column=0, sticky="w")
        self.game = tk.StringVar(value=self.s.get("game", ""))
        ttk.Entry(top, textvariable=self.game, width=60).grid(row=0, column=1, sticky="we", padx=6)
        ttk.Button(top, text="Browse", command=self._pick_game).grid(row=0, column=2)
        ttk.Label(top, text="Your name").grid(row=1, column=0, sticky="w", pady=(6, 0))
        self.name = tk.StringVar(value=self.s.get("name", "Player"))
        ttk.Entry(top, textvariable=self.name, width=20).grid(row=1, column=1, sticky="w", padx=6, pady=(6, 0))
        ttk.Label(top, text="Tunic").grid(row=2, column=0, sticky="w", pady=(6, 0))
        self.look = tk.StringVar(value=dict(LOOKS).get(self.s.get("look", "green"), LOOKS[0][1]))
        ttk.Combobox(top, textvariable=self.look, values=[t for _, t in LOOKS], state="readonly", width=20).grid(
            row=2, column=1, sticky="w", padx=6, pady=(6, 0))
        top.columnconfigure(1, weight=1)

        mode = ttk.Frame(self); mode.pack(fill="x", **pad)
        self.mode = tk.StringVar(value=self.s.get("mode", "host"))
        ttk.Radiobutton(mode, text="Host a game", value="host", variable=self.mode, command=self._mode).pack(side="left")
        ttk.Radiobutton(mode, text="Join a game", value="join", variable=self.mode, command=self._mode).pack(side="left", padx=18)
        self.status = ttk.Label(mode, text="Not connected"); self.status.pack(side="right")

        body = ttk.Frame(self); body.pack(fill="both", expand=True, **pad)
        self.host_f, self.join_f = ttk.Frame(body), ttk.Frame(body)
        ttk.Button(self.host_f, text="1. Create invite code", command=self._host_create).pack(anchor="w")
        self.h_invite = self._box(self.host_f)
        ttk.Button(self.host_f, text="Copy invite", command=lambda: self._copy(self.h_invite)).pack(anchor="e")
        ttk.Label(self.host_f, text="2. Paste your friend's reply code, then Connect").pack(anchor="w", pady=(6, 0))
        self.h_reply = self._box(self.host_f)
        ttk.Button(self.host_f, text="Connect", command=self._host_accept).pack(anchor="e")
        ttk.Label(self.host_f, text="More players: once a friend is connected, create a new invite for the next one "
                                    "(up to 3 friends, 4 players). Each friend needs their own invite.",
                  wraplength=700, foreground="#666").pack(anchor="w", pady=(4, 0))
        ttk.Label(self.join_f, text="1. Paste the host's invite code, then Create reply code").pack(anchor="w")
        self.j_invite = self._box(self.join_f)
        ttk.Button(self.join_f, text="Create reply code", command=self._join_create).pack(anchor="e")
        ttk.Label(self.join_f, text="2. Send this reply code back to the host").pack(anchor="w", pady=(6, 0))
        self.j_reply = self._box(self.join_f)
        ttk.Button(self.join_f, text="Copy reply", command=lambda: self._copy(self.j_reply)).pack(anchor="e")

        bottom = ttk.Frame(self); bottom.pack(fill="both", expand=True, **pad)
        self.players = ttk.Treeview(bottom, columns=("player", "status", "ping"), show="headings", height=4)
        for c, t, w in (("player", "Player", 220), ("status", "Status", 140), ("ping", "Ping", 80)):
            self.players.heading(c, text=t); self.players.column(c, width=w, anchor="w")
        self.players.pack(fill="x")
        self.logbox = tk.Text(bottom, height=8, wrap="word", state="disabled"); self.logbox.pack(fill="both", expand=True, pady=(6, 0))
        ttk.Button(bottom, text="Disconnect", command=self._disconnect).pack(anchor="e", pady=(6, 0))
        self._mode()
        self.log("Keep this window open while you play. Windows may ask to let Python through the firewall: allow it.")

    def _box(self, parent):
        t = tk.Text(parent, height=3, wrap="char"); t.pack(fill="x")
        return t

    def _mode(self):
        (self.join_f if self.mode.get() == "host" else self.host_f).pack_forget()
        (self.host_f if self.mode.get() == "host" else self.join_f).pack(fill="x")

    def _pick_game(self):
        d = filedialog.askdirectory(title="The Longshot game folder (where the game's exe and mods are)")
        if d:
            self.game.set(d); self._save()

    def _copy(self, box):
        text = box.get("1.0", "end").strip()
        if text:
            self.clipboard_clear(); self.clipboard_append(text); self.log("Copied.")

    def _set(self, box, text):
        box.delete("1.0", "end"); box.insert("1.0", text)

    def log(self, msg):
        self.logbox.config(state="normal"); self.logbox.insert("end", msg + "\n"); self.logbox.see("end")
        self.logbox.config(state="disabled")

    def _save(self):
        self.s.update(game=self.game.get(), name=self.name.get(), mode=self.mode.get(), look=self._look())
        try:
            SETTINGS.parent.mkdir(parents=True, exist_ok=True)
            SETTINGS.write_text(json.dumps(self.s, indent=2), encoding="utf-8")
        except OSError:
            pass

    def _look(self):
        return next((k for k, t in LOOKS if t == self.look.get()), "green")

    # ---- the links -----------------------------------------------------------------------------------------------
    def _run(self, coro, done):
        fut = asyncio.run_coroutine_threadsafe(coro, self.loop)
        fut.add_done_callback(lambda f: self.q.put(("done", (done, f))))

    def _new_session(self, role):
        self._save()
        self.friends = [f for f in self.friends if f.state == "connected"]
        if role == "host" and self.session and self.session.role == "host" and self.session.state == "connected":
            self.friends.append(self.session)   # (stays connected: the new invite is for the next friend)
            self.session = None
        if role == "host" and len(self.friends) >= MAX_FRIENDS:
            messagebox.showinfo("Game full", "%d friends are connected already: that is 4 players, the most." % MAX_FRIENDS)
            return None
        if role != "host":
            for f in list(self.friends):
                self._drop(f)
        if self.session:
            asyncio.run_coroutine_threadsafe(self.session.shutdown(), self.loop)
        s = Session(self.name.get() or "Player", 27015, 27016, True, None, legacy_udp=False)
        s.on_event = lambda e: self.q.put(("event", dict(e, session=s)))
        try:
            if role == "host":
                s.configure_tunnel("host", game_port=GAME_PORT)
            else:
                s.configure_tunnel("join", listen_port=free_loopback_port(GAME_PORT))
        except OSError as exc:
            messagebox.showerror("Longshot Co-op Link", str(exc))
            return None
        self.session = s
        return s

    def _drop(self, s):
        if s in self.friends:
            self.friends.remove(s)
        asyncio.run_coroutine_threadsafe(s.shutdown(), self.loop)

    def _host_create(self):
        s = self._new_session("host")
        if not s:
            return
        self._set(self.h_invite, ""); self._set(self.h_reply, "")
        self.log("Creating an invite code (a few seconds)...")

        def done(code):
            self._set(self.h_invite, code)
            self.clipboard_clear(); self.clipboard_append(code)
            self.log("Invite code created and copied: send it to your friend.")
        self._run(s.host_create(), done)

    def _host_accept(self):
        if not self.session or self.session.role != "host":
            return messagebox.showinfo("Invite first", "Create an invite code first.")
        self._run(self.session.host_accept(self.h_reply.get("1.0", "end")), lambda _: self.log("Connecting..."))

    def _join_create(self):
        code = self.j_invite.get("1.0", "end")
        s = self._new_session("join")
        if not s:
            return
        self._set(self.j_reply, "")
        self.log("Creating a reply code (a few seconds)...")

        def done(reply):
            self._set(self.j_reply, reply)
            self.clipboard_clear(); self.clipboard_append(reply)
            self.log("Reply code created and copied: send it back to the host.")
        self._run(s.join_create_reply(code), done)

    def _disconnect(self):
        for f in list(self.friends):
            self._drop(f)
        if self.session:
            asyncio.run_coroutine_threadsafe(self.session.shutdown(), self.loop)
            self.session = None
        self.status.config(text="Not connected"); self._refresh()
        self.log("Disconnected. (The game keeps running; its Network tab's Connection row disconnects it.)")

    def _connected(self, s):
        game = self.game.get().strip()
        if s.role == "join":
            port = s.tunnel.listen_port
            if game and os.path.isdir(game):
                try:
                    write_coop_txt(game, self.name.get(), "127.0.0.1", self._look())
                except OSError as exc:
                    self.log("Could not write coop.txt in the game folder (%s)." % exc)
            self.log("Connected to the host. In the game: Insert menu > Network: Play as Join, Host address 127.0.0.1, "
                     "Port %d, then Connection. (Start the game after this to have the address filled in.)" % port)
        else:
            if game and os.path.isdir(game) and not self.friends:
                try:
                    write_coop_txt(game, self.name.get(), "127.0.0.1", self._look())
                except OSError:
                    pass
            n = len(self.friends) + 1
            self.log("%s connected (%d friend%s, %d players). In the game: Insert menu > Network: Play as Host, Port %d, "
                     "then Connection (once: later friends join it by themselves)."
                     % (s.peer_name or "A friend", n, "" if n == 1 else "s", n + 1, GAME_PORT))

    # ---- events --------------------------------------------------------------------------------------------------
    def _pump(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "done":
                    done, fut = payload
                    exc = fut.exception()
                    if exc:
                        self.log("Error: %s" % exc)
                        if isinstance(exc, ValueError):
                            messagebox.showwarning("Code problem", str(exc))
                    else:
                        done(fut.result())
                else:
                    self._event(payload)
        except queue.Empty:
            pass
        self._refresh()
        self.after(500, self._pump)

    def _event(self, e):
        who = e.get("session")
        if who is not None and who is not self.session:   # (a friend connected earlier)
            if who in self.friends and e["kind"] == "state" and e["state"] in ("failed", "idle"):
                self.log("%s disconnected." % (who.peer_name or "A friend"))
                self._drop(who)
            elif who in self.friends and e["kind"] == "log":
                self.log("[%s] %s" % (who.peer_name or "friend", e["msg"]))
            return
        if e["kind"] == "log":
            self.log(e["msg"])
        elif e["kind"] == "state":
            self.status.config(text={"waiting": "Waiting", "connecting": "Connecting...", "connected": "Connected",
                                     "failed": "Connection failed"}.get(e["state"], "Not connected"))
            if e.get("msg"):
                self.log(e["msg"])
            if e.get("fatal"):
                messagebox.showwarning("Couldn't connect", e["msg"])
            if e["state"] == "connected" and who is not None:
                self._connected(who)

    def _refresh(self):
        self.players.delete(*self.players.get_children())
        self.players.insert("", "end", values=("%s (you)" % (self.name.get() or "Player"),
                                               "Host" if self.mode.get() == "host" else "Player", "-"))
        for s in self.friends + ([self.session] if self.session else []):
            snap = s.snapshot()
            if snap["state"] == "connected":
                self.players.insert("", "end", values=(snap["peer"] or "Friend", "Online",
                                                       "%s ms" % snap["ping"] if snap["ping"] is not None else "..."))
            elif snap["state"] in ("waiting", "connecting"):
                self.players.insert("", "end", values=("Waiting for a friend..." if snap["state"] == "waiting" else "Connecting...", "-", "-"))

    def _close(self):
        self._save()
        for s in self.friends + ([self.session] if self.session else []):
            try:
                asyncio.run_coroutine_threadsafe(s.shutdown(), self.loop).result(timeout=3)
            except Exception:
                pass
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
