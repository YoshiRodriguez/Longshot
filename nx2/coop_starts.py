"""Co-op (host/oot_coop.c, "go to a player"): every map's places Link can arrive at, from the player's own game files.

A map change in this game names the map (its banc) and a StartPos in it, an actor placed in the map with a name and a place
(host/hos_main.c oot_teleport). host/hos_main.c knows one way into 31 maps; with this list "go to a player" can travel
into any map the other player is in, and arrive at the StartPos nearest to them, then take the step to their side.

    python -I nx2/coop_starts.py <game folder>

Writes <game folder>/coop/starts.txt, a line for each StartPos: "<banc> <name> <x> <y> <z>". Made from the player's own
game files (romfs/Banc/*/*.bcett.byml.zs) and stays in the game folder: never commit or share it. Needs zstandard.
"""
import os
import re
import sys

# StartPos names a map change can safely arrive at: the plain ways in from a neighbouring map. Left out: places a story scene
# arrives at (after a boss, a warp out of a dungeon, waking from a dream, getting a stone...), which can start that scene: the
# Kokiri Forest bridge to Hyrule Field played Saria's farewell before the Deku Tree was done. A name used more than once in a
# map (EntrancePos) is left out too: a map change names one place.
SCENE = re.compile(r"(?i)after|return|warp|defeat|boss|get_|demo|dream|event|clear|escape|restart|opening|ending|"
                   r"^startpos_|LostWoodBridge_FrontOfHyruleField|"
                   r"_air$|underwater|_before$")   # (arriving in the air or under water; the place as it was before a change)
WAY_IN = re.compile(r"(?i)frontof|from|side|entrance|outsideof")

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bake_ds import romenum   # noqa: E402  (the game's zstd dictionaries and unpacking)
import byml_rw                # noqa: E402


def starts_of(data):
    """(name, x, y, z) for each StartPos actor in one banc file's data"""
    out = []
    for actor in data.get("Actors", []) if isinstance(data, dict) else []:
        if actor.get("Gyaml") != "StartPos":
            continue
        name, at = actor.get("Name"), actor.get("Translate")
        if not name or not isinstance(at, list) or len(at) != 3 or any(c in name for c in " \t\r\n"):
            continue
        out.append((name, float(at[0]), float(at[1]), float(at[2])))
    return out


def banc_of(filename):
    """KokiriForest_Child_Static.bcett.byml.zs -> KokiriForest_Child (the name the game's Sequence_CurrentBanc holds)"""
    base = filename.split(".")[0]
    for part in ("_Static", "_Dynamic"):
        if base.endswith(part):
            return base[:-len(part)]
    return None


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    game = sys.argv[1]
    romfs = os.path.join(game, "romfs")
    if not os.path.isdir(os.path.join(romfs, "Banc")):
        raise SystemExit("coop_starts: no romfs\\Banc in %s" % game)
    dicts = romenum.dictionaries(romfs)
    lines, maps, failed, found = [], set(), 0, {}
    for folder, _, files in sorted(os.walk(os.path.join(romfs, "Banc"))):
        for f in sorted(files):
            banc = banc_of(f) if f.endswith(".bcett.byml.zs") else None
            if not banc:
                continue
            try:
                data = byml_rw.read(romenum.unpack(open(os.path.join(folder, f), "rb").read(), dicts))
            except Exception:   # (a file this reader does not take: that map keeps only host/hos_main.c's way in)
                failed += 1
                continue
            found.setdefault(banc, []).extend(starts_of(data))
    for banc, places in found.items():
        counts = {}
        for name, *_ in places:
            counts[name] = counts.get(name, 0) + 1
        for name, x, y, z in places:
            if counts[name] == 1 and WAY_IN.search(name) and not SCENE.search(name):
                lines.append("%s %s %.2f %.2f %.2f" % (banc, name, x, y, z))
                maps.add(banc)
    out = os.path.join(game, "coop")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "starts.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Co-op: the places Link can arrive at in each map (nx2/coop_starts.py). Made from your game files: do not share.\n")
        f.write("\n".join(sorted(set(lines))) + "\n")
    print("co-op arrival places: %d in %d maps%s" % (len(set(lines)), len(maps), ", %d files not read" % failed if failed else ""))


if __name__ == "__main__":
    main()
