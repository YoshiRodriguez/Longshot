"""Co-op (host/oot_coop.c): how other players look, a tunic colour each, or Dark Link (adult).

Link's green is the cloth's tile texture, Tex/Angin_Tile_A_Alb (BC3): the child's tunic and cap (Mt_Cloth) and the adult's tunic
and part of his hood (Mt_Angin) take it; the material's own numbers hold no colour. So a colour is:
  * the tile recoloured   Tex/Angin_Tile_<n>_Alb.bntx.zs   each 4x4 block's two colour endpoints made the colour at the
                          endpoint's brightness (exact: the pixels between the endpoints are mixes of them, and so are the
                          new ones). The texture's own name inside the file is the new one.
  * Link's model again     Model/CoopLook_<Age>_<colour>.Player_<Age>.bfres.zs   the same file with the tile's name changed
                          in its string table (the same length, so nothing moves): its cloth samples the new tile.
  * the bodies             CoopPlayer_<Age>_<colour> and CoopFoe_<Age>_<colour>: the co-op bodies with a ModelInfo that
                          names that model, and their rows in the actor tables.
Dark Link (adult) is his own model, Model/Player_Adult_Dark: bodies CoopPlayer_Adult_dark and CoopFoe_Adult_dark name it; a
child who chose him is drawn black. Green is the bodies as they are.
Not recoloured yet: what has its own ASTC textures (the child's collar and cap knot, the adult's hood top and trims).

The colour a player chose is "look=" in <game>\\coop.txt (Navi writes it); the game says it to the others with its place.
"""
import os, struct

TILE = "Angin_Tile_A_Alb"
# name, the tile's one-character mark (the same length as "A": nothing in the model moves), the colour at the tile's
# average brightness (sRGB)
COLOURS = [
    ("white", "1", (205, 205, 198)),
    ("red", "2", (160, 32, 30)),
    ("blue", "3", (36, 64, 165)),
    ("purple", "4", (108, 46, 150)),
    ("orange", "5", (200, 98, 24)),
    ("cyan", "6", (38, 160, 170)),
    ("yellow", "7", (196, 172, 38)),
    ("pink", "8", (212, 92, 150)),
    ("black", "9", (34, 34, 38)),
]
DARK = "dark"
GREEN_MARK = "0"   # (Navi) the green body's own copy of the game's tile: with your own colour on, the plain tile is yours
SELF = "coop_self"  # (Navi) beside the mod's romfs, where the game does not load: your own Link's tile, a file per colour
LOOKS = ["green"] + [c[0] for c in COLOURS] + [DARK]


def _565(v):
    return ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31


def _to565(r, g, b):
    c = lambda x: max(0, min(255, int(round(x))))
    r, g, b = c(r), c(g), c(b)
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)


def _lum(r, g, b):
    return 0.299 * r + 0.587 * g + 0.114 * b


def recolour_tile(raw, mark, rgb):
    """the tile texture (BNTX, BC3) recoloured to rgb and named with mark: the new file's bytes"""
    out = bytearray(raw)
    i = out.find(b"BRTI")
    fmt, = struct.unpack_from("<I", out, i + 0x1c)
    if fmt >> 8 != 0x1c: raise SystemExit("the cloth tile is not BC3 (another game version?)")
    d = out.find(b"BRTD")
    size, = struct.unpack_from("<Q", out, d + 8)
    start, end = d + 0x10, d + size
    if size < 0x1000 or end > len(out): raise SystemExit("the cloth tile's data was not found (another game version?)")
    # the tile's average brightness, over its first (largest) picture's endpoints
    tot = n = 0
    for p in range(start, min(end, start + 512 * 512), 16):
        for e in (8, 10):
            v, = struct.unpack_from("<H", out, p + e)
            tot += _lum(*_565(v)); n += 1
    avg = tot / max(n, 1) or 1.0
    for p in range(start, end - 15, 16):
        e0, e1 = struct.unpack_from("<HH", out, p + 8)
        if not e0 and not e1: continue                    # (padding between the pictures)
        f0, f1 = _lum(*_565(e0)) / avg, _lum(*_565(e1)) / avg
        n0, n1 = _to565(*(c * f0 for c in rgb)), _to565(*(c * f1 for c in rgb))
        # (the endpoints keep their order: with the first not above the second some decoders read the block as three
        # colours and black, and the cloth was speckled. Swapped, each pixel's index is swapped with them: 0<->1, 2<->3)
        if (e0 > e1) != (n0 > n1) or n0 == n1:
            if n0 == n1:
                if e0 > e1: n0, n1 = (n0, n0 - 1) if n0 else (1, 0)
                else: n1 = n0 + 1 if n0 < 0xffff else n0
            else:
                n0, n1 = n1, n0
                idx, = struct.unpack_from("<I", out, p + 12)
                struct.pack_into("<I", out, p + 12, idx ^ 0x55555555)
        struct.pack_into("<HH", out, p + 8, n0, n1)
    return _rename(out, TILE, TILE[:11] + mark + TILE[12:])


def _rename(buf, old, new):
    """every whole old name (ended by a 0) in buf is new, the same length"""
    old_b, new_b = old.encode(), new.encode()
    assert len(old_b) == len(new_b)
    buf = bytearray(buf)
    at, n = 0, 0
    while True:
        at = buf.find(old_b + b"\0", at)
        if at < 0: break
        buf[at:at + len(old_b)] = new_b
        at += len(old_b); n += 1
    if not n: raise SystemExit("%s was not found to rename (another game version?)" % old)
    return bytes(buf)


def model_copy(raw, mark):
    """Link's model (bfres), its cloth sampling the tile marked so"""
    return _rename(raw, TILE, TILE[:11] + mark + TILE[12:])


def make(game, unz, dics, out, tab, table_get, sarc_read, sarc_write, byml_rw, packs):
    """The looks' files, into the mod at out (its romfs). packs: {(kind, age): (files of the pack, param key)} for
    CoopPlayer_/CoopFoe_ Child/Adult as coop_bodies made them. Returns (size entries, [(new actor, old actor, model project,
    fmdb)])"""
    import zstandard as zstd
    romfs = os.path.join(game, "romfs")
    zs = zstd.ZstdCompressor(dict_data=zstd.ZstdCompressionDict(dics["zs.zsdic"]), level=12)
    pk = zstd.ZstdCompressor(dict_data=zstd.ZstdCompressionDict(dics["pack.zsdic"]), level=12)
    plain = zstd.ZstdCompressor(level=12)
    want, rows = [], []
    tile = unz(open(os.path.join(romfs, "Tex", TILE + ".bntx.zs"), "rb").read())
    tile_room = table_get(tab, "Tex/%s.bntx" % TILE)[1] or len(tile) + 4096
    models = {age: unz(open(os.path.join(romfs, "Model", "Player_%s.Player_%s.bfres.zs" % (age, age)), "rb").read())
              for age in ("Child", "Adult")}

    def write(rel, data, comp):
        dst = os.path.join(out, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        blob = comp.compress(data)
        if os.path.exists(dst) and open(dst, "rb").read() == blob: return
        tmp = dst + ".new"
        open(tmp, "wb").write(blob)
        os.replace(tmp, dst)

    def bodies(age, look, project, fmdb):
        info = "Component/ModelInfo/CoopLook_%s_%s.engine__component__ModelInfo.bgyml" % (age, look)
        # (the body's own ModelInfo, copied whole and its model's names changed: written fresh it lacked ModelVariationAnims,
        # and making the child's black body crashed the game, 2026-11-14)
        pfiles, pkey = packs[("CoopPlayer_", age)]
        base_ref = byml_rw.read(pfiles[pkey])["Components"]["ModelInfoRef"].lstrip("?")
        if base_ref not in pfiles: raise SystemExit("the body's ModelInfo %s is not in its pack (another game version?)" % base_ref)
        base_info = dict(byml_rw.read(pfiles[base_ref]))
        base_info["FmdbName"], base_info["ModelProjectName"] = fmdb, project
        info_data = byml_rw.write(base_info)
        for kind in ("CoopPlayer_", "CoopFoe_"):
            files, key = packs[(kind, age)]
            base = kind + age
            new = "%s_%s" % (base, look)
            new_key = key.replace(base + ".", new + ".")
            param = byml_rw.read(files[key])
            param["Components"]["ModelInfoRef"] = "?" + info
            vf = {n: v for n, v in files.items() if n != key}
            vf[new_key] = byml_rw.write(param)
            vf[info] = info_data
            pack = sarc_write(vf)
            write("Pack/Actor/%s.pack.zs" % new, pack, pk)
            base_room = table_get(tab, "Pack/Actor/%s.pack" % base)[1] or 0
            want.append(("Pack/Actor/%s.pack" % new, max(base_room, len(pack)) + len(pack) // 4 + 65536))
            want.append((new_key, len(vf[new_key]) + 16384))
            rows.append((new, base, project, fmdb))
        want.append((info, 8192))

    # (Navi) your own Link in your colour: the tile recoloured under the game's own name, one file per colour, kept in
    # mods/00-coop/coop_self/ (not loaded). Navi puts the chosen one in the mod's romfs as Tex/Angin_Tile_A_Alb.bntx.zs at
    # each start, or takes it away for green (navi/native_coop.py apply_own_look): a colour changed in Navi needs no rebuild.
    # Then the plain tile is yours, so a player who chose green is drawn with the green bodies, which have a tile of their own.
    self_dir = os.path.join(os.path.dirname(out), SELF)
    os.makedirs(self_dir, exist_ok=True)
    for look, mark, rgb in COLOURS:
        blob = plain.compress(_rename(recolour_tile(tile, mark, rgb), TILE[:11] + mark + TILE[12:], TILE))
        dst = os.path.join(self_dir, look + ".bntx.zs")
        if not os.path.exists(dst) or open(dst, "rb").read() != blob:
            open(dst + ".new", "wb").write(blob)
            os.replace(dst + ".new", dst)
    for look, mark, rgb in [("green", GREEN_MARK, None)] + COLOURS:
        tex = TILE[:11] + mark + TILE[12:]
        write("Tex/%s.bntx.zs" % tex, _rename(tile, TILE, tex) if rgb is None else recolour_tile(tile, mark, rgb), plain)
        want.append(("Tex/%s.bntx" % tex, tile_room))
        for age in ("Child", "Adult"):
            project, fmdb = "CoopLook_%s_%s" % (age, look), "Player_" + age
            data = model_copy(models[age], mark)
            write("Model/%s.%s.bfres.zs" % (project, fmdb), data, zs)
            room = table_get(tab, "Model/%s.%s.bfres" % (fmdb, fmdb))[1] or len(data) + 65536
            want.append(("Model/%s.%s.bfres" % (project, fmdb), room))
            bodies(age, look, project, fmdb)
    bodies("Adult", DARK, "Player_Adult_Dark", "Player_Adult_Dark")   # (the game's own model: nothing new to write)
    return want, rows
