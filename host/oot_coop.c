/* Co-op for Ocarina of Time.
 *
 * Two to four players, each in their own copy of the game, sharing one adventure: what one
 * finds, all hold; what one has done, is done for all. The F1 menu's NETWORK tab turns it on.
 * docs/coop.md is the players' page.
 *
 * ONE PLAYER HOSTS, THE OTHERS JOIN. The host's game listens on a port (43400 unless the tab
 * says otherwise); the others connect to the host's address. The host passes every message on,
 * so three or four players need nothing more. On one network that works as it is; across the
 * internet the host forwards the port, or everyone is on one virtual network (Tailscale,
 * ZeroTier, Radmin): the address is then the host's address there.
 *
 * WHAT IS SHARED
 *   items      Every pickup enters the pouch by name through f_8112d570 (oot_coop_pouch_add
 *              below, hooked in by nx2/oot_patches.py). A lasting item (a weapon, a tunic, a
 *              song, a stone, a medallion, an upgrade, a trade item) is told to the others, who
 *              are given it the way the Inventory page gives it; so is each heart container,
 *              heart piece, empty bottle and Gold Skulltula token, one for one, and the rupees
 *              and ammunition picked up off the ground. What a bottle holds and the hearts a
 *              player has left are each player's own.
 *   flags      The game's save flags (GameDataMgr's bools) that are lasting progress: which
 *              ones is decided from the game's own flag list by nx2/coop_flags.py
 *              (<game>\coop\flags.bin). Twice a second the shared ones are compared with what
 *              they were; one that left the value a new file starts it with (most start off,
 *              some on) is told to the others, where it leaves it too. Nothing is ever put
 *              back to its start for anyone: progress only adds up.
 *   objects    The flags the game keeps by a placed object's own number (Bool64bitKey: a chest
 *              opened, a rupee or a heart piece taken, a Gold Skulltula killed, a rock blown
 *              up): the lasting kind, reset type 24. The kinds the game itself puts back (pots
 *              and grass, 88; enemies, 280; shop stock, 25) are left to each player. A chest
 *              another player opened shows open the next time its map is loaded.
 *   keys       Each dungeon's small-key count (DungeonInfo.<dungeon>.SmallKeyNum): a key found
 *              or used by one is one more or one less for all.
 *   where      Thirty times a second each player says which map they are on, where they stand
 *              and which way they face; the menu and the corner of the screen show it, and
 *              each other player is drawn there as a Link of their age (co_bodies_step).
 *   enemies    An enemy's life is one for all, and it stands in one place for all: the game of
 *              the player nearest to it leads it, the others' copies follow ("one enemy for
 *              everyone", further down).
 * A player who joins later is told everything the others hold and have done, and tells them the
 * same: after a moment all are level. Key counts are then the host's.
 * Nothing is changed in a game while an event plays in it or a map loads (co_game_busy): it waits.
 *
 * THE WIRE: text lines over TCP. A joining player sends "H 1 <name> <game>" and from then on
 * plain messages; the host sends every player "<from> <message>", from 0 being the host itself.
 * <game> is a word for a changed game that all must share (a randomizer's seed); "-" is the game
 * as it is, the only one this file says.
 *   W <id> <name> <game>     host -> the player who joined: your number, and who hosts
 *   J <name> <game>          a player is here            L   a player left
 *   P <map> <x> <y> <z> <yaw> <child> <busy> <pvp>       where I am
 *   I <item> <count> [q]     I picked this up (q: said while levelling, not a find)
 *   F <hash> <hash> ...      these flags have left the value a new file starts them with (hex)
 *   K <key> <key> ...        these per-object flags are on (hex, 64 bits)
 *   N <hash> <change>        a counted value (a dungeon's small keys) went up or down by this much
 *   V <hash> <value>         (the host, levelling) that counted value is this
 *   M <quarters>             (levelling) my hearts, in quarters: containers and pieces
 *   R <name>                 I am called this now
 *   D <player> <quarters>    (PvP) my swing reached that player: so many quarter hearts
 *   T <slot> <command>       what my Link plays in an animation slot ("-": nothing)
 *   B <weapon> <shield> <head> <armour> <boots>   what my Link wears, as actor names ("-": nothing); the others' bodies put it on
 *   Y <words> <base64>       my Link's animation inputs: the words that changed
 *   U <enemy> <x y z> <life> my hit took this much of that enemy's life (its name's hash, where it was made)
 *   Q <enemy> <x y z> ...    I lead that enemy: where it stands and what it plays (co_enemy_lead_step)
 *   O <player>               (PvP) that player's hearts ran out
 *   G, A, Z, E, S            test runs only (HOS_COOP_TEST=1): travel, an animation, an actor, equipment
 *
 * THREADS: sockets live on one thread of their own (co_net). The game's side (oot_coop_frame
 * every frame shown, co_tick from Link's own update while a save is played) talks to it
 * through two queues under co.lock. Winsock is loaded by name at the first use, so the build's
 * link line is as it was.
 *
 * Windows only for now: the sockets are Winsock's. Elsewhere the entry points at the end of the
 * file do nothing, and the Network tab's Connect finds no link.
 */
#include "hos.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "hos_paths.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stddef.h>
#include "../gpu/vk/oot_cheat_items.h"
#include "../gpu/vk/oot_coop_opts.h"   /* (HOS_OPT_COOP_*: the Network tab's rows) */

#define CO_VERSION 1
#define CO_PEERS 8
#define CO_LINE 1024
#define CO_GEAR 5                 /* what a body can wear: weapon, shield, head, armour, boots */
#define CO_GEAR_NAME 40

int rx_option(int id, int otherwise);
void rx_option_set(int id, int value);

/* ── the pouch, by an item's name ────────────────────────────────────────────
   How many of an item the pouch holds: the game's own count (f_805b63e0 on the manager at 0x84e98e28). -1: not ready. */
static int co_pouch_count(Ctx *c, const char *name) {
    if (strlen(name) >= 0x60) return -1;
    const uint64_t manager = recomp_load64(c, 0x84e98e28ULL);
    A64Fn count = a64_lookup(0x805b63e0ULL);
    if (!manager || !count) return -1;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x80) & ~0xfULL;
    memcpy(A64_PTR(frame + 0x10), name, strlen(name) + 1);
    recomp_store64_sp(c, frame, frame + 0x10);
    c->x[31] = frame;
    c->x[0] = manager, c->x[1] = frame, c->x[2] = 0, c->x[3] = 0;
    count(c);
    const int n = (int)(int32_t)c->x[0];
    *c = saved;
    return n;
}

/* An item added to the pouch or taken from it, the way the Inventory page does it (hos_main.c oot_pouch_change: the
   game's own pouch edit, f_828d4a70, with an event's arguments; above 0 adds, below takes away). The edit's status,
   -1 when the game is not ready for it. */
static int co_pouch_change(Ctx *c, const char *name, int delta) {
    const uint64_t pouch_singleton = recomp_load64(c, 0x84c9e0d0ULL);
    const uint64_t state_singleton = recomp_load64(c, 0x84c9e2a8ULL);
    const uint64_t actor_db = recomp_load64(c, 0x84eb6888ULL);
    const uint64_t actor_info = recomp_load64(c, 0x84e8c060ULL);
    A64Fn edit = a64_lookup(0x828d4a70ULL);
    if (!edit || !pouch_singleton || !recomp_load64(c, pouch_singleton) ||
        !state_singleton || !recomp_load64(c, state_singleton) || !actor_db || !actor_info || strlen(name) >= 0x60) return -1;
    const Ctx saved = *c;
    const uint64_t event = (c->x[31] - 0xe0) & ~0xfULL;
    memset(A64_PTR(event), 0, 0xe0);
    memcpy(A64_PTR(event + 0x80), name, strlen(name) + 1);
    recomp_store64_sp(c, event + 0x20, event + 0x60);
    recomp_store64_sp(c, event + 0x30, event + 0x70);
    recomp_store64_sp(c, event + 0x38, event + 0x68);
    recomp_store64_sp(c, event + 0x48, event + 0x74);
    recomp_store64_sp(c, event + 0x50, event + 0x74);
    recomp_store64_sp(c, event + 0x60, event + 0x80);
    recomp_store64_sp(c, event + 0x68, 0x83f0a6d8ULL);
    recomp_store32_sp(c, event + 0x70, (uint32_t)delta);
    c->x[31] = event;
    c->x[0] = event;
    edit(c);
    const int result = (int)(recomp_load32(c, event + 0x18) >> 30);
    *c = saved;
    return result;
}
static int co_pouch_give_many(Ctx *c, const char *name, int many) { return co_pouch_change(c, name, many < 1 ? 1 : many); }
static int co_pouch_give(Ctx *c, const char *name) { return co_pouch_give_many(c, name, 1); }
static int co_pouch_take(Ctx *c, const char *name, int many) { return many > 0 ? co_pouch_change(c, name, -many) : -1; }

/* ── the lasting items ───────────────────────────────────────────────────── */
#define CO_ITEM_LABEL(label, name, kind) label,
#define CO_ITEM_NAME(label, name, kind) name,
#define CO_ITEM_KIND(label, name, kind) kind,
#define CO_NO_SECTION(name)
static const char *const co_item_label[] = {OOT_CHEAT_ITEMS(CO_NO_SECTION, CO_ITEM_LABEL)};
static const char *const co_item_name[] = {OOT_CHEAT_ITEMS(CO_NO_SECTION, CO_ITEM_NAME)};
static const unsigned char co_item_kind[] = {OOT_CHEAT_ITEMS(CO_NO_SECTION, CO_ITEM_KIND)};
#define CO_ITEMS ((int)(sizeof co_item_name / sizeof co_item_name[0]))

/* A pickup's name to its place in the list. A chest hands over a stand-in (Item_Sword_Kokiri)
   for what the inventory keeps under another name (Weapon_Sword_Kokiri, Armor_Adult_HoverBoot,
   Armor_Child_Mask_Bunny for Item_Mask_Bunny): those count as the kept item. -1: not a lasting item. */
static int co_item_index(const char *name) {
    for (int i = 0; i < CO_ITEMS; i++)
        if (!strcmp(name, co_item_name[i])) return i;
    if (strncmp(name, "Item_", 5)) return -1;
    static const char *const kept[] = {"Weapon_", "Armor_Adult_", "Armor_Child_"};
    char twin[0x70];
    for (unsigned k = 0; k < 3; k++) {
        snprintf(twin, sizeof twin, "%s%s", kept[k], name + 5);
        for (int i = 0; i < CO_ITEMS; i++)
            if (!strcmp(twin, co_item_name[i])) return i;
    }
    return -1;
}

/* What a pickup is to the others, and the name it goes by between games:
     0  each player's own (a heart picked up, a fairy, what a bottle holds, a dungeon's own items: those are save values)
     1  lasting: held or not, given once
     2  counted and shared: given one for one (heart containers and pieces, bottles, tokens, rupees and ammunition:
        what one picks up, all get) */
static int co_item_class(const char *name, char canon[0x60]) {
    canon[0] = 0;
    if (strlen(name) >= 0x60) return 0;
    if (strstr(name, "Heart_Piece")) return snprintf(canon, 0x60, "Obj_Heart_Piece"), 2;   /* (the digging hole's, the mini-game's: one and the same) */
    if (!strcmp(name, "Obj_Heart_Container")) return snprintf(canon, 0x60, "%s", name), 2;
    /* (What lies on the ground, by the Actor Ledger's names: rupees but not a puzzle's silver ones, and ammunition.
       A heart, a fairy, a magic jar fill up the one who took them: those stay one's own.) */
    if (!strncmp(name, "Obj_Rupee_", 10) && !strstr(name, "Silver")) return snprintf(canon, 0x60, "%s", name), 2;
    if (!strcmp(name, "Obj_DekuStick") || !strcmp(name, "Obj_DekuNut") || !strcmp(name, "Obj_DekuSeed") || !strcmp(name, "Obj_Bomb") ||
        !strcmp(name, "Obj_Bombchu") || !strncmp(name, "Obj_ArrowBundle_", 16)) return snprintf(canon, 0x60, "%s", name), 2;
    const int i = co_item_index(name);
    if (i >= 0) {
        snprintf(canon, 0x60, "%s", co_item_name[i]);
        return co_item_kind[i] == 0 ? 1 : 2;                /* (the counted ones: seeds, nuts, sticks, bombs, arrows, beans, bottles, tokens) */
    }
    if (!strncmp(name, "Item_Dungeon_", 13) || !strncmp(name, "Item_Bottle_", 12) || !strncmp(name, "Item_GetDemo", 12)) return 0;
    if (strncmp(name, "Item_", 5) && strncmp(name, "Weapon_", 7) && strncmp(name, "Armor_", 6)) return 0;
    snprintf(canon, 0x60, "%s", name);   /* (an item the Inventory page does not list: a trade item, a nut or stick bag) */
    return 1;
}
/* The lasting items the Inventory page does not list, by the names the pouch keeps them under
   (PouchData.<name>.StockNum in the game's flag list): said to a player who joins when held. */
static const char *const co_more_known[] = {
    "Item_EyeDrop", "Item_EyeballFrog", "Item_OddMushroom", "Weapon_Sword_BrokenGiant",
    "Item_NutBag_Small", "Item_NutBag_Medium", "Item_NutBag_Large",
    "Item_StickBag_Small", "Item_StickBag_Medium", "Item_StickBag_Large",
};

/* The name the game takes an item by, and the other spellings it may hold it under. */
static void co_item_spellings(int i, char give[0x70], char other[2][0x70]) {
    const char *name = co_item_name[i];
    other[0][0] = other[1][0] = 0;
    snprintf(give, 0x70, "%s", name);
    if (!strncmp(name, "Armor_Child_Mask_", 17)) {            /* (hos_main.c oot_mask_alias: added as Item_Mask_X) */
        snprintf(give, 0x70, "Item_Mask_%s", name + 17);
        snprintf(other[0], 0x70, "%s", name);
    } else if (!strncmp(name, "Weapon_", 7)) snprintf(other[0], 0x70, "Item_%s", name + 7);
    else if (!strncmp(name, "Armor_Adult_", 12)) snprintf(other[0], 0x70, "Item_%s", name + 12);
    else if (!strncmp(name, "Item_Gauntlet_Goron", 19)) snprintf(other[0], 0x70, "Armor_Child_Gauntlet_Goron");
}

/* ── shared state ────────────────────────────────────────────────────────── */
typedef struct {
    int used, id;
    char name[24], seed[24], map[64];
    float pos[3], yaw;
    float was[3], was_yaw;              /* the place before this one, and when each came: the body moves between them */
    int64_t seen_before;
    int child;
    int busy;                           /* in a cutscene, a conversation or a loading: not to be hit (PvP) */
    int pvp;                            /* they have PvP on */
    char look[12];                      /* (Navi) how they look: a tunic colour, "dark", or "" (green) (nx2/coop_looks.py) */
    int life, life_max;                 /* (Navi) their hearts in quarters ("C"), 0 0: not said yet */
    unsigned drawn;                     /* (Navi) their pieces in hand, a bit per equipment slot (P's last field): 0 sword, 3 held, 4 shield */
    char held[CO_GEAR_NAME];            /* (Navi) their held item's actor (B's sixth), "": none */
    int64_t seen;                       /* when a position last came */
    char gear[CO_GEAR][CO_GEAR_NAME];   /* what they wear (B); "": nothing said */
    int gear_have;
} CoPeer;

typedef struct { int from; char text[CO_LINE]; } CoEvent;

static struct {
    CRITICAL_SECTION lock;
    CRITICAL_SECTION game;              /* the game's side: oot_coop_frame and co_tick, one at a time */
    int ready;
    HANDLE thread;
    volatile LONG want, port;           /* what the menu asks for */
    volatile LONG state;                /* 0 off, 1 hosting, 2 joining (trying), 3 joined, 4 failed to listen */
    volatile LONG my_id, links;         /* my number; how many sockets are up */
    char name[24], addr[128], seed[24];
    char look[12];                      /* (Navi) how I look to the others: coop.txt's look= */
    char why[72], advice[200];          /* not connected: why, in a few words for the status; what to do, for a note */
    CoPeer peer[CO_PEERS];
    char *out; size_t out_len, out_cap; /* lines for the others (game -> net) */
    CoEvent *in; size_t in_len, in_cap; /* what came (net -> game) */
    /* me, from oot_coop_player */
    volatile int64_t player_seen;
    float pos[3], yaw;
} co;

static char co_map[64];                 /* the map I am on (co_tick) */
static int co_map_age(const char *map);   /* (Navi) */
static int co_playing;
#define CO_AS_SLOTS 12               /* (the animation: slots read of an actor, and a command name's room) */
#define CO_AS_NAME 48
static volatile LONG co_test_play;      /* (test runs) play co_test_play_name in slot co_test_play_slot on the bodies */
static char co_test_play_name[CO_AS_NAME];
static int co_test_play_slot;
static volatile LONG co_test_equip;     /* (test runs) "E <slot> <name> <kind>" and "S <mode> <state>": tried on the bodies */
static char co_test_equip_line[96];
static int64_t co_flinch_until[CO_PEERS];   /* (PvP) a body I just hit plays the hit until then, before its player's own game says so */
static int co_flinch_hard[CO_PEERS];
static char co_test_go[64];          /* (test runs: a map to travel to) */
static void co_enemy_hit(uint32_t hash, const float at[3], uint32_t taken);   /* (one enemy for everyone, below) */
static void co_enemy_led(int from, const char *text);
static void co_world_take(int from, const char *text);   /* (Navi) the world: placed objects broken, moved, cut */
static int co_world_resay;                                 /* (Navi) somebody joined: the world's state said again */
static int co_actor_move(Ctx *c, uint64_t actor, const float at[12]);
static volatile LONG co_busy_now;      /* (co_tick) an event is playing or a map is loading in my game */
static volatile LONG co_hurt;           /* (PvP) quarter hearts to come off mine (the game side takes it) */
static volatile LONG co_hurt_from;      /* (PvP) the number of the player whose swing it was */
static char co_hurt_by[24];
static volatile LONG co_test_spawn;     /* (test runs) make co_test_name beside Link */
static char co_test_name[0x60];
static int co_test_option;
static _Thread_local int co_applying;   /* an item this side is handing over: not a pickup of mine */

static int co_ok(uint64_t addr, uint64_t len) {
    if (!len || addr + len < addr) return 0;
    if (addr >= HOS_SYSTEM_BASE && addr + len <= HOS_SYSTEM_END) return 1;
    if (addr >= hos_image_base && addr + len <= hos_image_end) return 1;
    if (addr >= HOS_HEAP_BASE && addr + len <= HOS_HEAP_BASE + hos_heap_used) return 1;
    /* (The game has memory above the heap too, hos_region_alloc's: an animation command's name is there. The system region,
       the image and the heap are not all: with only those no command name is read at all, and every player
       stands in one pose. So anywhere else in the guest window: is it memory, as Windows has it?) */
    if (addr < hos_image_base || !hos_guest_range(addr, len)) return 0;
    static _Thread_local uint64_t known_from, known_to;   /* (the stretch last asked about: the next address is most often in it) */
    static _Thread_local int64_t known_at;
    const int64_t now = hos_now_ns();
    if (addr >= known_from && addr + len <= known_to && now - known_at < 50000000LL) return 1;
    MEMORY_BASIC_INFORMATION info;
    for (uint64_t at = addr; at < addr + len;) {
        if (!VirtualQuery(GP(at), &info, sizeof info) || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(info.Protect & 0xff)) return 0;
        const uint64_t done = (uint64_t)((const uint8_t *)info.BaseAddress + info.RegionSize - (const uint8_t *)GP(at));
        if (at == addr) known_from = addr & ~0xfffULL, known_to = addr + done, known_at = now;
        at += done ? done : 0x1000;
    }
    return 1;
}

/* (Navi) Every read of guest memory below this line checks that it IS memory (co_ok), not only that it lies in the guest
   window (hos_guest_range): a pointer of 0 passed that, and reading through a null link in Link's equipment slots crashed the
   game ("read 0x100000000000, guest 0", the window's base) as soon as another player was there or co-op played, 2026-11-14. */
static inline void *co_real_range(uint64_t a, uint64_t n) { return co_ok(a, n) ? GP(a) : (void *)0; }
#define hos_guest_range(a, n) co_real_range((a), (n))

static void co_game_dir(char *game, size_t cap) {
    hos_game_path_a(game, (DWORD)cap);
    char *slash = strrchr(game, '\\');
    if (!slash) slash = strrchr(game, '/');
    if (slash) *slash = 0;
}

/* (the socket thread) Not connected, and why. The status line says the few words; the advice is shown once as a note. */
static void co_fail(const char *why, const char *fmt, ...) {
    char advice[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(advice, sizeof advice, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&co.lock);
    const int said = !strcmp(co.why, why);
    snprintf(co.why, sizeof co.why, "%s", why);
    snprintf(co.advice, sizeof co.advice, "%s", said ? "" : advice);   /* (the same failure again: not said again) */
    LeaveCriticalSection(&co.lock);
    if (!said) hos_log("coop: not connected: %s", why);
}

static void co_fine(void) {
    EnterCriticalSection(&co.lock);
    co.why[0] = co.advice[0] = 0;
    LeaveCriticalSection(&co.lock);
}

/* Letters, digits and a few marks only: a name or an address travels in a line of words. */
static void co_clean(char *out, size_t cap, const char *in, const char *also) {
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) {
        const unsigned char ch = (unsigned char)*in;
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch && strchr(also, ch))) out[n++] = (char)ch;
    }
    out[n] = 0;
}

/* ── settings: <game>\coop.txt (name, the host's address); the port is the menu's own row ── */
static void co_config_path(char *path, size_t cap) {
    char game[MAX_PATH];
    co_game_dir(game, sizeof game);
    snprintf(path, cap, "%s\\coop.txt", game);
}

static int co_looks_off;   /* (Navi) the last start crashed making a colour body: plain bodies this start (co_look_guard) */
static int co_mirror;   /* (Navi) mirror mode: my own place, moves and gear come back as another player, "Mirror" (co_mirror_line) */
static void co_mirror_line(const char *line);

static void co_config_save(void) {
    char path[MAX_PATH + 16];
    co_config_path(path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "# Co-op (the F1 menu's NETWORK tab). Change these there: press Enter on the row, type, Enter again.\r\n"
               "# Or change them here while the game is closed.\r\n"
               "name=%s\r\n"
               "# Join: the host's address. Not used when hosting.\r\n"
               "host=%s\r\n"
               "# How the others see me: green, white, red, blue, purple, orange, cyan, yellow, pink, black, dark (Dark Link, adult).\r\n"
               "look=%s\r\n", co.name, co.addr, co.look[0] ? co.look : "green");
    fclose(f);
}

static void co_config_load(void) {
    char path[MAX_PATH + 16], line[256];
    snprintf(co.name, sizeof co.name, "Player");
    snprintf(co.addr, sizeof co.addr, "127.0.0.1");
    co_config_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;   /* (none yet: written when a name or an address is first typed) */
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "name=", 5)) co_clean(co.name, 17, line + 5, "_-");
        else if (!strncmp(line, "host=", 5)) co_clean(co.addr, sizeof co.addr, line + 5, "._-:");
        else if (!strncmp(line, "mirror=", 7)) co_mirror = line[7] == '1';   /* (Navi: written by Navi for one start) */
        else if (!strncmp(line, "look=", 5)) {           /* (Navi) a word of small letters */
            co_clean(co.look, sizeof co.look, line + 5, "");
            for (char *k = co.look; *k; k++) {
                if (*k >= 'A' && *k <= 'Z') *k = (char)(*k - 'A' + 'a');
                if (*k < 'a' || *k > 'z') { co.look[0] = 0; break; }
            }
        }
    }
    fclose(f);
    { const char *e = getenv("HOS_COOP_MIRROR"); if (e && e[0] == '1') co_mirror = 1; }
    {   /* (Navi) a colour body was being made when the last start ended: it crashed there, so plain bodies this time */
        char game[MAX_PATH], mark[MAX_PATH + 40], what[64] = "";
        co_game_dir(game, sizeof game);
        snprintf(mark, sizeof mark, "%s\\coop\\look_making.txt", game);
        FILE *m = fopen(mark, "rb");
        if (m) {
            if (!fgets(what, sizeof what, m)) what[0] = 0;
            fclose(m);
            remove(mark);
            what[strcspn(what, "\r\n")] = 0;
            co_looks_off = 1;
            hos_log("coop: look: the last start ended while making %s: tunic colours off for this start", what[0] ? what : "a colour body");
        }
    }
    if (co_mirror) hos_log("coop: mirror: on - my own place, moves and gear come back as another player, \"Mirror\", beside me");
    if (!co.name[0]) snprintf(co.name, sizeof co.name, "Player");
    if (!co.addr[0]) snprintf(co.addr, sizeof co.addr, "127.0.0.1");
}

/* (the word for a changed game that all players must share: none here, the game as it is) */
static void co_seed_load(void) {
    snprintf(co.seed, sizeof co.seed, "-");
}

/* ── to the menu: a text, a letter at a time (the host's calls into it carry numbers only) ── */
static void co_menu_text(int which, const char *text) {
    rx_option_set(HOS_OPT_COOP_CHAR, 0);
    for (; *text; text++) rx_option_set(HOS_OPT_COOP_CHAR, (unsigned char)*text);
    rx_option_set(HOS_OPT_COOP_TEXT, which);
}

static void co_note(const char *fmt, ...) {
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    co_menu_text(1, text);
    hos_log("coop: %s", text);
    hos_log_flush();                                   /* (rare, and the first thing asked for when it goes wrong) */
}

/* ── the queues ──────────────────────────────────────────────────────────── */
static void co_send(const char *fmt, ...) {            /* a line for the others (any thread) */
    char line[CO_LINE];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof line - 1 || !co.ready) return;
    if (co_mirror) co_mirror_line(line);                /* (Navi: before its end of line) */
    line[n] = '\n';
    EnterCriticalSection(&co.lock);
    if (co.links > 0 && co.out_len < (1u << 22)) {      /* (nobody there: nothing to say; a stuck link: not without end) */
        if (co.out_len + (size_t)n + 1 > co.out_cap) {
            const size_t cap = (co.out_cap ? co.out_cap * 2 : 8192) + (size_t)n + 1;
            char *grown = realloc(co.out, cap);
            if (grown) co.out = grown, co.out_cap = cap;
        }
        if (co.out_len + (size_t)n + 1 <= co.out_cap) memcpy(co.out + co.out_len, line, (size_t)n + 1), co.out_len += (size_t)n + 1;
    }
    LeaveCriticalSection(&co.lock);
}

static void co_event(int from, const char *text) {     /* (net thread) for the game's side */
    EnterCriticalSection(&co.lock);
    if (co.in_len == co.in_cap) {
        const size_t cap = co.in_cap ? co.in_cap * 2 : 64;
        CoEvent *grown = cap <= 65536 ? realloc(co.in, cap * sizeof *grown) : NULL;
        if (grown) co.in = grown, co.in_cap = cap;
    }
    if (co.in_len < co.in_cap) {
        co.in[co.in_len].from = from;
        snprintf(co.in[co.in_len].text, CO_LINE, "%s", text);
        co.in_len++;
    }
    LeaveCriticalSection(&co.lock);
}

#define CO_INPUTS 1024                  /* (how a player moves: see "how another player moves" below) */
static uint32_t co_in_value[CO_PEERS][CO_INPUTS];        /* (under co.lock, by the peer's slot) their Link's animation inputs, word by word */
static uint8_t co_in_have[CO_PEERS][CO_INPUTS];
static uint32_t co_in_words[CO_PEERS];                   /* how many words their graph's inputs are: ours must be as many */
static char co_cmd_name[CO_PEERS][CO_AS_SLOTS][CO_AS_NAME];   /* what their Link plays in each slot ("" nothing) */
static uint8_t co_cmd_have[CO_PEERS][CO_AS_SLOTS];

static CoPeer *co_peer(int id, int make) {             /* (under co.lock) */
    CoPeer *free_slot = NULL;
    for (int i = 0; i < CO_PEERS; i++) {
        if (co.peer[i].used && co.peer[i].id == id) return &co.peer[i];
        if (!co.peer[i].used && !free_slot) free_slot = &co.peer[i];
    }
    if (!make || !free_slot) return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = 1, free_slot->id = id;
    {   /* (another player's movement was here) */
        const size_t at = (size_t)(free_slot - co.peer);
        memset(co_in_have[at], 0, CO_INPUTS), co_in_words[at] = 0, memset(co_cmd_have[at], 0, CO_AS_SLOTS);
    }
    return free_slot;
}

static void co_peer_name(int id, char *out, size_t cap) {
    EnterCriticalSection(&co.lock);
    const CoPeer *p = co_peer(id, 0);
    snprintf(out, cap, "%s", p && p->name[0] ? p->name : "A player");
    LeaveCriticalSection(&co.lock);
}

/* ---- how another player moves ----
   What was tried first and does not work: the animation controller keeps the pose it has worked out (at +0x30: a
   count of bones at +0x48, 40 bytes a bone at +0x50), and writing another player's pose over a body's looked like
   the whole answer. It is not drawn from: with the body's own animation playing, that is worked out and drawn in one
   go and the write is lost; with it stopped, nothing is drawn afresh and the body stands as it last stood: it
   moves but floats.
   What works is to animate the body the way the game animates Link:
     * the same command in the same slot. Link's controller has nine slots (0 the body: Wait, Move, Jump, CutS1...;
       1 the upper body: CommonUpper...; 4 the face; 8 the mask), co_as_names reads what each plays, co_as_play and
       co_as_remove are the game's own calls.
     * the same inputs. Standing and walking are not clips but blends driven by values the game sets every frame
       (speed, lean, what is in hand...): the object at controller+0x40 (vtable 0x84c49818) holds them, in arrays
       it points at from +0x10..+0xc8, all inside itself (for the child's graph +0x108..+0xca0). Without them Wait
       and Move leave the body in its build pose; with Link's copied over, its legs ran with his.
   So a game says, with its place: "T <slot> <command or ->" when a slot changes (and all of them again every two
   seconds), and "Y <words> <base64>": its inputs as 32-bit words, those that changed, each a u16 number and the
   value (all of them again within four seconds). Words that hold addresses are never said or written.
   This needs a body that has Link's own animation graph: nx2/coop_bodies.py makes those actors from the
   player's game files. The pause menu's Link, which co-op used before, has a graph of menu poses only: there the
   inputs' size does not match and nothing here is applied; it stands as it did. */
static size_t co_base64(const uint8_t *in, size_t n, char *out) {
    static const char set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = set[v >> 18], out[o++] = set[v >> 12 & 63];
        if (i + 1 < n) out[o++] = set[v >> 6 & 63];
        if (i + 2 < n) out[o++] = set[v & 63];
    }
    out[o] = 0;
    return o;
}

static size_t co_unbase64(const char *in, uint8_t *out, size_t cap) {
    size_t o = 0;
    uint32_t v = 0;
    int bits = 0;
    for (; *in; in++) {
        const char ch = *in;
        const int d = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52
                    : ch == '+' ? 62 : ch == '/' ? 63 : -1;
        if (d < 0) break;
        v = v << 6 | (uint32_t)d, bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return 0;
            out[o++] = (uint8_t)(v >> bits);
        }
    }
    return o;
}

static void co_inputs_take(int from, const char *text) {    /* (the net thread) "Y <words> <base64>" */
    unsigned words = 0;
    int skip = 0;
    uint8_t raw[CO_LINE];
    if (sscanf(text, "%u %n", &words, &skip) < 1 || !words || words > CO_INPUTS) return;
    const size_t len = co_unbase64(text + skip, raw, sizeof raw);
    EnterCriticalSection(&co.lock);
    CoPeer *p = co_peer(from, 0);
    if (p) {
        const size_t at = (size_t)(p - co.peer);
        if (co_in_words[at] != words) memset(co_in_have[at], 0, CO_INPUTS), co_in_words[at] = words;   /* (another age: another graph) */
        for (size_t o = 0; o + 6 <= len; o += 6) {
            uint16_t word;
            memcpy(&word, raw + o, 2);
            if (word >= words) break;
            memcpy(&co_in_value[at][word], raw + o + 2, 4);
            co_in_have[at][word] = 1;
        }
    }
    LeaveCriticalSection(&co.lock);
}

static void co_command_take(int from, const char *text) {   /* (the net thread) "T <slot> <command or ->" */
    int slot = -1;
    char name[CO_AS_NAME] = "";
    if (sscanf(text, "%d %47s", &slot, name) != 2 || slot < 0 || slot >= CO_AS_SLOTS) return;
    EnterCriticalSection(&co.lock);
    CoPeer *p = co_peer(from, 0);
    if (p) {
        const size_t at = (size_t)(p - co.peer);
        snprintf(co_cmd_name[at][slot], CO_AS_NAME, "%s", strcmp(name, "-") ? name : "");
        co_cmd_have[at][slot] = 1;
    }
    LeaveCriticalSection(&co.lock);
}

/* ---- what a player wears: the lines in (B); the lines out and the putting on are further down ----
   A name is an actor's, as the game keeps it (Weapon_Sword_Master, Weapon_Shield_Hylian, Armor_...): only such a name
   from the net is ever handed to the game. */
static int co_gear_name_ok(const char *s) {
    if (strlen(s) >= CO_GEAR_NAME) return 0;
    if (strncmp(s, "Weapon_", 7) && strncmp(s, "Armor_", 6) && strncmp(s, "Item_", 5)) return 0;
    for (; *s; s++) {
        const char ch = *s;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-')) return 0;
    }
    return 1;
}

static void co_gear_take(int from, const char *text) {      /* (the net thread) "B <weapon> <shield> <head> <armour> <boots>" */
    char raw[CO_GEAR][CO_GEAR_NAME + 1], mine[CO_GEAR][CO_GEAR_NAME];
    memset(raw, 0, sizeof raw), memset(mine, 0, sizeof mine);
    char held_raw[CO_GEAR_NAME + 1] = "", held[CO_GEAR_NAME] = "";
    const int got = sscanf(text, "%40s %40s %40s %40s %40s %40s", raw[0], raw[1], raw[2], raw[3], raw[4], held_raw);   /* (one more than a name can be: a longer one is refused, not cut) */
    if (got >= 6 && strcmp(held_raw, "-") && co_gear_name_ok(held_raw)) snprintf(held, sizeof held, "%.39s", held_raw);   /* (Navi) */
    if (got < 1) return;
    for (int k = 0; k < got && k < CO_GEAR; k++)
        if (strcmp(raw[k], "-") && co_gear_name_ok(raw[k])) snprintf(mine[k], CO_GEAR_NAME, "%.39s", raw[k]);
    EnterCriticalSection(&co.lock);
    CoPeer *p = co_peer(from, 0);
    if (p) memcpy(p->gear, mine, sizeof mine), p->gear_have = 1, snprintf(p->held, sizeof p->held, "%s", held);
    LeaveCriticalSection(&co.lock);
}

/* What every side does with a line from player `from` (the net thread): positions are kept
   here, the comings and goings set the table, the rest is the game side's. */
static void co_take(int from, const char *text) {
    if (text[0] == 'Y' && text[1] == ' ') { co_inputs_take(from, text + 2); return; }
    if (text[0] == 'T' && text[1] == ' ') { co_command_take(from, text + 2); return; }
    if (text[0] == 'B' && text[1] == ' ') { co_gear_take(from, text + 2); return; }
    if (text[0] == 'C' && text[1] == ' ') {             /* (Navi) a player's hearts: "C <life> <max>", in quarter hearts */
        int life = 0, most = 0;
        if (sscanf(text + 2, "%d %d", &life, &most) == 2 && life >= 0 && most >= 0 && most <= 400 && life <= 400) {
            EnterCriticalSection(&co.lock);
            CoPeer *p = co_peer(from, 0);
            if (p) p->life = life, p->life_max = most;
            LeaveCriticalSection(&co.lock);
        }
        return;
    }
    if (text[0] == 'Q' && text[1] == ' ') { co_enemy_led(from, text + 2); return; }
    if (text[0] == 'P' && text[1] == ' ') {
        char map[64] = "";
        float x = 0, y = 0, z = 0, yaw = 0;
        int child = 1, busy = 0, pvp = 0;
        char look[12] = "";
        unsigned drawn = 0;
        if (sscanf(text + 2, "%63s %f %f %f %f %d %d %d %11s %u", map, &x, &y, &z, &yaw, &child, &busy, &pvp, look, &drawn) < 5) return;
        for (char *k = look; *k; k++) if (!(*k >= 'a' && *k <= 'z')) { look[0] = 0; break; }   /* (Navi: a word of small letters, or none) */
        EnterCriticalSection(&co.lock);
        CoPeer *p = co_peer(from, 0);
        char pvp_who[24] = "";
        if (p && !!p->pvp != !!pvp && p->seen) snprintf(pvp_who, sizeof pvp_who, "%s", p->name);   /* (Navi) their PvP switched */
        if (p) {
            const int moved_map = strcmp(p->map, map) != 0;
            snprintf(p->map, sizeof p->map, "%s", map);
            memcpy(p->was, p->pos, sizeof p->was), p->was_yaw = p->yaw, p->seen_before = p->seen;
            {   /* (Navi) a map with an age says theirs, whatever their game said: their body is never the other age there */
                const int age = co_map_age(map);
                if (age >= 0 && age != child) {
                    static int said;
                    if (said++ < 5) hos_log("coop: age: %s said %s on %s: drawn %s", p->name, child ? "young" : "adult", map, age ? "young" : "adult");
                    child = age;
                }
                if (p->seen && p->child != child) hos_log("coop: age: %s is %s now", p->name, child ? "young Link" : "adult Link");
            }
            p->pos[0] = x, p->pos[1] = y, p->pos[2] = z, p->yaw = yaw, p->child = child, p->busy = busy, p->pvp = pvp;
            snprintf(p->look, sizeof p->look, "%s", look);
            p->drawn = drawn & 0xff;
            p->seen = hos_now_ns();
            if (moved_map || !p->seen_before) memcpy(p->was, p->pos, sizeof p->was), p->was_yaw = yaw, p->seen_before = p->seen;
        }
        LeaveCriticalSection(&co.lock);
        if (pvp_who[0]) {
            co_note("%s turned PvP %s", pvp_who, pvp ? "on" : "off");
            hos_log("coop: pvp: %s turned PvP %s", pvp_who, pvp ? "on" : "off");
        }
        return;
    }
    if (text[0] == 'R' && text[1] == ' ') {              /* a player took another name */
        EnterCriticalSection(&co.lock);
        CoPeer *p = co_peer(from, 0);
        if (p) co_clean(p->name, 17, text + 2, "_-");
        LeaveCriticalSection(&co.lock);
        return;
    }
    if (text[0] == 'J' && text[1] == ' ') {
        char name[64] = "", seed[64] = "-";
        sscanf(text + 2, "%63s %63s", name, seed);
        EnterCriticalSection(&co.lock);
        CoPeer *p = co_peer(from, 1);
        if (p) co_clean(p->name, 17, name, "_-"), co_clean(p->seed, 17, seed, "_-");
        LeaveCriticalSection(&co.lock);
    }
    if (text[0] == 'L' && !text[1]) {                  /* (their name goes with it: the table forgets them here) */
        char left[48] = "L A player";
        EnterCriticalSection(&co.lock);
        CoPeer *p = co_peer(from, 0);
        if (p) { if (p->name[0]) snprintf(left, sizeof left, "L %s", p->name); p->used = 0; }
        LeaveCriticalSection(&co.lock);
        co_event(from, left);
        return;
    }
    co_event(from, text);
}

/* (Navi) Mirror mode: test the other players' look alone. The lines that draw a player (its place P, its moves T and Y, its
   gear B) are taken as from player 900, "Mirror": its body stands 1.5 m to my right and does all I do, as another player's
   body would on a friend's screen; what it plays and holds goes into the checks (coop_checks.txt). Nothing else comes
   back: no flags, items, world, hits. */
#define CO_MIRROR_ID 900
static void co_mirror_line(const char *line) {
    static int busy, joined;
    if (busy || !line[0] || line[1] != ' ' || !strchr("PTYBC", line[0])) return;
    busy = 1;
    if (!joined) joined = 1, co_take(CO_MIRROR_ID, "J Mirror -");
    if (line[0] == 'P') {
        char map[64] = "", look[12] = "-";
        float x = 0, y = 0, z = 0, yaw = 0;
        int child = 1, b2 = 0, pvp = 0;
        unsigned drawn = 0;
        if (sscanf(line + 2, "%63s %f %f %f %f %d %d %d %11s %u", map, &x, &y, &z, &yaw, &child, &b2, &pvp, look, &drawn) >= 5) {
            char moved[CO_LINE];
            /* (Link faces sin yaw, cos yaw; his right is cos yaw, -sin yaw) */
            snprintf(moved, sizeof moved, "P %s %.2f %.2f %.2f %.3f %d %d %d %s %u", map, x + 1.5f * cosf(yaw), y, z - 1.5f * sinf(yaw),
                     yaw, child, b2, pvp, look, drawn);
            co_take(CO_MIRROR_ID, moved);
        }
    } else {
        co_take(CO_MIRROR_ID, line);
    }
    busy = 0;
}

/* ── the sockets (their own thread) ──────────────────────────────────────── */
static struct {
    HMODULE dll;
    int (WSAAPI *startup)(WORD, LPWSADATA);
    SOCKET (WSAAPI *socket_)(int, int, int);
    int (WSAAPI *bind_)(SOCKET, const struct sockaddr *, int);
    int (WSAAPI *listen_)(SOCKET, int);
    SOCKET (WSAAPI *accept_)(SOCKET, struct sockaddr *, int *);
    int (WSAAPI *connect_)(SOCKET, const struct sockaddr *, int);
    int (WSAAPI *send_)(SOCKET, const char *, int, int);
    int (WSAAPI *recv_)(SOCKET, char *, int, int);
    int (WSAAPI *close_)(SOCKET);
    int (WSAAPI *ioctl_)(SOCKET, long, u_long *);
    int (WSAAPI *setopt_)(SOCKET, int, int, const char *, int);
    int (WSAAPI *getopt_)(SOCKET, int, int, char *, int *);
    int (WSAAPI *select_)(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
    int (WSAAPI *getaddr_)(PCSTR, PCSTR, const ADDRINFOA *, PADDRINFOA *);
    void (WSAAPI *freeaddr_)(PADDRINFOA);
    int (WSAAPI *error_)(void);
} ws;

static int co_ws_load(void) {
    if (ws.dll) return ws.error_ != NULL;
    ws.dll = LoadLibraryA("ws2_32.dll");
    if (!ws.dll) return 0;
#define CO_WS(member, name) *(FARPROC *)&ws.member = GetProcAddress(ws.dll, name)
    CO_WS(startup, "WSAStartup"); CO_WS(socket_, "socket"); CO_WS(bind_, "bind"); CO_WS(listen_, "listen");
    CO_WS(accept_, "accept"); CO_WS(connect_, "connect"); CO_WS(send_, "send"); CO_WS(recv_, "recv");
    CO_WS(close_, "closesocket"); CO_WS(ioctl_, "ioctlsocket"); CO_WS(setopt_, "setsockopt"); CO_WS(getopt_, "getsockopt"); CO_WS(select_, "select");
    CO_WS(getaddr_, "getaddrinfo"); CO_WS(freeaddr_, "freeaddrinfo"); CO_WS(error_, "WSAGetLastError");
#undef CO_WS
    WSADATA data;
    if (!ws.startup || !ws.socket_ || !ws.bind_ || !ws.listen_ || !ws.accept_ || !ws.connect_ || !ws.send_ || !ws.recv_ ||
        !ws.close_ || !ws.ioctl_ || !ws.setopt_ || !ws.getopt_ || !ws.select_ || !ws.getaddr_ || !ws.freeaddr_ || !ws.error_ ||
        ws.startup(MAKEWORD(2, 2), &data)) { ws.error_ = NULL; return 0; }
    return 1;
}

typedef struct {
    SOCKET s;
    int id, hello;                      /* (host) the player's number; whether their H line came */
    char name[24], seed[24];
    char in[CO_LINE * 2]; int in_len;
    char *out; size_t out_len, out_cap;
} CoLink;

static CoLink co_link[CO_PEERS];        /* host: the players who joined; joined: [0] is the host */
static SOCKET co_listen = INVALID_SOCKET;
static int co_next_id = 1;

static void co_link_put(CoLink *l, const char *text, size_t n) {
    if (l->s == INVALID_SOCKET || l->out_len > (1u << 22)) return;
    if (l->out_len + n > l->out_cap) {
        const size_t cap = (l->out_cap ? l->out_cap * 2 : 8192) + n;
        char *grown = realloc(l->out, cap);
        if (!grown) return;
        l->out = grown, l->out_cap = cap;
    }
    memcpy(l->out + l->out_len, text, n);
    l->out_len += n;
}

static void co_link_say(CoLink *l, const char *fmt, ...) {
    char line[CO_LINE + 32];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof line - 1) return;
    line[n] = '\n';
    co_link_put(l, line, (size_t)n + 1);
}

static void co_links_count(void) {
    int n = 0;
    for (int i = 0; i < CO_PEERS; i++) n += co_link[i].s != INVALID_SOCKET && (co.state != 1 || co_link[i].hello);
    InterlockedExchange(&co.links, n);
}

static void co_link_close(CoLink *l) {
    if (l->s == INVALID_SOCKET) return;
    ws.close_(l->s);
    l->s = INVALID_SOCKET;
    free(l->out);
    l->out = NULL, l->out_len = l->out_cap = 0, l->in_len = 0;
    if (co.state == 1 && l->hello) {                   /* (host) the others hear it, and so does this game */
        char line[32];
        const int n = snprintf(line, sizeof line, "%d L\n", l->id);
        for (int i = 0; i < CO_PEERS; i++)
            if (&co_link[i] != l && co_link[i].hello) co_link_put(&co_link[i], line, (size_t)n);
        co_take(l->id, "L");
    }
    l->hello = 0;
    co_links_count();
}

static void co_close_all(void) {
    for (int i = 0; i < CO_PEERS; i++) {
        if (co_link[i].s == INVALID_SOCKET) continue;
        ws.close_(co_link[i].s);
        co_link[i].s = INVALID_SOCKET;
        free(co_link[i].out);
        co_link[i].out = NULL, co_link[i].out_len = co_link[i].out_cap = 0, co_link[i].in_len = 0, co_link[i].hello = 0;
    }
    if (co_listen != INVALID_SOCKET) ws.close_(co_listen), co_listen = INVALID_SOCKET;
    EnterCriticalSection(&co.lock);
    for (int i = 0; i < CO_PEERS; i++) {
        if (!co.peer[i].used) continue;
        co.peer[i].used = 0;
    }
    co.out_len = 0;
    LeaveCriticalSection(&co.lock);
    InterlockedExchange(&co.links, 0);
    InterlockedExchange(&co.my_id, 0);
}

static void co_nonblocking(SOCKET s) {
    u_long on = 1;
    ws.ioctl_(s, FIONBIO, &on);
    const int nodelay = 1;
    ws.setopt_(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
}

/* (Navi) which build this game is: the 8 hex digits of its sources' fingerprint (nx2/version.py, "... (source 81f8fa58)").
   Said in the hello and the welcome: a game of another update is turned away with a word on both sides, never let in to
   play with code the other has fixed (an outdated joiner kept crashing in what the host's update had fixed, 2026-11-14) */
const char *hos_version(void);   /* hos_main.c */
static const char *co_build(void) {
    static char build[16];
    if (!build[0]) {
        const char *at = strstr(hos_version(), "(source ");
        size_t n = 0;
        if (at) for (at += 8; n < sizeof build - 1 && ((at[n] >= '0' && at[n] <= '9') || (at[n] >= 'a' && at[n] <= 'f')); n++) build[n] = at[n];
        build[n] = 0;
        if (!n) snprintf(build, sizeof build, "unknown");
    }
    return build;
}
#define CO_OTHER_BUILD "this game and the host's are different Longshot builds: both players build the same Longshot, then join again"

/* (host) a line from the player on link l */
static void co_host_line(CoLink *l, char *text) {
    if (!l->hello) {
        int version = 0;
        char name[64] = "", seed[64] = "-", build[16] = "";
        if (sscanf(text, "H %d %63s %63s %15s", &version, name, seed, build) >= 2 && version == CO_VERSION &&
            strcmp(build, co_build())) {   /* (Navi: another update's game; one from before this check says no build) */
            co_link_say(l, "0 X %s", CO_OTHER_BUILD);
            hos_log("coop: %s turned away: another Longshot build (theirs %s, mine %s)", name, build[0] ? build : "older", co_build());
            co_note("%s tried to join from a different Longshot build: both build the same one", name);
            l->in_len = -1;
            return;
        }
        if (sscanf(text, "H %d %63s %63s", &version, name, seed) < 2 || version != CO_VERSION) {
            co_link_say(l, "0 X %s", version != CO_VERSION && version ? "this game and the host's are different co-op versions" : "not a co-op hello");
            hos_log("coop: a connection was turned away (%.40s)", text);
            l->in_len = -1;                            /* (closed once its goodbye is out) */
            return;
        }
        l->hello = 1, l->id = co_next_id++;
        co_clean(l->name, 17, name, "_-");
        co_clean(l->seed, 17, seed, "_-");
        if (!l->name[0]) snprintf(l->name, sizeof l->name, "Player%d", l->id);
        co_link_say(l, "0 W %d %s %s %s", l->id, co.name, co.seed, co_build());
        for (int i = 0; i < CO_PEERS; i++) {
            CoLink *o = &co_link[i];
            if (o == l || o->s == INVALID_SOCKET || !o->hello) continue;
            co_link_say(l, "%d J %s %s", o->id, o->name, o->seed);
            co_link_say(o, "%d J %s %s", l->id, l->name, l->seed);
        }
        char joined[96];
        snprintf(joined, sizeof joined, "J %s %s", l->name, l->seed);
        co_take(l->id, joined);
        co_links_count();
        return;
    }
    if (text[0] == 'H' || text[0] == 'W' || text[0] == 'J' || text[0] == 'L' || text[0] == 'X') return;   /* (the host's own to say) */
    if (text[0] == 'R' && text[1] == ' ') co_clean(l->name, 17, text + 2, "_-");   /* (their new name, for players who join later) */
    char line[CO_LINE + 32];
    const int n = snprintf(line, sizeof line, "%d %s\n", l->id, text);
    for (int i = 0; i < CO_PEERS; i++)
        if (&co_link[i] != l && co_link[i].s != INVALID_SOCKET && co_link[i].hello) co_link_put(&co_link[i], line, (size_t)n);
    co_take(l->id, text);
}

/* (joined) a line from the host: "<from> <message>" */
static void co_join_line(CoLink *l, char *text) {
    char *rest = NULL;
    const long from = strtol(text, &rest, 10);
    if (rest == text || *rest != ' ' || from < 0) return;
    rest++;
    if (rest[0] == 'W' && rest[1] == ' ' && from == 0) {
        int id = 0;
        char name[64] = "", seed[64] = "-", build[16] = "";
        if (sscanf(rest + 2, "%d %63s %63s %15s", &id, name, seed, build) < 2) return;
        if (strcmp(build, co_build())) {   /* (Navi: the host's game is another update's, or from before this check) */
            hos_log("coop: the host is on another Longshot build (theirs %s, mine %s): leaving", build[0] ? build : "older", co_build());
            co_take(0, "X " CO_OTHER_BUILD);       /* (as the host's own no: the same note and status) */
            l->in_len = -1;                        /* (the link closed, as a turned-away one is) */
            return;
        }
        InterlockedExchange(&co.my_id, id);
        InterlockedExchange(&co.state, 3);
        InterlockedExchange(&co.links, 1);
        co_fine();
        char joined[160];
        snprintf(joined, sizeof joined, "J %s %s", name, seed);
        co_take(0, joined);
        return;
    }
    if (from == co.my_id && co.my_id) return;
    co_take((int)from, rest);
}

static void co_link_pump(CoLink *l, int hosting) {
    if (l->s == INVALID_SOCKET) return;
    for (;;) {                                         /* what came */
        if (l->in_len < 0) break;
        const int room = (int)sizeof l->in - 1 - l->in_len;
        if (room <= 0) { hos_log("coop: a line too long to be ours: link closed"); co_link_close(l); return; }
        const int n = ws.recv_(l->s, l->in + l->in_len, room, 0);
        if (n == 0) { co_link_close(l); return; }
        if (n < 0) {
            if (ws.error_() != WSAEWOULDBLOCK) { co_link_close(l); return; }
            break;
        }
        l->in_len += n;
        int start = 0;
        for (int i = 0; i < l->in_len && l->in_len >= 0; i++) {
            if (l->in[i] != '\n') continue;
            l->in[i] = 0;
            if (i > start && l->in[i - 1] == '\r') l->in[i - 1] = 0;
            char *text = l->in + start;
            start = i + 1;
            if (!*text) continue;
            const int before = l->in_len;
            if (hosting) co_host_line(l, text); else co_join_line(l, text);
            if (l->in_len != before) break;            /* (turned away) */
        }
        if (l->in_len < 0) break;
        memmove(l->in, l->in + start, (size_t)(l->in_len - start));
        l->in_len -= start;
    }
    while (l->out_len) {                               /* what goes */
        const int n = ws.send_(l->s, l->out, (int)(l->out_len > 65536 ? 65536 : l->out_len), 0);
        if (n <= 0) {
            if (n < 0 && ws.error_() != WSAEWOULDBLOCK) { co_link_close(l); return; }
            break;
        }
        memmove(l->out, l->out + n, l->out_len - (size_t)n);
        l->out_len -= (size_t)n;
    }
    if (l->in_len < 0 && !l->out_len) { l->in_len = 0; co_link_close(l); }
}

static void co_host_step(void) {
    if (co_listen == INVALID_SOCKET) {
        static int64_t retry;
        if (hos_now_ns() < retry) return;
        retry = hos_now_ns() + 3000000000LL;
        SOCKET s = ws.socket_(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { InterlockedExchange(&co.state, 4); return; }
        struct sockaddr_in at;
        memset(&at, 0, sizeof at);
        at.sin_family = AF_INET;
        const unsigned port = (unsigned)co.port;
        at.sin_port = (u_short)((port >> 8) | ((port & 0xff) << 8));
        if (ws.bind_(s, (const struct sockaddr *)&at, sizeof at) || ws.listen_(s, 4)) {
            hos_log("coop: cannot listen on port %u (error %d): is another game hosting on it?", port, ws.error_());
            co_fail("that port is in use on this PC", "Port %u is already used on this PC, by another program or another copy of the game: "
                    "pick another Port (everyone has to use the same one)", port);
            ws.close_(s);
            InterlockedExchange(&co.state, 4);
            return;
        }
        co_nonblocking(s);
        co_listen = s;
        co_fine();
        InterlockedExchange(&co.state, 1);
        hos_log("coop: hosting on port %u as %s", port, co.name);
    }
    for (;;) {
        SOCKET s = ws.accept_(co_listen, NULL, NULL);
        if (s == INVALID_SOCKET) break;
        CoLink *l = NULL;
        for (int i = 0; i < CO_PEERS - 1 && !l; i++)
            if (co_link[i].s == INVALID_SOCKET) l = &co_link[i];
        if (!l) { ws.close_(s); continue; }            /* (full) */
        co_nonblocking(s);
        memset(l, 0, sizeof *l);
        l->s = s;
    }
    char *out = NULL; size_t out_len = 0;              /* this game's own lines: to everyone, as from 0 */
    EnterCriticalSection(&co.lock);
    if (co.out_len) {
        out = malloc(co.out_len + 1);
        if (out) memcpy(out, co.out, co.out_len), out_len = co.out_len;
        co.out_len = 0;
    }
    LeaveCriticalSection(&co.lock);
    for (size_t at = 0; at < out_len;) {
        const char *end = memchr(out + at, '\n', out_len - at);
        if (!end) break;
        const size_t n = (size_t)(end - (out + at)) + 1;
        for (int i = 0; i < CO_PEERS; i++)
            if (co_link[i].s != INVALID_SOCKET && co_link[i].hello) co_link_put(&co_link[i], "0 ", 2), co_link_put(&co_link[i], out + at, n);
        at += n;
    }
    free(out);
    for (int i = 0; i < CO_PEERS; i++) co_link_pump(&co_link[i], 1);
}

static void co_join_step(void) {
    CoLink *l = &co_link[0];
    static int connecting;
    static int64_t retry, started;
    if (l->s == INVALID_SOCKET) {
        if (co.state == 3) {                           /* the link went: say so, start over */
            co_fail("the connection to the host dropped", "The connection to the host dropped (their game closed, or the network cut out): trying again");
            co_event(0, "D");
            EnterCriticalSection(&co.lock);
            for (int i = 0; i < CO_PEERS; i++) co.peer[i].used = 0;
            LeaveCriticalSection(&co.lock);
        }
        InterlockedExchange(&co.state, 2);
        InterlockedExchange(&co.my_id, 0);
        if (hos_now_ns() < retry) return;
        retry = hos_now_ns() + 3000000000LL;
        char addr[128], port[16];
        EnterCriticalSection(&co.lock);
        snprintf(addr, sizeof addr, "%s", co.addr);
        LeaveCriticalSection(&co.lock);
        char *colon = strrchr(addr, ':');              /* (an address copied with its port: that port) */
        snprintf(port, sizeof port, "%d", colon && atoi(colon + 1) > 0 ? atoi(colon + 1) : (int)co.port);
        if (colon) *colon = 0;
        ADDRINFOA hint, *found = NULL;
        memset(&hint, 0, sizeof hint);
        hint.ai_family = AF_INET, hint.ai_socktype = SOCK_STREAM;
        if (ws.getaddr_(addr, port, &hint, &found) || !found) {
            co_fail("that address was not found", "The host address could not be found: check it was typed right (NETWORK tab, Host address)");
            return;
        }
        SOCKET s = ws.socket_(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            co_nonblocking(s);
            ws.connect_(s, found->ai_addr, (int)found->ai_addrlen);   /* (answers at once: select below says when it is up) */
            memset(l, 0, sizeof *l);
            l->s = s;
            connecting = 1, started = hos_now_ns();
        }
        ws.freeaddr_(found);
        return;
    }
    if (connecting) {
        fd_set writable, failed;
        writable.fd_count = failed.fd_count = 1;
        writable.fd_array[0] = failed.fd_array[0] = l->s;
        const struct timeval none = {0, 0};
        ws.select_(0, NULL, &writable, &failed, &none);
        if (failed.fd_count || hos_now_ns() - started > 6000000000LL) {
            int error = WSAETIMEDOUT, size = sizeof error;
            if (failed.fd_count) ws.getopt_(l->s, SOL_SOCKET, SO_ERROR, (char *)&error, &size);
            ws.close_(l->s);
            l->s = INVALID_SOCKET;
            connecting = 0;
            char addr[128];
            EnterCriticalSection(&co.lock);
            snprintf(addr, sizeof addr, "%s", co.addr);
            LeaveCriticalSection(&co.lock);
            const int home = !strncmp(addr, "192.168.", 8) || !strncmp(addr, "10.", 3) || (!strncmp(addr, "172.", 4) && atoi(addr + 4) >= 16 && atoi(addr + 4) <= 31);
            if (error == WSAECONNREFUSED)
                co_fail("no game is hosting at that address", "That PC answered, but no game is hosting on port %d there: the host has to press Connect first, "
                        "and both of you need the same Port", (int)co.port);
            else if (home)
                co_fail("no answer from the host", "No answer. That is a home-network address: it only works when both PCs are in the same house. "
                        "Over the internet, use the host's public address (with port %d forwarded) or a VPN address (Tailscale, Radmin)", (int)co.port);
            else
                co_fail("no answer from the host", "No answer. The host's router has to let port %d through to their PC (port forwarding), "
                        "or both of you join one VPN (Tailscale, Radmin) and use the host's address there", (int)co.port);
            return;
        }
        if (!writable.fd_count) return;
        connecting = 0;
        co_link_say(l, "H %d %s %s %s", CO_VERSION, co.name, co.seed, co_build());
        hos_log("coop: reached the host, saying hello as %s", co.name);
    }
    if (co.state == 3) {
        EnterCriticalSection(&co.lock);
        if (co.out_len) co_link_put(l, co.out, co.out_len), co.out_len = 0;
        LeaveCriticalSection(&co.lock);
    }
    co_link_pump(l, 0);
}

static DWORD WINAPI co_net(void *unused) {
    (void)unused;
    int mode = 0, port = 0;
    char addr[128] = "";
    for (;;) {
        const int want = (int)co.want;
        EnterCriticalSection(&co.lock);
        const int moved = want != mode || (mode && (int)co.port != port) || (mode == 2 && strcmp(addr, co.addr));
        if (moved) snprintf(addr, sizeof addr, "%s", co.addr);
        LeaveCriticalSection(&co.lock);
        if (moved) {
            if (mode) co_close_all(), hos_log("coop: link closed");
            mode = want, port = (int)co.port;
            co_fine();
            InterlockedExchange(&co.state, mode == 1 ? 1 : mode == 2 ? 2 : 0);
            if (mode == 1) co_listen = INVALID_SOCKET;
        }
        if (mode == 1) co_host_step();
        else if (mode == 2) co_join_step();
        Sleep(mode ? 4 : 100);
    }
    return 0;
}

/* The name and the address are read with the game's first frame, for the tab's rows. The link's thread is started the
   first time the tab asks to connect (link), not before: a game that never connects loads no network library and has
   no thread of co-op's. 1 with the thread running. */
static int co_start(int link) {
    static int failed;
    if (!co.ready) {
        InitializeCriticalSection(&co.lock);
        InitializeCriticalSection(&co.game);
        for (int i = 0; i < CO_PEERS; i++) co_link[i].s = INVALID_SOCKET;
        co_config_load();
        co_seed_load();
        co.ready = 1;
    }
    if (co.thread || !link || failed) return co.thread != NULL;
    if (!co_ws_load()) { failed = 1; hos_log("coop: Windows' network library did not load: no co-op"); return 0; }
    co.thread = CreateThread(NULL, 0, co_net, NULL, 0, NULL);
    if (!co.thread) failed = 1;
    return co.thread != NULL;
}

/* ── the game's side ─────────────────────────────────────────────────────── */

/* The save flags: GameDataMgr (hos_main.c OOT_GAMEDATA_SLOT), its bools as f_8022d9c0 reads them:
   +32 the (hash, index) table (+44 its size), +24 the 24-byte entries (+16 their count). An entry, as the
   game's own code writes it (80bc27c0 loads a save into it, 80bcad18 keeps the changed ones at a save):
       +0x10 which save file keeps it (below 0: none)    +0x14 changed since the last save
       +0x15 the value a new file starts with            +0x16 the value now
       +0x17 the value as last saved
   Two things to know of them:
     * a flag is progress when it has LEFT the value it starts with, whichever way. Mido stands in the way
       while KokiriForest_Npc_Kokiri_Mido_Child_Blocker is ON, and it starts on: "tell the others what is
       on" had a new file telling everyone Mido was back. 54 of the shared flags start on.
     * a flag written without its "changed" mark is not in the next save. */
#define CO_GAMEDATA_SLOT 0x84c9e180ULL
typedef struct { uint64_t table, entries; uint32_t size, count; } CoFlags;

static int co_flags_open(CoFlags *f) {
    const uint64_t holder = G64(CO_GAMEDATA_SLOT);
    if (!co_ok(holder, 8)) return 0;
    const uint64_t mgr = G64(holder);
    if (!co_ok(mgr, 0x30)) return 0;
    f->table = G64(mgr + 32), f->size = G32(mgr + 44), f->entries = G64(mgr + 24), f->count = G32(mgr + 16);
    return f->size && f->count && f->size < (1u << 20) && f->count < (1u << 20) && co_ok(f->table, f->size * 8ULL) &&
           co_ok(f->entries, f->count * 24ULL);
}

static uint8_t *co_flag_value(const CoFlags *f, uint32_t hash, uint32_t *index) {
    for (uint32_t probe = 0, slot = hash % f->size; probe < f->size; probe++, slot = (slot + 1) % f->size) {
        const uint32_t key = G32(f->table + slot * 8ULL);
        if (!key) return NULL;
        if (key != hash) continue;
        const uint32_t i = G32(f->table + slot * 8ULL + 4);
        if (i >= f->count) return NULL;
        if (index) *index = i;
        return (uint8_t *)GP(f->entries + i * 24ULL + 22);
    }
    return NULL;
}

/* Has this flag left the value a new file starts with? (entry: the 24 bytes) */
static int co_flag_moved(const uint8_t *entry) { return (entry[0x16] & 1) != (entry[0x15] & 1); }

/* Another player's flag has: this one leaves its start value too, and is marked for the next save. */
static int co_flag_move(uint8_t *entry) {
    if (co_flag_moved(entry)) return 0;
    int32_t kept;
    memcpy(&kept, entry + 0x10, 4);
    entry[0x16] = !(entry[0x15] & 1);
    if (kept >= 0) entry[0x14] = 1;
    HOS_WRITTEN(entry, 24);
    return 1;
}

static uint32_t *co_shared; static size_t co_shared_n;   /* <game>\coop\flags.bin, sorted */
static int co_shared_loaded;

static void co_shared_load(void) {
    char game[MAX_PATH], path[MAX_PATH + 32];
    co_shared_loaded = 1;
    co_game_dir(game, sizeof game);
    snprintf(path, sizeof path, "%s\\coop\\flags.bin", game);
    FILE *f = fopen(path, "rb");
    if (!f) { hos_log("coop: %s is not there: items are shared, flags are not (nx2/coop_flags.py makes it)", path); return; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < (1 << 22) && (co_shared = malloc((size_t)size)) != NULL)
        co_shared_n = fread(co_shared, 4, (size_t)size / 4, f);
    fclose(f);
    hos_log("coop: %u save flags are shared", (unsigned)co_shared_n);
}

static int co_is_shared(uint32_t hash) {
    size_t lo = 0, hi = co_shared_n;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (co_shared[mid] < hash) lo = mid + 1; else hi = mid;
    }
    return lo < co_shared_n && co_shared[lo] == hash;
}

/* What the others have said is on (all of it, for the whole session: asked again of a save that
   is loaded later), and what of it is still to be turned on here. */
static uint32_t *co_theirs; static size_t co_theirs_n, co_theirs_cap, co_theirs_done;
static uint8_t *co_shadow; static uint32_t co_shadow_n;   /* each bool as last seen: 0, 1, 0xff not looked at */
static int co_level;                                  /* say everything again: someone joined */

static void co_theirs_add(uint32_t hash) {
    for (size_t i = 0; i < co_theirs_n; i++)
        if (co_theirs[i] == hash) return;
    if (co_theirs_n == co_theirs_cap) {
        const size_t cap = co_theirs_cap ? co_theirs_cap * 2 : 1024;
        uint32_t *grown = cap <= (1u << 16) ? realloc(co_theirs, cap * 4) : NULL;
        if (!grown) return;
        co_theirs = grown, co_theirs_cap = cap;
    }
    co_theirs[co_theirs_n++] = hash;
}

/* The others' flags not yet on here: on. Then mine that came on since the last look: said. */
static void co_flags_step(int levelling) {
    CoFlags f;
    if (!co_shared_n || !co_flags_open(&f)) return;
    if (co_shadow_n != f.count) {
        uint8_t *grown = realloc(co_shadow, f.count);
        if (!grown) return;
        co_shadow = grown, co_shadow_n = f.count;
        memset(co_shadow, 0xff, f.count);
    }
    if (levelling) memset(co_shadow, 0xff, co_shadow_n);
    unsigned turned_on = 0;
    for (; co_theirs_done < co_theirs_n; co_theirs_done++) {
        const uint32_t hash = co_theirs[co_theirs_done];
        uint32_t index = 0;
        uint8_t *value = co_is_shared(hash) ? co_flag_value(&f, hash, &index) : NULL;   /* (only what this game shares too) */
        if (!value) continue;
        turned_on += (unsigned)co_flag_move(value - 22);
        co_shadow[index] = 1;
    }
    if (turned_on) hos_log("coop: %u flags the others moved, moved here", turned_on);
    char line[CO_LINE];
    size_t used = 0;
    unsigned said = 0;
    for (uint32_t slot = 0; slot < f.size; slot++) {
        const uint32_t key = G32(f.table + slot * 8ULL);
        if (!key) continue;
        const uint32_t index = G32(f.table + slot * 8ULL + 4);
        if (index >= f.count) continue;
        const uint8_t now = (uint8_t)co_flag_moved((const uint8_t *)GP(f.entries + index * 24ULL));   /* (1: it has left its start value) */
        const uint8_t was = co_shadow[index];
        co_shadow[index] = now;
        if (!now || was == 1 || !co_is_shared(key)) continue;
        if (!used) used = (size_t)snprintf(line, sizeof line, "F");
        used += (size_t)snprintf(line + used, sizeof line - used, " %x", key);
        said++;
        if (used > sizeof line - 64) co_send("%s", line), used = 0;
    }
    if (used) co_send("%s", line);
    if (said && co.links > 0) hos_log("coop: %u flags of mine said to the others", said);
}

/* The flags kept by a placed object's own number (Bool64bitKey), as the game's own code keeps them
   (f_805ce6b0 fills the table, the loop at 80bc2570 writes one): GameDataMgr +0xc58 the count,
   +0xc60 the entries, sorted by key, 16 bytes each:
       +0 the key   +8 reset type   +0xc the value   +0xd save file index   +0xe changed   +0xf group
   Turning one on the game's way: the value, "changed" (bit 0, when the flag is one a save keeps),
   the table's own reset types seen (+0x50) and its changed mark (+0x54). */
typedef struct { uint64_t box, entries; uint32_t count; } CoKeys;
#define CO_KEY_LASTING 24u              /* (reset type: 88 pots and grass, 280 enemies, 25 shop stock put themselves back) */

static int co_keys_open(CoKeys *k) {
    const uint64_t holder = G64(CO_GAMEDATA_SLOT);
    if (!co_ok(holder, 8)) return 0;
    const uint64_t mgr = G64(holder);
    if (!co_ok(mgr, 0xd20)) return 0;
    k->box = mgr + 0xc58, k->count = G32(mgr + 0xc58), k->entries = G64(mgr + 0xc60);
    return k->count && k->count < (1u << 20) && co_ok(k->entries, k->count * 16ULL);
}

static int co_key_find(const CoKeys *k, uint64_t key) {
    uint32_t lo = 0, hi = k->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (G64(k->entries + mid * 16ULL) < key) lo = mid + 1; else hi = mid;
    }
    return lo < k->count && G64(k->entries + lo * 16ULL) == key ? (int)lo : -1;
}

/* (co_ktheirs_ok: that key is dealt with in this save. One that this game's table does not hold yet stays waiting and
   is looked for again each time: it used to be dropped at the first look, and a chest another player had opened was
   then closed again here.) */
static uint64_t *co_ktheirs; static uint8_t *co_ktheirs_ok; static size_t co_ktheirs_n, co_ktheirs_cap, co_ktheirs_done;
static uint8_t *co_kshadow; static uint32_t co_kshadow_n;

static void co_ktheirs_add(uint64_t key) {
    for (size_t i = 0; i < co_ktheirs_n; i++)
        if (co_ktheirs[i] == key) return;
    if (co_ktheirs_n == co_ktheirs_cap) {
        const size_t cap = co_ktheirs_cap ? co_ktheirs_cap * 2 : 1024;
        uint64_t *grown = cap <= (1u << 16) ? realloc(co_ktheirs, cap * 8) : NULL;
        if (!grown) return;
        co_ktheirs = grown;
        uint8_t *ok = realloc(co_ktheirs_ok, cap);
        if (!ok) return;
        co_ktheirs_ok = ok, co_ktheirs_cap = cap;
    }
    co_ktheirs_ok[co_ktheirs_n] = 0;
    co_ktheirs[co_ktheirs_n++] = key;
}

static void co_keys_step(int levelling) {
    CoKeys k;
    if (!co_keys_open(&k)) return;
    if (co_kshadow_n != k.count) {
        uint8_t *grown = realloc(co_kshadow, k.count);
        if (!grown) return;
        co_kshadow = grown, co_kshadow_n = k.count;
        memset(co_kshadow, 0xff, k.count);
    }
    if (levelling) memset(co_kshadow, 0xff, co_kshadow_n);
    unsigned turned_on = 0, waiting = 0;
    if (!co_ktheirs_done && co_ktheirs_n) memset(co_ktheirs_ok, 0, co_ktheirs_n), co_ktheirs_done = 1;   /* (another save, maybe: all looked at again) */
    for (size_t t = 0; t < co_ktheirs_n; t++) {
        if (co_ktheirs_ok[t]) continue;
        const int i = co_key_find(&k, co_ktheirs[t]);
        if (i < 0) { waiting++; continue; }               /* (not in this game's table now: kept, looked for again) */
        co_ktheirs_ok[t] = 1;
        const uint64_t e = k.entries + (uint64_t)i * 16;
        uint8_t *b = (uint8_t *)GP(e);
        if (G32(e + 8) != CO_KEY_LASTING) continue;       /* (only what this game shares too) */
        if (!(b[0xc] & 1)) {
            b[0xc] = 1;
            if ((int8_t)b[0xd] >= 0) b[0xe] |= 1;
            G32(k.box + 0x50) |= G32(e + 8);
            *(uint8_t *)GP(k.box + 0x54) = 1;
            HOS_WRITTEN(b, 16);
            turned_on++;
        }
        co_kshadow[i] = 1;
    }
    if (turned_on) hos_log("coop: %u chests and other objects the others dealt with marked done", turned_on);
    { static unsigned said_waiting = ~0u; if (waiting != said_waiting) said_waiting = waiting, hos_log("coop: %u of the others' chests and objects are not in this game's table now (of %u objects here): kept for when they are", waiting, k.count); }
    char line[CO_LINE];
    size_t used = 0;
    unsigned said = 0;
    for (uint32_t i = 0; i < k.count; i++) {
        const uint64_t e = k.entries + (uint64_t)i * 16;
        const uint8_t now = *(const uint8_t *)GP(e + 0xc) & 1, was = co_kshadow[i];
        co_kshadow[i] = now;
        if (!now || was == 1 || G32(e + 8) != CO_KEY_LASTING) continue;
        if (!used) used = (size_t)snprintf(line, sizeof line, "K");
        used += (size_t)snprintf(line + used, sizeof line - used, " %llx", (unsigned long long)G64(e));
        said++;
        if (used > sizeof line - 64) co_send("%s", line), used = 0;
    }
    if (used) co_send("%s", line);
    if (said && co.links > 0) hos_log("coop: %u chests and other objects of mine said to the others", said);
}

/* The counted save values that are shared: each dungeon's small keys. GameDataMgr's ints: +0xd0 the
   count, +0xd8 the 40-byte entries (the value at +0x1c), +0xe0 the (hash, index) table, +0xec its size.
   (PlayerStatus.MaxLife and the like are in there too, but the game writes those from Link himself
   every frame: a value put there is gone the next moment. Hearts go round as the items they are.) */
static uint32_t co_mm3(const char *text) {             /* MurMur3, seed 0: the game's name hash */
    const size_t len = strlen(text), blocks = len / 4;
    uint32_t h = 0, k = 0;
    for (size_t i = 0; i < blocks; i++) {
        memcpy(&k, text + i * 4, 4);
        k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k, h = (h << 13) | (h >> 19), h = h * 5 + 0xe6546b64u;
    }
    k = 0;
    for (size_t i = len & 3; i > 0; i--) k = (k << 8) | (unsigned char)text[blocks * 4 + i - 1];
    if (len & 3) k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u, h ^= k;
    h ^= (uint32_t)len, h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

static const char *const co_dungeon[] = {
    "InsideTheDekuTree", "DodongoCavern", "InsideJabuJabuBelly", "ForestTemple", "FireTemple", "WaterTemple", "ShadowTemple",
    "SpiritTemple", "IceCavern", "BottomOfTheWell", "GerudoTrainingGround", "GerudoFortress", "InsideGanonCastle", "FinalBattleField",
};
#define CO_COUNTS ((int)(sizeof co_dungeon / sizeof co_dungeon[0]))
static uint32_t co_count_hash[CO_COUNTS];
static int co_count_seen[CO_COUNTS], co_count_known[CO_COUNTS];   /* each as last read; whether it has been read */
static int co_count_add[CO_COUNTS], co_count_set[CO_COUNTS], co_count_has_set[CO_COUNTS];   /* the others' changes still to make */

static int32_t *co_int_value(uint32_t hash) {          /* a save value of the Int kind, by its name's hash */
    const uint64_t holder = G64(CO_GAMEDATA_SLOT);
    const uint64_t mgr = co_ok(holder, 8) ? G64(holder) : 0;
    if (!co_ok(mgr, 0x100)) return NULL;
    const uint64_t entries = G64(mgr + 0xd8), table = G64(mgr + 0xe0);
    const uint32_t count = G32(mgr + 0xd0), size = G32(mgr + 0xec);
    if (!size || !count || size > (1u << 20) || count > (1u << 20) || !co_ok(table, size * 8ULL) || !co_ok(entries, count * 40ULL)) return NULL;
    for (uint32_t probe = 0, slot = hash % size; probe < size; probe++, slot = (slot + 1) % size) {
        const uint32_t key = G32(table + slot * 8ULL);
        if (!key) return NULL;
        if (key != hash) continue;
        const uint32_t i = G32(table + slot * 8ULL + 4);
        return i < count ? (int32_t *)GP(entries + i * 40ULL + 0x1c) : NULL;
    }
    return NULL;
}

static int32_t *co_count_value(int n) { return co_int_value(co_count_hash[n]); }

static int co_count_index(uint32_t hash) {
    if (!co_count_hash[0]) {
        char name[96];
        for (int n = 0; n < CO_COUNTS; n++) snprintf(name, sizeof name, "DungeonInfo.%s.SmallKeyNum", co_dungeon[n]), co_count_hash[n] = co_mm3(name);
        if (co_mm3("Sequence_IsLoadingBanc") != 0xef2b60c2u) hos_log("coop: the name hash is wrong here (%x): small keys will not be shared", co_mm3("Sequence_IsLoadingBanc"));
    }
    for (int n = 0; n < CO_COUNTS; n++)
        if (co_count_hash[n] == hash) return n;
    return -1;
}

/* fresh: a save was just loaded, what it holds is where it starts (not a find to tell of).
   levelling: someone joined; the host says what each count is. */
static void co_counts_step(int fresh, int levelling) {
    co_count_index(0);
    for (int n = 0; n < CO_COUNTS; n++) {
        int32_t *value = co_count_value(n);
        if (!value) continue;
        if (fresh || !co_count_known[n]) co_count_known[n] = 1, co_count_seen[n] = *value;
        if (*value != co_count_seen[n]) {                 /* mine changed: a key found (+1), a door opened (-1) */
            co_send("N %x %d", co_count_hash[n], *value - co_count_seen[n]);
            hos_log("coop: %s small keys %d -> %d, said to the others", co_dungeon[n], co_count_seen[n], *value);
            co_count_seen[n] = *value;
        }
        if (co_count_has_set[n] || co_count_add[n]) {     /* theirs */
            int next = co_count_has_set[n] ? co_count_set[n] : *value + co_count_add[n];
            next = next < 0 ? 0 : next > 99 ? 99 : next;
            if (next != *value) {
                hos_log("coop: %s small keys %d -> %d (the others)", co_dungeon[n], *value, next);
                *value = next;
                *((uint8_t *)value - 0x1c + 0x14) = 1;      /* (changed since the last save, as a flag's entry has it) */
                HOS_WRITTEN((uint8_t *)value - 0x1c, 40);
            }
            co_count_seen[n] = next, co_count_has_set[n] = 0, co_count_add[n] = 0;
        }
        if (levelling && co.state == 1) co_send("V %x %d", co_count_hash[n], *value);
    }
}

/* Sequence_CurrentBanc (hos_main.c oot_gamedata_string: f_80cd0ea0, String64 by name hash) */
static void co_current_map(Ctx *c, char *buf, size_t size) {
    buf[0] = 0;
    const uint64_t holder = G64(CO_GAMEDATA_SLOT);
    const uint64_t mgr = co_ok(holder, 8) ? G64(holder) : 0;
    A64Fn get = a64_lookup(0x80cd0ea0ULL);
    if (!co_ok(mgr, 0x30) || !get) return;
    const Ctx saved = *c;
    const uint64_t out = (c->x[31] - 0x40) & ~0xfULL;
    recomp_store64_sp(c, out, 0);
    c->x[31] = out;
    c->x[0] = mgr, c->x[1] = out, c->x[2] = 0x1d6189dau;
    get(c);
    const int found = (int)(c->x[0] & 1);
    const uint64_t text = recomp_load64(c, out);
    *c = saved;
    if (!found || !co_ok(text, 64)) return;
    const char *s = (const char *)GP(text);
    size_t n = 0;
    for (; n + 1 < size && n < 63 && s[n] > ' ' && s[n] < 0x7f; n++) buf[n] = s[n];
    buf[n] = 0;
}

/* (Navi) the age a map is for: each age has its own copy of a map, named ..._Child or ..._Adult. 1 child, 0 adult, -1 a map
   both ages share (Hyrule Castle's grounds, a dungeon), or none */
static int co_map_age(const char *map) {
    const size_t len = strlen(map);
    if (len > 6 && !strcmp(map + len - 6, "_Child")) return 1;
    if (len > 6 && !strcmp(map + len - 6, "_Adult")) return 0;
    return -1;
}

/* (Navi) my Link's age, said to the others (their game makes my body that age: a wrong one drew a young Link as adult).
   The map's age when it has one; else the game's own answer to Sequence_IsChildhood (f_8022d9c0, what the Debug tab's age row
   reads), not the save table read by hand; else the save table; else child, as a new file starts. */
static int co_my_child(Ctx *c) {
    static int said = -1;
    int child = co_map_age(co_map), how = 0;
    if (child < 0) {
        const uint64_t holder = G64(CO_GAMEDATA_SLOT);
        const uint64_t mgr = co_ok(holder, 8) ? G64(holder) : 0;
        A64Fn get = a64_lookup(0x8022d9c0ULL);
        if (co_ok(mgr, 0x30) && get) {
            const Ctx saved = *c;
            const uint64_t out = (c->x[31] - 0x40) & ~0xfULL;
            recomp_store64_sp(c, out, 0);
            c->x[31] = out;
            c->x[0] = mgr, c->x[1] = out, c->x[2] = 0xcea220d3u;
            get(c);
            const int found = (int)(c->x[0] & 1), value = recomp_load8(c, out) & 1;
            *c = saved;
            if (found) child = value, how = 1;
        }
    }
    if (child < 0) {
        CoFlags f;
        const uint8_t *v = co_flags_open(&f) ? co_flag_value(&f, 0xcea220d3u, NULL) : NULL;
        if (v) child = *v & 1, how = 2;
    }
    if (child < 0) child = 1, how = 3;
    if (child != said) {
        said = child;
        hos_log("coop: age: I am %s (%s), map '%s'", child ? "young Link" : "adult Link",
                how == 0 ? "the map's age" : how == 1 ? "the game's Sequence_IsChildhood" : how == 2 ? "the save's flag" : "nothing read: a new file's",
                co_map);
    }
    return child;
}

/* "KokiriForest" as "Kokiri Forest" */
static void co_map_words(const char *map, char *out, size_t cap) {
    size_t n = 0, len = strlen(map);
    if (len > 6 && (!strcmp(map + len - 6, "_Child") || !strcmp(map + len - 6, "_Adult"))) len -= 6;   /* (each age has its own copy of a map) */
    for (size_t i = 0; i < len && n + 2 < cap; i++) {
        const char ch = map[i] == '_' ? ' ' : map[i];
        if (i && ch >= 'A' && ch <= 'Z' && map[i - 1] >= 'a' && map[i - 1] <= 'z') out[n++] = ' ';
        out[n++] = ch;
    }
    out[n] = 0;
}

/* The others' items: which they hold (lasting ones), how many counted ones are still to give. Those the
   Inventory page lists go by their place in it; any other by its name (co_more). */
static uint8_t co_they_hold[256], co_to_check[256];
static int co_to_give[256];
typedef struct { char name[0x60]; uint8_t they_hold, to_check; int to_give; } CoMore;
static CoMore co_more[96];
static int co_more_n;
static int co_their_quarters = -1;                    /* the most hearts another player said they have, in quarters */

static CoMore *co_more_get(const char *name) {
    for (int i = 0; i < co_more_n; i++)
        if (!strcmp(co_more[i].name, name)) return &co_more[i];
    if (co_more_n >= (int)(sizeof co_more / sizeof co_more[0]) || strlen(name) >= 0x60) return NULL;
    CoMore *m = &co_more[co_more_n++];
    memset(m, 0, sizeof *m);
    strcpy(m->name, name);
    return m;
}

static int co_holds(Ctx *c, int i) {
    char give[0x70], other[2][0x70];
    co_item_spellings(i, give, other);
    int n = co_pouch_count(c, co_item_name[i]);
    if (n < 0) return -1;
    if (n > 0) return 1;
    if (strcmp(give, co_item_name[i]) && co_pouch_count(c, give) > 0) return 1;
    for (unsigned k = 0; k < 2; k++)
        if (other[k][0] && co_pouch_count(c, other[k]) > 0) return 1;
    return 0;
}

static void co_give_named(Ctx *c, const char *name) {
    co_applying = 1;
    const int status = co_pouch_give(c, name);
    co_applying = 0;
    hos_log("coop: gave %s (status %d)", name, status);
}
/* (counted things: the lot in one call, as the game itself adds thirty seeds at once) */
static void co_give_many(Ctx *c, const char *name, int many) {
    co_applying = 1;
    const int status = co_pouch_give_many(c, name, many);
    co_applying = 0;
    hos_log("coop: gave %s x%d (status %d)", name, many, status);
}

/* An item that takes another's place. The game itself swaps them (an egg hatches: the egg goes, the cucco comes), but the
   others are only told what was found, so they kept both, and the game went on showing them the old one: a Weird Egg
   stayed an egg after another player's hatched, with Item_AlarmCucco in the pouch all along. Whoever holds a later one
   of a row has the earlier ones taken away. Looked at every two seconds of play, so a save that already holds both is put
   right too. */
static const char *const co_trade_rows[][11] = {
    {"Item_WeirdEgg", "Item_AlarmCucco", "Item_ZeldaLetter", NULL},
    {"Item_PocketEgg", "Item_PocketCucco", "Item_CojiroCucco", "Item_OddMushroom", "Item_PoacherSaw", "Item_Sword_BrokenGiant",
     "Item_Prescription", "Item_EyeballFrog", "Item_ClaimCheck", NULL},
};
static void co_trade_step(Ctx *c, int64_t now) {
    static int64_t next;
    if (now < next) return;
    next = now + 2000000000LL;
    for (unsigned r = 0; r < sizeof co_trade_rows / sizeof co_trade_rows[0]; r++) {
        int last = -1;
        for (int k = 0; co_trade_rows[r][k]; k++)
            if (co_pouch_count(c, co_trade_rows[r][k]) > 0) last = k;
        for (int k = 0; k < last; k++) {
            const int held = co_pouch_count(c, co_trade_rows[r][k]);
            if (held <= 0) continue;
            co_applying = 1;
            const int status = co_pouch_take(c, co_trade_rows[r][k], held);
            co_applying = 0;
            hos_log("coop: %s took the place of %s: taken away (status %d)", co_trade_rows[r][last], co_trade_rows[r][k], status);
        }
    }
}

static void co_give(Ctx *c, int i) {
    char give[0x70], other[2][0x70];
    co_item_spellings(i, give, other);
    co_give_named(c, give);
}

/* My hearts in quarters: containers (PlayerStatus.MaxLife counts them in quarters already) and pieces. */
static int co_my_quarters(Ctx *c) {
    static uint32_t hash;
    if (!hash) hash = co_mm3("PlayerStatus.MaxLife");
    const int32_t *max = co_int_value(hash);
    const int pieces = co_pouch_count(c, "Obj_Heart_Piece");
    if (!max || pieces < 0 || *max < 4 || *max > 400) return -1;
    return *max + (pieces > 3 ? 3 : pieces);
}

/* One item a tick at most, a little apart: the game shows each one it is handed. */
static void co_items_step(Ctx *c, int64_t now) {
    static int64_t next;
    static int at;
    if (now < next) return;
    if (co_pouch_count(c, "Item_DekuSeed") < 0) return;   /* the pouch is not ready to be asked */
    if (co_their_quarters > 0) {                       /* hearts another player has and I do not: a container, or a piece */
        const int mine = co_my_quarters(c);
        if (mine > 0 && co_their_quarters > mine) {
            co_give_named(c, co_their_quarters - mine >= 4 ? "Obj_Heart_Container" : "Obj_Heart_Piece");
            next = now + 600000000LL;
            return;
        }
        if (mine > 0) co_their_quarters = -1;
    }
    const int total = CO_ITEMS + co_more_n;
    for (int tried = 0; tried < total; tried++, at = (at + 1) % total) {
        const int i = at % total;
        if (i >= CO_ITEMS) {
            CoMore *m = &co_more[i - CO_ITEMS];
            if (m->to_check) {
                m->to_check = 0;
                if (co_pouch_count(c, m->name) > 0) continue;
                co_give_named(c, m->name);
            } else if (m->to_give > 0) {
                const int many = m->to_give > 99 ? 99 : m->to_give;
                m->to_give -= many;
                co_give_many(c, m->name, many);
            } else continue;
        } else if (co_item_kind[i] == 0 && co_to_check[i]) {
            const int holds = co_holds(c, i);
            if (holds < 0) return;
            co_to_check[i] = 0;
            if (holds) continue;
            co_give(c, i);
        } else if (co_item_kind[i] != 0 && co_to_give[i] > 0) {
            char give[0x70], other[2][0x70];
            const int many = co_to_give[i] > 99 ? 99 : co_to_give[i];
            co_to_give[i] -= many;
            co_item_spellings(i, give, other);
            co_give_many(c, give, many);
        } else continue;
        at = (at + 1) % total;
        next = now + 400000000LL;
        return;
    }
}

/* Everything lasting I hold, said quietly, and my hearts: for a player who just joined. */
static void co_say_items(Ctx *c) {
    for (int i = 0; i < CO_ITEMS; i++) {
        if (co_item_kind[i] != 0) continue;
        const int holds = co_holds(c, i);
        if (holds < 0) return;
        if (holds) co_send("I %s 1 q", co_item_name[i]);
    }
    for (unsigned i = 0; i < sizeof co_more_known / sizeof co_more_known[0]; i++)
        if (co_pouch_count(c, co_more_known[i]) > 0) co_send("I %s 1 q", co_more_known[i]);
    for (int i = 0; i < co_more_n; i++)
        if (co_more[i].they_hold) co_send("I %s 1 q", co_more[i].name);   /* (what a third player told me of) */
    const int quarters = co_my_quarters(c);
    if (quarters > 0) co_send("M %d", quarters);
}

/* (oot_coop_pouch_add, any of the game's threads) an item entered the pouch by this name */
static void co_pickup(const char *name, int many) {
    if (!co.ready || co.links <= 0 || co_applying) return;
    char canon[0x60];
    const int kind = co_item_class(name, canon);
    if (kind == 1) co_send("I %s 1", canon);
    else if (kind == 2) co_send("I %s %d", canon, many < 1 ? 1 : many > 99 ? 99 : many);
}

/* (hos_main.c hos_oot_lift_player, the player's own actor each frame) where Link stands */
static void co_tick(Ctx *c, uint64_t link);
void oot_coop_player(Ctx *c, uint64_t actor) {
    (void)c;
    if (!co.ready || !hos_guest_range(actor, 848)) return;   /* (not co_ok: Link is not in the heap's first stretch) */
    {
        static int said;
        if (!said) said = 1, hos_log("coop: Link's actor seen at %llx", (unsigned long long)actor);
    }
    float v[3], sx, sz;
    memcpy(v, GP(actor + 796), sizeof v);
    memcpy(&sx, GP(actor + 816), 4);
    memcpy(&sz, GP(actor + 840), 4);
    EnterCriticalSection(&co.lock);
    memcpy(co.pos, v, sizeof v);
    co.yaw = atan2f(sx, sz);
    LeaveCriticalSection(&co.lock);
    co.player_seen = hos_now_ns();
    co_tick(c, actor);
}

/* A name or an address typed in the menu's Network tab (which: 1 the name, 2 the address): its letters, then kept. */
static void co_typed(int which) {
    char text[160];
    size_t n = 0;
    for (int ch; n + 1 < sizeof text && (ch = rx_option(HOS_OPT_COOP_OUT, 0)) != 0;) text[n++] = (char)ch;
    text[n] = 0;
    char clean[128];
    co_clean(clean, which == 1 ? 17 : sizeof clean, text, which == 1 ? "_-" : "._-:");
    if (!clean[0]) { co_note(which == 1 ? "A name is letters and digits: nothing changed" : "No address typed: nothing changed"); return; }
    EnterCriticalSection(&co.lock);
    if (which == 1) snprintf(co.name, sizeof co.name, "%s", clean);
    else snprintf(co.addr, sizeof co.addr, "%s", clean);
    LeaveCriticalSection(&co.lock);
    co_config_save();
    co_menu_text(which == 1 ? 2 : 3, which == 1 ? clean : "Set (kept off the screen)");
    co_note(which == 1 ? "Name set to %s" : "Host address set%s", which == 1 ? clean : "");
    if (which == 1) co_send("R %s", clean);            /* (connected: the others see the new name now) */
}

static void co_events(void) {
    CoEvent *list = NULL; size_t n = 0;
    EnterCriticalSection(&co.lock);
    if (co.in_len && (list = malloc(co.in_len * sizeof *list)) != NULL) memcpy(list, co.in, co.in_len * sizeof *list), n = co.in_len;
    if (list || !co.in_len) co.in_len = 0;
    LeaveCriticalSection(&co.lock);
    for (size_t e = 0; e < n; e++) {
        const char *text = list[e].text;
        char who[24];
        co_peer_name(list[e].from, who, sizeof who);
        if (text[0] == 'J' && text[1] == ' ') {
            char name[64] = "", seed[64] = "-";
            sscanf(text + 2, "%63s %63s", name, seed);
            co_note("%s is here", name);
            if (strcmp(seed, co.seed)) co_note("%s plays a changed game: items will not match", name);
            co_level = 1;                              /* all I have done and hold: said again */
            co_world_resay = 1;                        /* (Navi) and the world's broken, moved and cut things */
        } else if (text[0] == 'L' && text[1] == ' ') {
            co_note("%s left", text + 2);
        } else if (text[0] == 'D' && text[1] == ' ') {     /* a player's swing reached someone: me? */
            int target = -1, quarters = 0;
            if (sscanf(text + 2, "%d %d", &target, &quarters) == 2 && target == (int)co.my_id && quarters > 0 && quarters <= 8) {
                snprintf(co_hurt_by, sizeof co_hurt_by, "%s", who);
                InterlockedExchange(&co_hurt_from, list[e].from);
                InterlockedExchange(&co_hurt, quarters);
            }
        } else if (text[0] == 'w' && (text[1] == 'b' || text[1] == 'm' || text[1] == 'p' || text[1] == 'g') && text[2] == ' ') {
            co_world_take(list[e].from, text);        /* (Navi) the world: broken, moved, placed, cut */
        } else if (text[0] == 'U' && text[1] == ' ') {     /* a player's hit took life off an enemy: the same off mine */
            unsigned hash = 0, taken = 0;
            float at[3] = {0, 0, 0};
            if (sscanf(text + 2, "%x %f %f %f %u", &hash, &at[0], &at[1], &at[2], &taken) == 5 && taken > 0 && taken < 100000) co_enemy_hit(hash, at, taken);
        } else if (text[0] == 'O' && text[1] == ' ') {     /* a player lost their last heart to another's hit */
            const int by = atoi(text + 2);
            char winner[24];
            co_peer_name(by, winner, sizeof winner);
            if (by == (int)co.my_id) co_note("You defeated %s", who);
            else co_note("%s defeated %s", winner, who);
        } else if (text[0] == 'D' && !text[1]) {
            co_note("The link to the host is gone: trying again");
        } else if (text[0] == 'X' && text[1] == ' ') {
            co_note("The host said no: %s", text + 2);
            co_fail("the host turned this game away", "%s", "");
        } else if (text[0] == 'I' && text[1] == ' ') {
            char name[0x70] = "", canon[0x60], quiet[8] = "";
            int many = 1;
            if (sscanf(text + 2, "%100s %d %7s", name, &many, quiet) < 1) continue;
            const int kind = co_item_class(name, canon);
            if (!kind) continue;
            many = many < 1 ? 1 : many > 99 ? 99 : many;
            const int i = co_item_index(canon);
            const char *label = canon;
            if (i >= 0 && i < 256) {
                label = co_item_label[i];
                if (kind == 1) co_they_hold[i] = co_to_check[i] = 1; else co_to_give[i] += many;
            } else {
                CoMore *m = co_more_get(canon);
                if (!m) continue;
                if (kind == 1) m->they_hold = m->to_check = 1; else m->to_give += many;
                label = !strcmp(canon, "Obj_Heart_Piece") ? "a Piece of Heart" : !strcmp(canon, "Obj_Heart_Container") ? "a Heart Container"
                      : !strncmp(canon, "Item_", 5) ? canon + 5 : canon;
            }
            if (!quiet[0] && kind == 1) co_note("%s found %s", who, label);   /* (a find worth a line: not every rupee and seed) */
            else if (!quiet[0] && (!strcmp(canon, "Obj_Heart_Piece") || !strcmp(canon, "Obj_Heart_Container"))) co_note("%s found %s", who, label);
        } else if (text[0] == 'K' && text[1] == ' ') {
            const char *at = text + 2;
            while (*at) {
                char *end = NULL;
                const unsigned long long key = strtoull(at, &end, 16);
                if (end == at) break;
                if (key) co_ktheirs_add(key);
                at = end;
            }
        } else if ((text[0] == 'N' || text[0] == 'V') && text[1] == ' ') {
            unsigned hash = 0;
            int amount = 0;
            if (sscanf(text + 2, "%x %d", &hash, &amount) != 2) continue;
            const int n = co_count_index(hash);
            if (n < 0 || amount < -99 || amount > 99) continue;
            if (text[0] == 'N') co_count_add[n] += amount;
            else if (list[e].from == 0) co_count_set[n] = amount, co_count_has_set[n] = 1, co_count_add[n] = 0;   /* (the host's word only) */
        } else if ((text[0] == 'E' || text[0] == 'S') && text[1] == ' ') {   /* (test runs only: equipment tried on the bodies) */
            const char *e = getenv("HOS_COOP_TEST");
            if (e && !strcmp(e, "1")) snprintf(co_test_equip_line, sizeof co_test_equip_line, "%s", text), InterlockedExchange(&co_test_equip, 1);
        } else if (text[0] == 'G' && text[1] == ' ') {     /* (test runs only: travel to this map) */
            const char *e = getenv("HOS_COOP_TEST");
            if (e && !strcmp(e, "1")) snprintf(co_test_go, sizeof co_test_go, "%s", text + 2);
        } else if (text[0] == 'A' && text[1] == ' ') {     /* (test runs only: play this animation command on the bodies) */
            const char *e = getenv("HOS_COOP_TEST");
            int slot = 0;
            char name[CO_AS_NAME] = "";
            if (e && !strcmp(e, "1") && sscanf(text + 2, "%d %47s", &slot, name) == 2) {
                co_test_play_slot = slot;
                snprintf(co_test_play_name, sizeof co_test_play_name, "%s", name);
                InterlockedExchange(&co_test_play, 1);
            }
        } else if (text[0] == 'Z' && text[1] == ' ') {     /* (test runs only: make this actor beside Link) */
            static int allowed = -1;
            if (allowed < 0) { const char *e = getenv("HOS_COOP_TEST"); allowed = e && !strcmp(e, "1"); }
            int option = 0;
            if (allowed && sscanf(text + 2, "%95s %d", co_test_name, &option) >= 1) co_test_option = option, InterlockedExchange(&co_test_spawn, 1);
        } else if (text[0] == 'M' && text[1] == ' ') {
            const int quarters = atoi(text + 2);
            if (quarters > co_their_quarters && quarters <= 400) co_their_quarters = quarters;
        } else if (text[0] == 'F' && text[1] == ' ') {
            const char *at = text + 2;
            while (*at) {
                char *end = NULL;
                const unsigned long hash = strtoul(at, &end, 16);
                if (end == at) break;
                if (hash) co_theirs_add((uint32_t)hash);
                at = end;
            }
        }
    }
    free(list);
}

static void co_status(Ctx *c, int64_t now, const char *map, int playing) {
    static char last[200], last_list[700];
    char text[200], list[700] = "";                    /* (the list: "Name: where", a line each, in the peers' order: co_goto_ask counts the same way) */
    static char last_hearts[200];
    char hearts[200] = "";                             /* (Navi) the party panel: "<life> <max>" a line each, in the list's order */
    size_t hearted = 0;
    size_t listed = 0;
    const int state = (int)co.state;
    char why[72], advice[200];
    EnterCriticalSection(&co.lock);
    snprintf(why, sizeof why, "%s", co.why);
    snprintf(advice, sizeof advice, "%s", co.advice);
    co.advice[0] = 0;                                   /* (said once) */
    LeaveCriticalSection(&co.lock);
    if (advice[0] && co.want) co_note("%s", advice);
    if (!co.want) snprintf(text, sizeof text, "Not connected");
    else if (!co.thread) snprintf(text, sizeof text, "Not connected: Windows' network library did not load");
    else if (state == 4) snprintf(text, sizeof text, "Not connected: %s", why[0] ? why : "this PC cannot host on that port");
    else if (state == 2 && why[0]) snprintf(text, sizeof text, "Not connected: %s (trying again)", why);
    else if (state == 2) snprintf(text, sizeof text, "Connecting...");
    else {
        size_t used = (size_t)snprintf(text, sizeof text, "Connected: ");
        int others = 0;
        float mine[3];
        EnterCriticalSection(&co.lock);
        memcpy(mine, co.pos, sizeof mine);
        for (int i = 0; i < CO_PEERS; i++) {
            const CoPeer *p = &co.peer[i];
            if (!p->used) continue;
            char words[96], where[112];
            co_map_words(p->map, words, sizeof words);
            if (!p->map[0] || now - p->seen > 3000000000LL) snprintf(where, sizeof where, "not in the game yet");
            else if (playing && !strcmp(p->map, map)) {
                const float dx = p->pos[0] - mine[0], dy = p->pos[1] - mine[1], dz = p->pos[2] - mine[2];
                snprintf(where, sizeof where, "here, %.0f m away", sqrtf(dx * dx + dy * dy + dz * dz));
            } else snprintf(where, sizeof where, "%s", words);
            if (used <= sizeof text - 96) used += (size_t)snprintf(text + used, sizeof text - used, "%s%s: %s", others ? "   " : "", p->name, where);
            if (listed < sizeof list - 150) {
                listed += (size_t)snprintf(list + listed, sizeof list - listed, "%s: %s\n", p->name, where);
                if (hearted < sizeof hearts - 24) hearted += (size_t)snprintf(hearts + hearted, sizeof hearts - hearted, "%d %d\n", p->life, p->life_max);
            }
            others++;
        }
        LeaveCriticalSection(&co.lock);
        if (!others) snprintf(text, sizeof text, state == 1 ? "Hosting on port %d: nobody has joined yet" : "Connected: waiting for the others", (int)co.port);
    }
    (void)c;
    if (strcmp(text, last)) snprintf(last, sizeof last, "%s", text), co_menu_text(0, text);
    if (strcmp(list, last_list)) snprintf(last_list, sizeof last_list, "%s", list), co_menu_text(4, list);
    if (strcmp(hearts, last_hearts)) snprintf(last_hearts, sizeof last_hearts, "%s", hearts), co_menu_text(5, hearts);   /* (Navi) */
}

/* ---- go to a player ----
   The NETWORK tab has a "Go to player" row for each of the others. In the same map it is a step: Link is stood
   beside them, by the call the debug tools' "return to the mark" makes (the controller's change-position, hos_main.c
   oot_update_player_cheats). In another map it is the debug tools' teleport to that map first (hos_oot_travel),
   then the step once the map is in. Not during an event or a load, as everything else here. */
int hos_oot_travel(Ctx *c, uint64_t actor, const char *banc);   /* hos_main.c */
int hos_oot_travel_to(Ctx *c, uint64_t actor, const char *banc, const char *start);   /* hos_main.c (Navi) */
static int co_game_busy(void);

/* (Navi) Where to arrive in another map: <game>\coop\starts.txt (nx2/coop_starts.py), each map's plain ways in, "<banc> <name>
   <x> <y> <z>" a line. The one nearest the other player is asked for, so the step to their side that follows is a short one,
   and a map host/hos_main.c's table does not list can be travelled into too. Without the file, the table's way in. */
typedef struct { char banc[64], name[96]; float pos[3]; } CoStart;
static CoStart *co_starts;
static size_t co_starts_n;

static void co_starts_load(void) {
    static int loaded;
    if (loaded) return;
    loaded = 1;
    char game[MAX_PATH], path[MAX_PATH + 32], line[256];
    co_game_dir(game, sizeof game);
    snprintf(path, sizeof path, "%s\\coop\\starts.txt", game);
    FILE *f = fopen(path, "r");
    if (!f) { hos_log("coop: %s is not there: go to a player travels by the teleport's own list (nx2/coop_starts.py makes it)", path); return; }
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        CoStart s;
        if (line[0] == '#' || sscanf(line, "%63s %95s %f %f %f", s.banc, s.name, &s.pos[0], &s.pos[1], &s.pos[2]) != 5) continue;
        if (co_starts_n == cap) {
            CoStart *more = realloc(co_starts, (cap = cap ? cap * 2 : 256) * sizeof *co_starts);
            if (!more) break;
            co_starts = more;
        }
        co_starts[co_starts_n++] = s;
    }
    fclose(f);
    hos_log("coop: go to a player: %u places to arrive at", (unsigned)co_starts_n);
}

/* the way into banc nearest at (flat distance, then height), or NULL */
static const char *co_start_near(const char *banc, const float at[3]) {
    co_starts_load();
    const CoStart *best = NULL;
    float best_d = 0;
    for (size_t i = 0; i < co_starts_n; i++) {
        const CoStart *s = &co_starts[i];
        if (strcmp(s->banc, banc)) continue;
        const float dx = s->pos[0] - at[0], dy = s->pos[1] - at[1], dz = s->pos[2] - at[2];
        const float d = dx * dx + dz * dz + 4.0f * dy * dy;   /* (another floor counts for more) */
        if (!best || d < best_d) best = s, best_d = d;
    }
    return best ? best->name : NULL;
}
static struct { int id, stage; int64_t since, settle; char name[24]; } co_go;   /* stage 1 asked, 2 the map is changing */

static void co_goto_ask(int which) {
    int n = 0, found = 0;
    EnterCriticalSection(&co.lock);
    for (int i = 0; i < CO_PEERS && !found; i++) {
        if (!co.peer[i].used || n++ != which) continue;
        co_go.id = co.peer[i].id;
        snprintf(co_go.name, sizeof co_go.name, "%s", co.peer[i].name);
        found = 1;
    }
    LeaveCriticalSection(&co.lock);
    if (!found) return;
    co_go.stage = 1, co_go.since = hos_now_ns(), co_go.settle = 0;
    co_note("Going to %s...", co_go.name);
}

static int co_place(Ctx *c, uint64_t link, const float at[3]) {
    static const unsigned step[] = {80, 32, 16, 8};    /* (Link's components to his controller) */
    uint64_t controller = hos_guest_range(link + 624, 8) ? G64(link + 624) : 0;
    for (unsigned i = 0; i < 4 && controller; i++) controller = hos_guest_range(controller + step[i], 8) ? G64(controller + step[i]) : 0;
    A64Fn change = a64_lookup(0x806d2530ULL);
    if (!change || !hos_guest_range(controller, 8) || G64(controller) != 0x84c54f10ULL || G64(0x84c54f10ULL + 0xe8) != 0x806d2530ULL) return 0;
    const Ctx saved = *c;
    const uint64_t arg = c->x[31] - 16;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t bits;
        memcpy(&bits, &at[i], 4);
        recomp_store32_sp(c, arg + i * 4, bits);
    }
    recomp_store32_sp(c, arg + 12, 0);
    c->x[31] = arg;
    c->x[0] = controller, c->x[1] = arg;
    change(c);
    *c = saved;
    return 1;
}

static void co_goto_step(Ctx *c, uint64_t link, int64_t now) {
    if (!co_go.stage) return;
    char map[64] = "", words[96];
    float at[3] = {0, 0, 0}, yaw = 0;
    int there = 0;
    EnterCriticalSection(&co.lock);
    const CoPeer *p = co_peer(co_go.id, 0);
    if (p && p->map[0] && now - p->seen < 3000000000LL) {
        there = 1, yaw = p->yaw;
        snprintf(map, sizeof map, "%s", p->map);
        memcpy(at, p->pos, sizeof at);
    }
    LeaveCriticalSection(&co.lock);
    if (!there) { co_go.stage = 0; co_note("%s is not in the game right now", co_go.name); return; }
    if (now - co_go.since > 45000000000LL) { co_go.stage = 0; co_note("Could not get to %s", co_go.name); return; }
    if (!co_map[0] || co_game_busy()) { co_go.settle = now + 1500000000LL; return; }
    if (now < co_go.settle) return;
    co_map_words(map, words, sizeof words);
    if (!strcmp(map, co_map)) {
        at[0] -= sinf(yaw) * 0.8f, at[2] -= cosf(yaw) * 0.8f, at[1] += 0.05f;   /* (a step behind them, not inside them) */
        const int ok = co_place(c, link, at);
        co_go.stage = 0;
        co_note(ok ? "You are with %s" : "Could not move to %s", co_go.name);
        hos_log("coop: go to %s: placed at %.1f %.1f %.1f in %s (%d)", co_go.name, at[0], at[1], at[2], map, ok);
        return;
    }
    if (co_go.stage == 2) {                             /* the map came in, and it is not theirs (they moved on, or the other age's) */
        if (now - co_go.since < 20000000000LL) return;
        co_go.stage = 0;
        co_note("%s is in %s now", co_go.name, words);
        return;
    }
    /* (Navi) the way into their map nearest to them, when they are the same age (a map's name says its age; the other age's
       map would load the wrong Link), else the teleport's own way in for this age */
    const int child = co_my_child(c);   /* (Navi) */
    int their_child = child;
    EnterCriticalSection(&co.lock);
    { const CoPeer *q = co_peer(co_go.id, 0); if (q) their_child = q->child; }
    LeaveCriticalSection(&co.lock);
    const char *start = their_child == child ? co_start_near(map, at) : NULL;
    const int asked = start ? hos_oot_travel_to(c, link, map, start) : hos_oot_travel(c, link, map);   /* (0 from either: not now) */
    hos_log("coop: go to %s: travel to %s at %s (%d)", co_go.name, map, start && asked ? start : "the teleport's way in", asked);
    if (asked == 1) co_go.stage = 2, co_go.settle = now + 3000000000LL;
    else {
        co_go.stage = 0;
        if (asked < 0) co_note("%s is in %s: there is no way to travel into there, only to walk", co_go.name, words);
        else co_note("Cannot travel just now: try again in a moment");
    }
}

/* ── the other players, drawn ────────────────────────────────────────────────
 *
 * The game makes an actor by name through one function, f_82c4b220 (the one that names itself
 * "createActorAndConnectSync" to the engine; its only caller, at 82ebbf18, makes a horse's rider):
 *     f_82c4b220(ActorMgr, make, 0, &name, &arg, 0, 1, 0; on the stack: 0, 0, set-up, 0, 0, 0,
 *                "createActorAndConnectSync", a link for the new actor or 0, 8)
 *   ActorMgr  *(*0x84c9f3e0)
 *   make      f_82ec15e0: the manager's own "make it and start it" (f_8080b990, then f_83acfe30)
 *   set-up    f_82ec16c0: does nothing
 *   arg       as the caller fills it (82eb9fd0 on) and f_82c4be30 reads it: +0 place, +0xc a 3x3 turn,
 *             +0x30 size, +0x40 a text (must point at one, an empty one will do: it is read without a
 *             check, and 0 there was this code's first crash), +0x58..0x98 zero (+0x60 a list of values to
 *             give the actor: none), +0x98 which of place (1), turn (2) and size (4) are given
 * It answers with the actor. An actor keeps its place at +0x31c, its turn at +0x328 and its size at +0x34c
 * (the same caller reads them from the rider's horse; Link's are there too: hos_main.c); co_body_put moves one.
 *
 * The other players are PouchPlayer_Child and PouchPlayer_Adult: the Link the inventory screen shows, a
 * Link with nothing driving him. One is made for each player on this map and moved to where they say they
 * are; one whose player left the map is put away under the ground and used again. A map change takes all of
 * them with it (the game's own clean-up), which is seen as Link himself being another actor. */
#define CO_CREATE_ACTOR 0x82c4b220ULL
#define CO_CREATE_MAKE  0x82ec15e0ULL
#define CO_CREATE_SETUP 0x82ec16c0ULL
#define CO_CREATE_NAME  0x83dfa34aULL
#define CO_ACTORMGR_SLOT 0x84c9f3e0ULL
#define CO_ACTOR_PLACE 0x31c
#define CO_ACTOR_TURN  0x328

static void co_turn(float yaw, float m[9]) {           /* (hos_main.c oot_write_flight_rotation: about the up axis) */
    const float sn = sinf(yaw), cs = cosf(yaw);
    const float t[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs};
    memcpy(m, t, sizeof t);
}

static uint64_t co_create(Ctx *c, const char *name, const float place[3], float yaw, int option) {
    A64Fn create = a64_lookup(CO_CREATE_ACTOR);
    const uint64_t holder = G64(CO_ACTORMGR_SLOT);
    const uint64_t mgr = hos_guest_range(holder, 8) ? G64(holder) : 0;
    if (!create || !hos_guest_range(mgr, 0x110) || strlen(name) >= 0x60) return 0;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x500) & ~0xfULL;   /* the stack arguments, then our own blocks above them */
    const uint64_t arg = frame + 0x80, name_ref = frame + 0x2c0, name_at = frame + 0x2d0, list = frame + 0x340;
    memset(A64_PTR(frame), 0, 0x400);
    float turn[9];
    const float size[3] = {1, 1, 1};
    co_turn(yaw, turn);
    memcpy(A64_PTR(arg), place, 12);
    memcpy(A64_PTR(arg + 0xc), turn, sizeof turn);
    memcpy(A64_PTR(arg + 0x30), size, sizeof size);
    *(uint8_t *)A64_PTR(arg + 0x48) = (uint8_t)(option & 1);
    recomp_store64_sp(c, arg + 0x40, list);              /* (a text the actor may be given: an empty one. Not 0: it is read) */
    *(uint8_t *)A64_PTR(arg + 0x98) = 7;
    strcpy((char *)A64_PTR(name_at), name);
    recomp_store64_sp(c, name_ref, name_at);
    recomp_store64_sp(c, frame + 0x10, CO_CREATE_SETUP);
    recomp_store64_sp(c, frame + 0x30, CO_CREATE_NAME);
    recomp_store64_sp(c, frame + 0x40, 8);
    c->x[31] = frame;
    c->x[0] = mgr, c->x[1] = CO_CREATE_MAKE, c->x[2] = 0, c->x[3] = name_ref, c->x[4] = arg, c->x[5] = 0, c->x[6] = 1, c->x[7] = 0;
    create(c);
    const uint64_t actor = c->x[0];
    *c = saved;
    return hos_guest_range(actor, 0x360) ? actor : 0;
}

typedef struct { uint8_t hand_set[8], hand_tries[8]; uint32_t hand_id[8]; int64_t hand_next[8]; char held[CO_GEAR_NAME]; uint8_t held_tries;
                 uint64_t actor, made_of; int child, posed, foe, remake; char look[12]; uint32_t life_full; int64_t tried, struck; float push[2], push_far;
                 char worn[CO_GEAR][CO_GEAR_NAME], try_name[CO_GEAR][CO_GEAR_NAME]; uint8_t wear_tries[CO_GEAR]; int64_t wear_next; } CoBody;
static CoBody co_body[CO_PEERS];
static uint64_t co_link_actor;                        /* Link's own actor when the bodies were made */

/* Moving one. Writing its place (+0x31c) moves nothing: the picture stays where it was made. The game moves an actor
   by asking (f_80c8fd60, what its OneShotActorRequestSetMtx script node calls): the place wanted goes to +0x358 and
   the turn wanted to +0x364, and the actor takes them at its next update and marks them taken (the first value of
   each becomes "not a number"). So the first value of each is written last: the rest is in place when it is seen. */
#define CO_ACTOR_ASK_PLACE 0x358
#define CO_ACTOR_ASK_TURN  0x364
static void co_body_put(uint64_t actor, const float place[3], float yaw) {
    float turn[9];
    co_turn(yaw, turn);
    uint8_t *a = (uint8_t *)GP(actor);
    memcpy(a + CO_ACTOR_ASK_TURN + 4, turn + 1, 32);
    memcpy(a + CO_ACTOR_ASK_PLACE + 4, place + 1, 8);
    memcpy(a + CO_ACTOR_ASK_TURN, turn, 4);
    memcpy(a + CO_ACTOR_ASK_PLACE, place, 4);
    HOS_WRITTEN(a + CO_ACTOR_ASK_PLACE, 48);
}

/* ---- how a player moves: the animation ----
   An actor's animation controller is at actor+0x270 -> +0x40 -> +0x18 -> +0x18 (as the game's own "play this" request,
   f_802222a0, walks to it). It has slots: their count at +0x18, an array of 0x48-byte entries at +0x20 whose first
   word is the slot; a slot's +0x10 is the name of the command it plays now (a text, "" for none).
   The game plays a command by name with f_802228f0(controller, &name, slot, 0, 0, 0, 0, 0; s0 a blend the caller
   reads from a global, s1 -1): what f_802222a0 ends in, after a trace call this side has no use for. */
static uint64_t co_as_ctl(uint64_t actor) {
    static const unsigned step[] = {0x270, 0x40, 0x18, 0x18};
    uint64_t at = actor;
    for (unsigned i = 0; i < 4 && at; i++) at = co_ok(at + step[i], 8) ? G64(at + step[i]) : 0;
    return co_ok(at, 0x138) ? at : 0;
}

static int co_as_names(uint64_t actor, char names[CO_AS_SLOTS][CO_AS_NAME]) {
    const uint64_t ctl = co_as_ctl(actor);
    if (!ctl) return 0;
    const uint32_t count = G32(ctl + 0x18);
    const uint64_t slots = G64(ctl + 0x20);
    if (!count || count > 64 || !co_ok(slots, count * 0x48ULL)) return 0;
    int n = 0;
    for (uint32_t i = 0; i < count && n < CO_AS_SLOTS; i++, n++) {
        memset(names[n], 0, CO_AS_NAME);
        const uint64_t slot = G64(slots + i * 0x48ULL);
        const uint64_t name = co_ok(slot, 0x18) ? G64(slot + 0x10) : 0;
        if (!co_ok(name, CO_AS_NAME)) continue;
        const char *text = (const char *)GP(name);
        size_t len = 0;
        while (len < CO_AS_NAME - 1 && text[len] > 0x20 && text[len] < 0x7f) len++;
        if (text[len]) continue;                        /* (not a short plain word: not a name) */
        memcpy(names[n], text, len + 1);
    }
    return n;
}

static int co_as_play(Ctx *c, uint64_t actor, int slot, const char *name) {
    const uint64_t ctl = co_as_ctl(actor);
    A64Fn play = a64_lookup(0x802228f0ULL);
    if (!ctl || !play || slot < 0 || (uint32_t)slot >= G32(ctl + 0x18) || strlen(name) >= CO_AS_NAME) return -1;
    const uint64_t global = G64(0x84eb4b20ULL);
    float blend = 1.0f;
    if (hos_guest_range(global, 4)) memcpy(&blend, GP(global), 4);
    const float from = -1.0f;
    uint32_t bits[2];
    memcpy(&bits[0], &blend, 4), memcpy(&bits[1], &from, 4);
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x80) & ~0xfULL;
    memset(A64_PTR(frame), 0, 0x80);
    memcpy(A64_PTR(frame + 0x10), name, strlen(name) + 1);
    recomp_store64_sp(c, frame, frame + 0x10);
    c->x[31] = frame;
    c->x[0] = ctl, c->x[1] = frame, c->x[2] = (uint64_t)slot, c->x[3] = 0, c->x[4] = 0, c->x[5] = 0, c->x[6] = 0, c->x[7] = 0;
    c->vreg[0][0] = bits[0], c->vreg[0][1] = 0, c->vreg[1][0] = bits[1], c->vreg[1][1] = 0;
    play(c);
    const int ok = (int)(c->x[0] & 1);
    *c = saved;
    return ok;
}

/* A slot plays nothing any more: what the script node OneShotASRemoveNoMorph (f_828cf2d0) calls, f_837d5c10(controller,
   slot, 0; d0 0). */
static int co_as_remove(Ctx *c, uint64_t actor, int slot) {
    const uint64_t ctl = co_as_ctl(actor);
    A64Fn remove = a64_lookup(0x837d5c10ULL);
    if (!ctl || !remove || slot < 0 || (uint32_t)slot >= G32(ctl + 0x18)) return -1;
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x40) & ~0xfULL;
    c->x[0] = ctl, c->x[1] = (uint64_t)slot, c->x[2] = 0;
    c->vreg[0][0] = 0, c->vreg[0][1] = 0;
    remove(c);
    const int result = (int)(c->x[0] & 0xff);
    *c = saved;
    return result;
}

/* An actor's animation inputs: where they start and how many 32-bit words (see "how another player moves"). */
static uint64_t co_inputs_at(uint64_t actor, uint32_t *words) {
    const uint64_t ctl = co_as_ctl(actor);
    if (!ctl || G64(ctl) != 0x84c497c8ULL) return 0;
    const uint64_t holder = G64(ctl + 0x40);
    if (!co_ok(holder, 0x110) || G64(holder) != 0x84c49818ULL) return 0;
    uint64_t lo = ~0ULL, hi = 0;
    for (unsigned o = 0x10; o <= 0xc8; o += 8) {
        const uint64_t at = G64(holder + o);
        if (!at) continue;
        if (at < holder + 0x100 || at > holder + 0x4000) return 0;   /* (not inside the object: not the layout this knows) */
        if (at < lo) lo = at;
        if (at + 8 > hi) hi = at + 8;
    }
    if (lo >= hi || ((hi - lo) & 3) || (hi - lo) / 4 > CO_INPUTS || !co_ok(lo, hi - lo)) return 0;
    /* (Navi) readable now, as Windows has it: co_ok takes any heap address below what the heap has used, but the heap is
       committed on first touch and an actor's memory goes at a map change, so a read here could fault in the game's thread
       (a joiner crashed in co_pose_learn so, 2026-11-14). ReadProcessMemory fails where a read would fault. The stretch
       read is 8-byte aligned at both ends: co_input_is_address reads the 8 bytes about each word */
    static _Thread_local uint8_t probe[CO_INPUTS * 4 + 16];
    const uint64_t from = lo & ~7ULL, to = (hi + 7) & ~7ULL;
    SIZE_T got = 0;
    if (to - from > sizeof probe || !ReadProcessMemory(GetCurrentProcess(), GP(from), probe, (SIZE_T)(to - from), &got) ||
        got != to - from) return 0;
    *words = (uint32_t)((hi - lo) / 4);
    return lo;
}

/* Is the word at this place half of an address? (an 8-byte value, on an 8-byte boundary, that points into the game) */
static int co_input_is_address(uint64_t word_at) {
    const uint64_t v = G64(word_at & ~7ULL);
    return (v >= HOS_HEAP_BASE && v < HOS_HEAP_BASE + 0x400000000ULL) || (v >> 32) == 0x7f || (v >= 0x80000000ULL && v < 0x90000000ULL && (v & 3) == 0 && v > 0x80001000ULL);
}

/* ---- the client's own animation: a pose library learned from my own Link ----
   Another player's body is no longer left to what their game says it plays (T, Y): that stream only fits a body whose
   graph is the sender's, and when it does not arrive, or does not fit, the body stood with its arms held out (a T pose).
   This game now works out what a body should do from what it can SEE, the way it moves:
     * It watches MY Link while I play. Whenever I have been going at one speed for a moment (standing, walking, running,
       on the ground, not in a cutscene), it keeps what my Link plays in every slot and the animation inputs the game set
       for it, in a bin for that speed, for my age (child or adult).
     * A body is given the pose of the bin nearest the speed it is seen to move at (the place its player says, over the
       time between two sayings): the same commands in the same slots, and the same inputs, so the legs go as fast as the
       body does. Nothing is learned from the other player's game; nothing here depends on what theirs plays.
   The sender's own stream is still used for what the library has no pose for (a jump, a sword swing, a hit) and until
   a pose has been learned (move about for a few seconds, and the library fills). */
#define CO_BINS 8
typedef struct { int valid, slots; uint32_t words; float speed; char name[CO_AS_SLOTS][CO_AS_NAME]; uint32_t value[CO_INPUTS]; uint8_t have[CO_INPUTS]; } CoPose;
static CoPose co_lib[2][CO_BINS];                 /* [child][speed bin]: bin 0 stands still */
static const float co_bin_edge[CO_BINS - 1] = {0.25f, 0.8f, 1.6f, 2.6f, 3.8f, 5.2f, 7.0f};
static int co_game_busy(void);

static int co_bin_of(float speed) {
    int b = 0;
    while (b < CO_BINS - 1 && speed >= co_bin_edge[b]) b++;
    return b;
}

/* a command that is not ground movement is not learned from (a swing, a jump, a hit, a ladder): the body would then play
   it for as long as the other player walks */
static int co_not_movement(const char *name) {
    static const char *const words[] = {"Cut", "Charge", "Attack", "Jump", "Damage", "Fall", "Land", "Roll", "Climb", "Swim", "Dive", "Die", "Dead", "Hang", "Slip", "Ladder", "Talk", "Event", "Demo", "Item", "Shoot", "Aim", "Guard"};
    for (unsigned i = 0; i < sizeof words / sizeof *words; i++)
        if (strstr(name, words[i])) return 1;
    return 0;
}

/* (about thirty times a second, from co_tick) watch my own Link and keep a pose for the speed it keeps */
static void co_pose_learn(uint64_t link, int child) {
    static float last[3], speed, rise;
    static int64_t last_at, bin_since, kept_at;
    static int have_last, bin_was = -1;
    const int64_t now = hos_now_ns();
    float p[3];
    EnterCriticalSection(&co.lock);
    memcpy(p, co.pos, sizeof p);
    LeaveCriticalSection(&co.lock);
    if (have_last && now > last_at) {
        const float dt = (float)(now - last_at) * 1e-9f;
        if (dt > 0.5f) speed = 0, rise = 0, bin_was = -1;
        else {
            float rs = sqrtf((p[0] - last[0]) * (p[0] - last[0]) + (p[2] - last[2]) * (p[2] - last[2])) / dt;
            const float rv = (p[1] - last[1]) / dt;
            if (rs > 40.0f) rs = speed, bin_was = -1;          /* (a jump to another place is not a speed) */
            speed += (rs - speed) * 0.35f, rise += (rv - rise) * 0.35f;
        }
    }
    memcpy(last, p, sizeof last), last_at = now, have_last = 1;
    const int bin = co_bin_of(speed);
    if (bin != bin_was) bin_was = bin, bin_since = now;
    if (now - bin_since < 400000000LL || now - kept_at < 150000000LL || fabsf(rise) > 1.0f || co_game_busy()) return;
    char names[CO_AS_SLOTS][CO_AS_NAME];
    const int slots = co_as_names(link, names);
    if (slots < 1 || !names[0][0] || co_not_movement(names[0])) return;
    uint32_t words = 0;
    const uint64_t at = co_inputs_at(link, &words);
    if (!at || !words) return;
    CoPose *q = &co_lib[child ? 1 : 0][bin];
    if (!q->valid) hos_log("coop: anim: learned a %s pose for speed bin %d (%.1f/s): slot 0 plays '%s', %u input words", child ? "child" : "adult", bin, speed, names[0], words);
    memset(q, 0, offsetof(CoPose, value));
    q->valid = 1, q->slots = slots, q->words = words, q->speed = speed;
    for (int i = 0; i < slots; i++) memcpy(q->name[i], names[i], CO_AS_NAME);
    for (uint32_t i = 0; i < words; i++) {
        if (co_input_is_address(at + i * 4ULL)) { q->have[i] = 0; continue; }
        q->value[i] = G32(at + i * 4ULL), q->have[i] = 1;
    }
    kept_at = now;
}

/* the pose for a body of this age seen moving at this speed (words: its graph's input size, 0 when not known); NULL when
   none was learned for that kind of movement. Standing is bin 0 alone, and moving is never given the standing pose. */
static const CoPose *co_pose_pick(int child, float speed, uint32_t words) {
    const CoPose *best = NULL;
    float gap = 1e9f;
    const int standing = speed < co_bin_edge[0];
    for (int b = 0; b < CO_BINS; b++) {
        const CoPose *q = &co_lib[child ? 1 : 0][b];
        if (!q->valid || (words && q->words != words) || (b == 0) != standing) continue;
        const float d = fabsf(q->speed - speed);
        if (d < gap) gap = d, best = q;
    }
    return best;
}

/* does the command belong to ground movement, as far as what has been learned says */
static int co_is_movement(int child, const char *name) {
    for (int b = 0; b < CO_BINS; b++)
        if (co_lib[child ? 1 : 0][b].valid && !strcmp(co_lib[child ? 1 : 0][b].name[0], name)) return 1;
    return 0;
}

/* (with my place, thirty times a second) what my Link plays and his inputs, for the others */
static void co_anim_say(uint64_t link) {
    static char said_name[CO_AS_SLOTS][CO_AS_NAME];
    static uint8_t said_slot[CO_AS_SLOTS];
    static uint32_t said_value[CO_INPUTS], said_words, turn;
    static uint8_t said[CO_INPUTS];
    char names[CO_AS_SLOTS][CO_AS_NAME];
    const int slots = co_as_names(link, names);
    turn++;
    for (int i = 0; i < slots; i++) {
        if (said_slot[i] && !strcmp(said_name[i], names[i]) && turn % 60 != (uint32_t)i) continue;
        memcpy(said_name[i], names[i], CO_AS_NAME), said_slot[i] = 1;
        co_send("T %d %s", i, names[i][0] ? names[i] : "-");
    }
    uint32_t words = 0;
    const uint64_t at = co_inputs_at(link, &words);
    if (!at) return;
    if (words != said_words) memset(said, 0, sizeof said), said_words = words;
    uint8_t raw[660];
    char text[900];
    size_t used = 0;
    for (uint32_t i = 0; i < words; i++) {
        if (co_input_is_address(at + i * 4ULL)) continue;
        const uint32_t v = G32(at + i * 4ULL);
        if (said[i] && said_value[i] == v && i % 120 != turn % 120) continue;
        if (used + 6 > sizeof raw) {
            co_base64(raw, used, text);
            co_send("Y %u %s", words, text);
            used = 0;
        }
        const uint16_t word = (uint16_t)i;
        memcpy(raw + used, &word, 2), memcpy(raw + used + 2, &v, 4), used += 6;
        said_value[i] = v, said[i] = 1;
    }
    if (used) {
        co_base64(raw, used, text);
        co_send("Y %u %s", words, text);
    }
}

/* ---- what my Link wears, for the others (B) ----
   The game keeps what a player has on as String64 save values, PlayerStatus.Equip*: the same GameDataMgr as the bools and
   ints above, one more table. The String64 block is at GameDataMgr+0x610 (count, entries, (hash, index) table, its size),
   with 0x108-byte entries holding the text (a pointer at +0x68, its size at +0x70).
   Found by daurosk's oot-remake-multiplayer (GPL-3.0; its PLAYER_EQUIP and puppet gear: the slot kinds 0, 6, 7, 8, 9 for
   weapon, shield, head, armour, boots, and this table's layout), which proved the putting on in a game for sword and shield.
   Not known from there, and tried here: the names of the values (several spellings are asked for each) and exactly where
   the text sits in an entry (read as a pointer, else as text in the entry). The sword and shield are also looked for by
   their text if no name is found. hookshot.log says what was found: lines starting "coop: gear:". */
typedef struct { uint64_t table, entries; uint32_t size, count; } CoStr;
#define CO_STR64_AT 0x610
#define CO_STR64_ENTRY 0x108

static const int co_gear_slot[CO_GEAR] = {0, 6, 7, 8, 9};      /* the equipment user's slot kinds */
static const char *const co_gear_kind[CO_GEAR] = {"weapon", "shield", "head", "armour", "boots"};
static const char *const co_gear_flag[CO_GEAR][7] = {
    {"PlayerStatus.EquipWeapon", "PlayerStatus.EquipSword", "PlayerStatus.EquipWeaponName", "PlayerStatus.EquipSwordName", NULL},
    {"PlayerStatus.EquipShield", "PlayerStatus.EquipShieldName", NULL},
    {"PlayerStatus.EquipHeadArmor", "PlayerStatus.EquipHead", "PlayerStatus.EquipMask", NULL},   /* (Navi: EquipHeadArmor is the game's: a mask) */
    {"PlayerStatus.EquipArmor", NULL},                       /* (Navi: the game's, in its GameDataList: a tunic) */
    {"PlayerStatus.EquipBoot", "PlayerStatus.EquipBoots", NULL},   /* (Navi: EquipBoot is the game's) */
};

static int co_cstr(uint64_t addr, char *out, size_t cap) {    /* a name at a guest address; 0 when what is there is not one */
    size_t n = 0;
    for (; n + 1 < cap; n++) {
        if (!co_ok(addr + n, 1)) return 0;
        const char ch = *(const char *)GP(addr + n);
        if (!ch) break;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-')) return 0;
        out[n] = ch;
    }
    out[n] = 0;
    return n > 0 && n + 1 < cap;
}

static int co_string64_open(CoStr *s) {
    const uint64_t holder = G64(CO_GAMEDATA_SLOT);
    if (!co_ok(holder, 8)) return 0;
    const uint64_t mgr = G64(holder);
    if (!co_ok(mgr, CO_STR64_AT + 0x40)) return 0;
    s->count = G32(mgr + CO_STR64_AT), s->entries = G64(mgr + CO_STR64_AT + 8);
    s->table = G64(mgr + CO_STR64_AT + 16), s->size = G32(mgr + CO_STR64_AT + 28);
    return s->size && s->count && s->size < (1u << 20) && s->count < (1u << 20) && co_ok(s->table, s->size * 8ULL) &&
           co_ok(s->entries, s->count * (uint64_t)CO_STR64_ENTRY);
}

static int co_string64_find(const CoStr *s, uint32_t hash) {   /* the entry's number, -1 when there is none */
    for (uint32_t probe = 0, slot = hash % s->size; probe < s->size; probe++, slot = (slot + 1) % s->size) {
        const uint32_t key = G32(s->table + slot * 8ULL);
        if (!key) return -1;
        if (key != hash) continue;
        const uint32_t i = G32(s->table + slot * 8ULL + 4);
        return i < s->count ? (int)i : -1;
    }
    return -1;
}

static int co_string64_text(const CoStr *s, int i, char *out, size_t cap) {
    static const unsigned at[] = {0x68, 0x70, 0x78, 0x80};
    const uint64_t entry = s->entries + (uint64_t)i * CO_STR64_ENTRY;
    for (size_t k = 0; k < sizeof at / sizeof at[0]; k++) {      /* (a pointer to the text) */
        const uint64_t ptr = G64(entry + at[k]);
        if (ptr > 0x10000 && co_cstr(ptr, out, cap)) return 1;
    }
    return co_cstr(entry + 0x68, out, cap);                     /* (the text in the entry itself) */
}

/* what is worn in piece k: 1 and its name in out, or 0 */
static int co_gear_read(const CoStr *s, int k, char *out, size_t cap, const char **how) {
    for (int j = 0; co_gear_flag[k][j]; j++) {
        const int i = co_string64_find(s, co_mm3(co_gear_flag[k][j]));
        if (i >= 0 && co_string64_text(s, i, out, cap)) { *how = co_gear_flag[k][j]; return 1; }
    }
    if (k > 1) return 0;
    const char *const prefix = k == 0 ? "Weapon_Sword_" : "Weapon_Shield_";          /* (not found by name: the first by its text) */
    char text[CO_GEAR_NAME];
    for (uint32_t slot = 0; slot < s->size; slot++) {
        if (!G32(s->table + slot * 8ULL)) continue;
        const uint32_t i = G32(s->table + slot * 8ULL + 4);
        if (i >= s->count || !co_string64_text(s, (int)i, text, sizeof text)) continue;
        if (!strncmp(text, prefix, strlen(prefix))) { snprintf(out, cap, "%s", text); *how = "(by its text)"; return 1; }
    }
    return 0;
}

static void co_gear_dump(const CoStr *s) {               /* (the log, to learn the names from: the values that look like equipment) */
    int shown = 0;
    char text[CO_GEAR_NAME];
    hos_log("coop: gear: %u String64 values", s->count);
    for (uint32_t slot = 0; slot < s->size && shown < 40; slot++) {
        const uint32_t key = G32(s->table + slot * 8ULL);
        if (!key) continue;
        const uint32_t i = G32(s->table + slot * 8ULL + 4);
        if (i >= s->count || !co_string64_text(s, (int)i, text, sizeof text)) continue;
        if (strncmp(text, "Weapon_", 7) && strncmp(text, "Armor_", 6) && strncmp(text, "Item_", 5)) continue;
        hos_log("coop: gear:   value %08x = %s", key, text);
        shown++;
    }
}

/* ---- (Navi) what is in hand ----
   From daurosk's oot-remake-multiplayer (docs/puppet.md "Equipment", coop/host/src/addresses.hpp; GPL-3.0, the same game build,
   its main+X here 0x80020000+X): an actor's EquipmentUser ([actor+0x270]+0x200) has 8 slots at +0x20, 0x18 bytes each (the
   holder +0x8, the piece's id +0x10, ~0 empty; the holder's actor +0x40 and id +0x48; a piece whose state, actor+0x2c, is 7 to 11
   is gone). The game's names for them: 0 the sword, 3 the bow kind (the held item: the slingshot...), 4 the shield. A byte a
   slot at +0x13bc is a REQUEST that lasts a moment: bit 2 put in hand (0x806d6820 writes it), 4 put away (0x80e54c70), 1 none;
   so what is in hand is the last request seen, and a new piece starts put away. The change itself is the game's own call,
   0x80e54c40(user, slot, state): 1 in hand, 2 put away. */
#define CO_EQ_SLOTS 8
static uint64_t co_equipment_user(uint64_t actor);

static uint64_t co_slot_piece(uint64_t user, int i, uint32_t *id_out) {   /* the slot's live piece, 0: none */
    const uint64_t rec = user + 0x20 + 0x18ULL * (uint64_t)i;
    if (!hos_guest_range(rec, 0x18)) return 0;
    const uint32_t id = G32(rec + 0x10);
    if (id == 0xffffffffu) return 0;
    const uint64_t holder = G64(rec + 0x8);
    if (!hos_guest_range(holder, 0x50) || G32(holder + 0x48) != id) return 0;
    const uint64_t piece = G64(holder + 0x40);
    if (!hos_guest_range(piece, 0x268)) return 0;
    const uint32_t state = G32(piece + 0x2c);
    if (!state || (state >= 7 && state <= 11)) return 0;
    if (id_out) *id_out = id;
    return piece;
}

/* my pieces in hand, a bit a slot (P's last field) */
static unsigned co_my_drawn(uint64_t link) {
    static uint32_t memo_id[CO_EQ_SLOTS];
    static uint8_t memo_drawn[CO_EQ_SLOTS];
    const uint64_t user = co_equipment_user(link);
    if (!user || !hos_guest_range(user + 0x13bc, CO_EQ_SLOTS)) return 0;
    unsigned drawn = 0;
    for (int i = 0; i < CO_EQ_SLOTS; i++) {
        uint32_t id = 0;
        if (!co_slot_piece(user, i, &id)) { memo_id[i] = 0, memo_drawn[i] = 0; continue; }
        if (memo_id[i] != id) memo_id[i] = id, memo_drawn[i] = 0;   /* (a new piece: made put away) */
        const uint8_t bits = *(const uint8_t *)GP(user + 0x13bc + (uint64_t)i);
        if (bits & 2) memo_drawn[i] = 1;
        else if (bits & (4 | 1)) memo_drawn[i] = 0;
        if (memo_drawn[i]) drawn |= 1u << i;
    }
    return drawn;
}

/* the name of the piece in my slot i (the held item: slot 3), "" when none */
static void co_slot_name(uint64_t link, int i, char *out, size_t cap) {
    out[0] = 0;
    const uint64_t user = co_equipment_user(link);
    const uint64_t piece = user ? co_slot_piece(user, i, NULL) : 0;
    const uint64_t text = piece ? G64(piece + 0x260) : 0;
    if (!hos_guest_range(text, 1)) return;
    size_t n = 0;
    for (; n + 1 < cap && hos_guest_range(text + n, 1); n++) {
        const char ch = *(const char *)GP(text + n);
        if (!ch) break;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) { n = 0; break; }
        out[n] = ch;
    }
    out[n] = 0;
}

/* the game's change of state for one slot of an actor's equipment: 1 in hand, 2 put away */
static int co_change_state(Ctx *c, uint64_t actor, int slot, int state) {
    const uint64_t user = co_equipment_user(actor);
    A64Fn change = a64_lookup(0x80e54c40ULL);
    if (!user || !change) return -1;
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x40) & ~0xfULL;
    c->x[0] = user, c->x[1] = (uint64_t)(uint32_t)slot, c->x[2] = (uint64_t)(uint32_t)state;
    change(c);
    const int result = (int)c->x[0];
    *c = saved;
    return result;
}

/* (with my place) what my Link wears: said when it changes, and every three seconds for whoever came since */
static void co_gear_say(uint64_t link) {
    static int64_t next_read, next_say, next_dump;
    static char said[CO_GEAR][CO_GEAR_NAME];
    static int dumps;
    const int64_t now = hos_now_ns();
    if (now < next_read) return;
    next_read = now + 500000000LL;
    CoStr s;
    if (!co_string64_open(&s)) return;
    char cur[CO_GEAR][CO_GEAR_NAME];
    const char *how[CO_GEAR];
    memset(cur, 0, sizeof cur);
    int any = 0;
    for (int k = 0; k < CO_GEAR; k++) {
        how[k] = "";
        if (co_gear_read(&s, k, cur[k], CO_GEAR_NAME, &how[k]) && co_gear_name_ok(cur[k])) any = 1; else cur[k][0] = 0;
    }
    if (dumps < 2 && (!dumps || (!any && now >= next_dump))) dumps++, next_dump = now + 20000000000LL, co_gear_dump(&s);
    if (!any) return;
    static char said_held[CO_GEAR_NAME];
    char held[CO_GEAR_NAME];
    co_slot_name(link, 3, held, sizeof held);   /* (Navi) the held item, slot 3 */
    if (strcmp(held, said_held)) hos_log("coop: gear: I hold %s", held[0] ? held : "nothing (slot 3)"), snprintf(said_held, sizeof said_held, "%s", held);
    static char sent_held[CO_GEAR_NAME] = "?";
    const int changed = memcmp(cur, said, sizeof cur) != 0 || strcmp(held, sent_held) != 0;
    if (changed)
        for (int k = 0; k < CO_GEAR; k++)
            if (strcmp(cur[k], said[k])) hos_log("coop: gear: I wear %s: %s (read as %s)", co_gear_kind[k], cur[k][0] ? cur[k] : "nothing", how[k]);
    if (changed || now >= next_say) {
        next_say = now + 3000000000LL;
        memcpy(said, cur, sizeof said);
        snprintf(sent_held, sizeof sent_held, "%s", held);
        co_send("B %s %s %s %s %s %s", cur[0][0] ? cur[0] : "-", cur[1][0] ? cur[1] : "-", cur[2][0] ? cur[2] : "-",
                cur[3][0] ? cur[3] : "-", cur[4][0] ? cur[4] : "-", held[0] ? held : "-");
    }
}

/* (every frame) a body plays what this game works out for it. Its pose comes from the library learned from my own Link, for
   the speed it is seen to move at (see the pose library above); where the library has none, from what its player's game
   says it plays (T and Y); and where neither, it stands. */
static void co_check_body(int peer, int slot, const char *name, int played);   /* (Navi) */
static void co_anim_put(Ctx *c, uint64_t actor, int peer, int child, float speed, float rise, int busy) {
    static uint32_t value[CO_INPUTS];                   /* (the game side is one thread at a time: co.game) */
    static uint8_t have[CO_INPUTS], cmd_have[CO_AS_SLOTS];
    static char want[CO_AS_SLOTS][CO_AS_NAME];
    static struct { uint64_t actor; char name[CO_AS_NAME]; int64_t at; } tried[CO_PEERS][CO_AS_SLOTS];   /* (a command the body's graph does not have is not asked for every frame) */
    static int64_t said_at[CO_PEERS];
    static int said_mode[CO_PEERS] = {-1, -1, -1, -1, -1, -1, -1, -1};
    static char failed[CO_PEERS][CO_AS_SLOTS][CO_AS_NAME];   /* (the last command each slot would not play, said once to the log) */
    static int failed_said;
    uint32_t words = 0;
    const uint64_t at = co_inputs_at(actor, &words);    /* (0: this body's inputs are not where this knows to look: commands can still be played) */
    EnterCriticalSection(&co.lock);
    const int same = at && co_in_words[peer] == words;
    if (same) {
        memcpy(value, co_in_value[peer], words * 4), memcpy(have, co_in_have[peer], words);
        memcpy(want, co_cmd_name[peer], sizeof want), memcpy(cmd_have, co_cmd_have[peer], sizeof cmd_have);
    } else {
        memcpy(want, co_cmd_name[peer], sizeof want), memcpy(cmd_have, co_cmd_have[peer], sizeof cmd_have);   /* (commands do not depend on the inputs' size) */
    }
    LeaveCriticalSection(&co.lock);
    if (!same) memset(have, 0, sizeof have);            /* (their graph is not this body's, or they have said nothing yet: no inputs of theirs fit it. A hit still shows) */
    char names[CO_AS_SLOTS][CO_AS_NAME];
    const int slots = co_as_names(actor, names);
    const int64_t now = hos_now_ns();
    int mode = same ? 1 : 0;                            /* 0 stands, 1 their game's, 2 the library's */
    if (slots > 0 && !busy && fabsf(rise) < 2.0f) {
        const CoPose *pose = co_pose_pick(child, speed, at ? words : 0);
        /* (their game's own command for a jump, a swing or a hit is kept: the library is for walking and standing) */
        /* (Navi) a swing, a shot, an item, a guard, a jump or a hit their game says is always theirs, even while the body
           moves: a sword swing lunges forward, and going by speed alone the library's run took its place, so another
           player was never seen to swing or use an item) */
        const int theirs = cmd_have[0] && want[0][0] && co_not_movement(want[0]);
        if (pose && !theirs && (!cmd_have[0] || !want[0][0] || co_is_movement(child, want[0]) || speed >= co_bin_edge[0])) {
            if (at) memcpy(value, pose->value, pose->words * 4ULL), memcpy(have, pose->have, pose->words);
            memset(cmd_have, 0, sizeof cmd_have);
            for (int i = 0; i < slots && i < pose->slots; i++) snprintf(want[i], CO_AS_NAME, "%s", pose->name[i]), cmd_have[i] = 1;
            mode = 2;
        }
    }
    if (now - said_at[peer] > 15000000000LL || said_mode[peer] != mode) {
        said_at[peer] = now, said_mode[peer] = mode;
        hos_log("coop: anim: body %d is animated by %s (%d slots, %u input words%s), seen at %.1f/s", peer,
                mode == 2 ? "this game's pose library" : mode == 1 ? "its player's game" : "nothing (standing)", slots, words, at ? "" : ", inputs not found", speed);
    }
    if (at) {
        for (uint32_t i = 0; i < words; i++)
            if (have[i] && !co_input_is_address(at + i * 4ULL)) *(uint32_t *)GP(at + i * 4ULL) = value[i];
        HOS_WRITTEN(GP(at), words * 4ULL);
    }
    if (now < co_flinch_until[peer]) snprintf(want[0], CO_AS_NAME, "%s", co_flinch_hard[peer] ? "DamageM" : "DamageS"), cmd_have[0] = 1;   /* (PvP: I just hit them) */
    if (slots > 0 && cmd_have[0] && !want[0][0]) snprintf(want[0], CO_AS_NAME, "Wait");   /* (no body animation at all is arms held out: stand instead) */
    for (int i = 0; i < slots; i++) {
        if (!cmd_have[i] || !strcmp(want[i], names[i])) continue;
        if (!want[i][0]) { co_as_remove(c, actor, i); continue; }
        if (tried[peer][i].actor == actor && !strcmp(tried[peer][i].name, want[i]) && now - tried[peer][i].at < 400000000LL) continue;
        tried[peer][i].actor = actor, tried[peer][i].at = now;
        memcpy(tried[peer][i].name, want[i], CO_AS_NAME);
        const int played = co_as_play(c, actor, i, want[i]);
        co_check_body(peer, i, want[i], played);   /* (Navi) */
        if (played != 1 && strcmp(failed[peer][i], want[i]) && failed_said < 60) {   /* (for the log: a body that would not play what was asked is how a T pose starts) */
            snprintf(failed[peer][i], CO_AS_NAME, "%s", want[i]), failed_said++;
            hos_log("coop: anim: body %d slot %d would not play '%s' (answer %d, %d slots)", peer, i, want[i], played, slots);
        }
    }
    /* (The body plays nothing: its player's game said nothing yet, said a command this graph does not take, as in a
       cutscene, and nothing was learned yet. It stands rather than hold its arms out.) */
    static int64_t stood[CO_PEERS];
    if (slots > 0 && !names[0][0] && now - stood[peer] > 500000000LL &&
        (!cmd_have[0] || !strcmp(want[0], "Wait") || (tried[peer][0].actor == actor && now - tried[peer][0].at > 150000000LL))) {
        stood[peer] = now;
        co_as_play(c, actor, 0, "Wait");
    }
}

/* ---- PvP ----
   On when both players have the NETWORK tab's PvP row on. The one who swings decides the hit: when Link starts an
   attack (slot 0 goes to a command named Cut..., but not the ones after a swing, or ChargeAttack) every other player
   in reach in front of him, a moment into the swing, is told "D <their number> <quarter hearts>". The one who is told
   takes it off their own hearts (Link's life: a count of quarter hearts at [[player+7184]+8], the value the heart
   protection in nx2/oot_patches.py guards; player is the object 0x1d50 before Link's movement component), once for a
   swing, and not more often than every 0.6 s.
   A hit is seen to land: the one who swings sees the other's body flinch at once (DamageS, or DamageM for the heavy
   swings: Link's own hit animations, played on the body until its player's game takes over); the one who is hit has
   his own Link play the same, and is pushed back from the one who hit him, a metre over a quarter of a second (the
   controller's change-position, as "go to a player" uses). His game says that animation to everyone like any other. */
static struct { int64_t at; int quarters, swing, shot; int told[CO_PEERS]; } co_swing;
static int co_body_is_foe(int i);
static int co_camera(float m[12], float scale[2]);

static int co_attack_quarters(const char *name) {
    if (!strncmp(name, "ChargeAttack", 12)) return 4;
    if (strncmp(name, "Cut", 3) || strstr(name, "After") || strstr(name, "Land") || strstr(name, "Pattern")) return 0;
    const size_t n = strlen(name);
    if (n >= 2 && !strcmp(name + n - 2, "Ed")) return 0;
    return strstr(name, "Jump") || strstr(name, "Hammer") ? 4 : 2;
}

static uint64_t co_life_at(Ctx *c, uint64_t link) {
    int hos_oot_is_player(Ctx *c, uint64_t actor);       /* hos_main.c */
    const uint64_t parts = hos_guest_range(link + 624, 8) ? G64(link + 624) : 0;
    const uint64_t move = hos_guest_range(parts + 800, 8) ? G64(parts + 800) : 0;
    if (move < HOS_HEAP_BASE + 0x1d50 || !hos_oot_is_player(c, move - 0x1d50)) return 0;
    const uint64_t holder = hos_guest_range(move - 0x1d50 + 7184, 8) ? G64(move - 0x1d50 + 7184) : 0;
    return hos_guest_range(holder, 16) ? holder + 8 : 0;
}

/* (Navi) the checks: what each item and move looks like from both sides, to make the other players' bodies use them.
   Reads only. Kept in <game>\coop_checks.txt (made new at each start) and the log, as "coop: check:" lines:
     * mine: each move my Link's body starts (slot 0's command: a swing, a shot, the boomerang, the hookshot, a bomb, the
       ocarina, the shield, a roll, a jump...) the first time it is seen, with what is equipped then (the sword, the shield,
       the item buttons: PlayerStatus.EquipItemY, EquipShortcut_0, EquipArrow, EquipOcarina, by their GameDataList names);
       and for the first two times of each, the 32-bit words of my equipment user that changed from the move before (how
       the game takes an item out or puts it away: what a body must be told to show it in hand);
     * theirs: each move a body was asked to play, and whether its graph played it (a body that refuses one stands in
       the wrong pose: that is the list of what is missing);
     * every minute, how often each was seen. HOS_COOP_CHECKS=0: none. */
#define CO_CHECK_NAMES 160
static FILE *co_check_file;
static int co_check_off = -1, co_check_lines;
static struct { char name[CO_AS_NAME]; int mine, asked, played, refused, diffs; } co_check_seen[CO_CHECK_NAMES];
static int co_check_n;
static uint64_t co_equipment_user(uint64_t actor);

static void co_check_line(const char *fmt, ...) {
    if (co_check_off < 0) { const char *e = getenv("HOS_COOP_CHECKS"); co_check_off = e && !strcmp(e, "0"); }
    if (co_check_off || co_check_lines >= 3000) return;
    char line[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    co_check_lines++;
    hos_log("coop: check: %s", line);
    if (!co_check_file) {
        char game[MAX_PATH], path[MAX_PATH + 32];
        co_game_dir(game, sizeof game);
        snprintf(path, sizeof path, "%s\\coop_checks.txt", game);
        co_check_file = fopen(path, "wb");
        if (co_check_file) fprintf(co_check_file, "# Longshot co-op checks: what each item and move looks like on this PC (send this file with the log)\r\n");
    }
    if (co_check_file) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(co_check_file, "%02d:%02d:%02d %s\r\n", t.wHour, t.wMinute, t.wSecond, line);
        fflush(co_check_file);
    }
}

static int co_check_slot(const char *name) {
    for (int i = 0; i < co_check_n; i++) if (!strcmp(co_check_seen[i].name, name)) return i;
    if (co_check_n >= CO_CHECK_NAMES) return -1;
    snprintf(co_check_seen[co_check_n].name, CO_AS_NAME, "%s", name);
    return co_check_n++;
}

/* what is equipped now, as one line: the sword, the shield, the item buttons */
static void co_check_held(char *out, size_t cap) {
    static const char *const what[][2] = {{"PlayerStatus.EquipWeapon", "sword"}, {"PlayerStatus.EquipShield", "shield"},
        {"PlayerStatus.EquipItemY", "Y"}, {"PlayerStatus.EquipShortcut_0", "shortcut"}, {"PlayerStatus.EquipArrow", "arrow"},
        {"PlayerStatus.EquipOcarina", "ocarina"}, {"PlayerStatus.EquipHeadArmor", "head"}, {"PlayerStatus.EquipArmor", "tunic"},
        {"PlayerStatus.EquipBoot", "boots"}};
    size_t n = 0;
    out[0] = 0;
    CoStr st;
    if (!co_string64_open(&st)) { snprintf(out, cap, "(equipment not readable)"); return; }
    for (unsigned k = 0; k < sizeof what / sizeof what[0] && n + 8 < cap; k++) {
        char text[CO_GEAR_NAME] = "";
        const int i = co_string64_find(&st, co_mm3(what[k][0]));
        if (i < 0) continue;
        if (!co_string64_text(&st, i, text, sizeof text)) snprintf(text, sizeof text, "-");
        n += (size_t)snprintf(out + n, cap - n, "%s%s=%s", n ? " " : "", what[k][1], text);
    }
}

/* mine: each frame, with what my Link's slots play */
static void co_check_mine(uint64_t link, char names[CO_AS_SLOTS][CO_AS_NAME], int slots) {
    static char was[CO_AS_NAME];
    static uint32_t before[0x100];
    static uint64_t user_was;
    static int have;
    if (slots < 1 || !strcmp(was, names[0])) return;
    const uint64_t user = co_equipment_user(link);
    uint32_t cur[0x100];
    const int readable = user && hos_guest_range(user, sizeof cur);
    if (readable) for (int i = 0; i < 0x100; i++) cur[i] = G32(user + i * 4ULL);
    if (user != user_was) user_was = user, have = 0;
    const int k = names[0][0] ? co_check_slot(names[0]) : -1;
    if (k >= 0) {
        co_check_seen[k].mine++;
        if (co_check_seen[k].mine == 1) {               /* (the first time: what it is, and what is held) */
            char others[CO_AS_SLOTS * (CO_AS_NAME + 4)] = "", held[400];
            size_t n = 0;
            for (int i = 1; i < slots && i < CO_AS_SLOTS && n + CO_AS_NAME + 4 < sizeof others; i++)
                if (names[i][0]) n += (size_t)snprintf(others + n, sizeof others - n, " %d:%s", i, names[i]);
            co_check_held(held, sizeof held);
            co_check_line("mine: '%s' (after '%s'; other slots%s) | %s", names[0], was[0] ? was : "-", n ? others : " none", held);
        }
        if (readable && have && co_check_seen[k].diffs < 2) {   /* (the equipment user's words that changed with it) */
            char line[900];
            int n = 0, count = 0;
            for (int i = 0; i < 0x100 && n < (int)sizeof line - 40; i++)
                if (cur[i] != before[i]) n += snprintf(line + n, sizeof line - n, " +%x %x>%x", i * 4, before[i], cur[i]), count++;
            if (count && count < 48) co_check_line("mine: '%s' changed my equipment user:%s", names[0], line), co_check_seen[k].diffs++;
        }
    }
    if (readable) memcpy(before, cur, sizeof before), have = 1;
    snprintf(was, sizeof was, "%s", names[0]);
}

/* theirs: a body asked to play a command (slot 0 only: the moves), and what its graph answered */
static void co_check_body(int peer, int slot, const char *name, int played) {
    if (slot != 0 || !name[0]) return;
    const int k = co_check_slot(name);
    if (k < 0) return;
    co_check_seen[k].asked++;
    if (played == 1) co_check_seen[k].played++;
    else if (!co_check_seen[k].refused++) co_check_line("theirs: body %d would not play '%s' (answer %d): this move is missing on it", peer, name, played);
    if (co_check_seen[k].asked == 1 && played == 1) co_check_line("theirs: body %d plays '%s'", peer, name);
}

/* every minute: how often each was seen */
static void co_check_summary(int64_t now) {
    static int64_t next;
    static int said_n;
    if (now < next || !co_check_n) return;
    next = now + 60000000000LL;
    if (co_check_n == said_n && co_check_lines > 2900) return;
    said_n = co_check_n;
    char line[1000];
    size_t n = 0;
    for (int i = 0; i < co_check_n && n + 64 < sizeof line; i++) {
        const int m = co_check_seen[i].mine, a = co_check_seen[i].asked;
        if (!m && !a) continue;
        n += (size_t)snprintf(line + n, sizeof line - n, "%s%s mine %d, theirs %d/%d", n ? "; " : "", co_check_seen[i].name, m,
                              co_check_seen[i].played, a);
    }
    if (n) co_check_line("so far (theirs: played/asked): %s", line);
}

static void co_sword_probe_body(uint64_t actor) {
    static int said;
    if (said >= 2) return;
    const uint64_t user = co_equipment_user(actor);
    if (!hos_guest_range(user, 0x400)) return;
    char line[400];
    int n = snprintf(line, sizeof line, "coop: sword probe: a body's equipment user %llx holds the start state at:", (unsigned long long)user), found = 0;
    for (int i = 0; i < 0x100 && n < (int)sizeof line - 16; i++)
        if (G32(user + i * 4ULL) == 0xa471d9c4u) n += snprintf(line + n, sizeof line - n, " +%x", i * 4), found++;
    if (!found) snprintf(line + n, sizeof line - n, " nowhere in its first 0x400 bytes");
    co_check_line("%s", line + 6), said++;   /* (Navi: "coop: " off) */
}

/* (Navi) Link's most life, in quarter hearts: the Life component's maximum, a second holder beside the life's, at
   [player+7192]+8 (the life's is at [player+7184]+8, co_life_at; daurosk's oot-remake-multiplayer, docs/items.md: "its max at
   [Life + 0x1c18] + 8"; the game draws its hearts from it, not from PlayerStatus.MaxLife). 0 when it does not read as a
   maximum (12 to 80 quarters, a whole heart, not below the life) */
static uint32_t co_life_max(Ctx *c, uint64_t link, uint32_t life) {
    const uint64_t at = co_life_at(c, link);
    if (!at) return 0;
    const uint64_t parts = hos_guest_range(link + 624, 8) ? G64(link + 624) : 0;
    const uint64_t move = hos_guest_range(parts + 800, 8) ? G64(parts + 800) : 0;
    const uint64_t holder = hos_guest_range(move - 0x1d50 + 7192, 8) ? G64(move - 0x1d50 + 7192) : 0;
    if (!hos_guest_range(holder, 16)) return 0;
    const uint32_t most = G32(holder + 8);
    return most >= 12 && most <= 80 && most % 4 == 0 && most >= life ? most : 0;
}

/* (Navi) my hearts, for the others' party panel: "C <life> <max>" when they change, and every 3 s for whoever came since */
static void co_hearts_say(Ctx *c, uint64_t link, int64_t now) {
    static int64_t next;
    static uint32_t said_life = ~0u, said_max = ~0u;
    static int told;
    const uint64_t at = co_life_at(c, link);
    if (!at) return;
    const uint32_t life = G32(at), most = co_life_max(c, link, life);
    if (!told++) hos_log("coop: hearts: mine %u of %u (quarters)%s", life, most, most ? "" : ", the most not readable: the others see the hearts left");
    if (life == said_life && most == said_max && now < next) return;
    said_life = life, said_max = most, next = now + 3000000000LL;
    co_send("C %u %u", life, most);
}

static void co_pvp_step(Ctx *c, uint64_t link, int64_t now) {
    const int on = rx_option(HOS_OPT_COOP_PVP, 0) > 0;
    /* mine: did a swing start, and who is in its way */
    static char was[CO_AS_NAME];
    char names[CO_AS_SLOTS][CO_AS_NAME];
    const int my_slots = co_as_names(link, names);
    if (my_slots > 0) co_check_mine(link, names, my_slots);   /* (Navi) the checks: read only */
    co_check_summary(now);
    if (my_slots > 0 && strcmp(was, names[0])) {
        memcpy(was, names[0], CO_AS_NAME);
        /* (a seed or an arrow let go: Link's body goes to BowShoot, the slingshot's too. It reaches the player the camera
           looks at: the shot goes where the picture's middle is) */
        const int shot = !strcmp(names[0], "BowShoot");
        const int quarters = shot ? 2 : co_attack_quarters(names[0]);
        if (quarters && on) co_swing.at = now, co_swing.quarters = quarters, co_swing.shot = shot, co_swing.swing++, memset(co_swing.told, 0, sizeof co_swing.told);
    }
    if (on && co_swing.at && now - co_swing.at > 120000000LL && now - co_swing.at < 450000000LL) {
        float mine[3], yaw, view[12], lens[2], eye[3] = {0, 0, 0}, ahead[3] = {0, 0, 0};
        const int camera = co_swing.shot && co_camera(view, lens);
        if (camera) {                                   /* (the view matrix takes the world into the camera's space, which looks down -z) */
            for (int k = 0; k < 3; k++) eye[k] = -(view[k] * view[3] + view[4 + k] * view[7] + view[8 + k] * view[11]), ahead[k] = -view[8 + k];
        }
        EnterCriticalSection(&co.lock);
        memcpy(mine, co.pos, sizeof mine), yaw = co.yaw;
        for (int i = 0; i < CO_PEERS; i++) {
            const CoPeer *p = &co.peer[i];
            if (!p->used || co_swing.told[i] || !p->map[0] || strcmp(p->map, co_map) || now - p->seen > 1500000000LL) continue;
            if (p->busy) continue;                      /* (in a cutscene or talking: left alone) */
            if (!p->pvp) {                              /* (Navi) their PvP is off: their game takes no hit, so none is said to land */
                static int64_t warned[CO_PEERS];
                if (now - warned[i] > 10000000000LL) {
                    warned[i] = now;
                    char name[24];
                    snprintf(name, sizeof name, "%s", p->name);
                    LeaveCriticalSection(&co.lock);
                    co_note("%s has PvP off: no hit", name);
                    hos_log("coop: pvp: my swing reached %s, whose PvP is off (their NETWORK tab): no hit", name);
                    EnterCriticalSection(&co.lock);
                }
                co_swing.told[i] = 1;
                continue;
            }
            if (co_body_is_foe(i) && !co_swing.shot) continue;   /* (a real target: the game's own combat says what a swing reached, co_bodies_step; a seed or an arrow is still judged by its line too: the sensor is narrow) */
            const float dx = p->pos[0] - mine[0], dy = p->pos[1] - mine[1], dz = p->pos[2] - mine[2];
            const float apart = sqrtf(dx * dx + dz * dz);
            if (co_swing.shot) {                        /* (the shot's line: from the camera, the way it looks, past Link) */
                if (!camera) continue;
                const float t[3] = {p->pos[0] - eye[0], p->pos[1] + (p->child ? 0.55f : 0.95f) - eye[1], p->pos[2] - eye[2]};
                const float along = t[0] * ahead[0] + t[1] * ahead[1] + t[2] * ahead[2];
                const float off2 = t[0] * t[0] + t[1] * t[1] + t[2] * t[2] - along * along;
                const float wide = 0.45f + 0.012f * along;   /* (a body's width, and a little more with the distance) */
                if (along < 1.5f || along > 45.0f || off2 > wide * wide) continue;
            } else {
            if (apart > 1.9f || fabsf(dy) > 1.5f) continue;
            if (apart > 0.3f && (dx * sinf(yaw) + dz * cosf(yaw)) / apart < 0.25f) continue;   /* (not in front: some 75 degrees either side) */
            }
            co_swing.told[i] = 1;
            co_flinch_until[i] = now + 450000000LL, co_flinch_hard[i] = co_swing.quarters >= 4;
            const int id = p->id;
            char name[24];
            snprintf(name, sizeof name, "%s", p->name);
            LeaveCriticalSection(&co.lock);
            co_send("D %d %d", id, co_swing.quarters);
            co_note("You hit %s", name);
            hos_log("coop: pvp: my %s reached %s (%d quarter hearts)", co_swing.shot ? "shot" : "swing", name, co_swing.quarters);
            hos_log_flush();
            EnterCriticalSection(&co.lock);
        }
        LeaveCriticalSection(&co.lock);
    }
    /* theirs: what is to come off my hearts */
    static int64_t last_hit, push_until, calm_until;
    static float push[2];
    /* (Not in a cutscene, a conversation or a loading, nor for a moment after: a swing that reaches a player during
       a scene takes hearts, plays the hit on a Link the scene is moving and pushes him out of his place, and the scene
       never ends.) */
    if (co_game_busy()) calm_until = now + 1500000000LL, push_until = 0;
    if (now < calm_until && InterlockedExchange(&co_hurt, 0) > 0) hos_log("coop: pvp: a hit by %s came in a cutscene, a talk or a load: not taken", co_hurt_by);
    if (!on && co_hurt > 0) {                           /* (Navi) said, so a hit that did nothing has a reason in the log */
        static int64_t warned;
        InterlockedExchange(&co_hurt, 0);
        if (now - warned > 10000000000LL) warned = now, co_note("%s hit you, but your PvP is off", co_hurt_by[0] ? co_hurt_by : "A player"),
            hos_log("coop: pvp: hit by %s, but PvP is off here (NETWORK tab): not taken", co_hurt_by);
    }
    if (now < push_until) {                             /* (pushed back: a step each frame) */
        float at[3];
        EnterCriticalSection(&co.lock);
        memcpy(at, co.pos, sizeof at);
        LeaveCriticalSection(&co.lock);
        at[0] += push[0] * 0.07f, at[2] += push[1] * 0.07f;
        co_place(c, link, at);
    }
    const int hurt = (int)InterlockedExchange(&co_hurt, 0);
    if (hurt > 0 && on && now - last_hit > 600000000LL) {
        const uint64_t at = co_life_at(c, link);
        if (at) {
            const uint32_t life = G32(at), next = life > (uint32_t)hurt ? life - (uint32_t)hurt : 0;
            *(uint32_t *)GP(at) = next;
            HOS_WRITTEN(GP(at), 4);
            last_hit = now;
            if (life && !next) {                        /* (that was the last of my hearts: everyone is told who did it) */
                co_send("O %d", (int)co_hurt_from);
                co_note("%s defeated you", co_hurt_by[0] ? co_hurt_by : "A player");
            }
            co_as_play(c, link, 0, hurt >= 4 ? "DamageM" : "DamageS");
            {   /* away from the one who swung */
                float mine[3], dx = 0, dz = 0;
                EnterCriticalSection(&co.lock);
                memcpy(mine, co.pos, sizeof mine);
                const CoPeer *by = co_peer((int)co_hurt_from, 0);
                if (by) dx = mine[0] - by->pos[0], dz = mine[2] - by->pos[2];
                LeaveCriticalSection(&co.lock);
                const float size = sqrtf(dx * dx + dz * dz);
                if (size > 0.05f) push[0] = dx / size, push[1] = dz / size, push_until = now + 250000000LL;
            }
            co_note("%s hit you", co_hurt_by[0] ? co_hurt_by : "A player");
            hos_log("coop: pvp: hit by %s: hearts %u -> %u (quarters)", co_hurt_by, life, next);
            hos_log_flush();
        } else {
            static int said;
            if (said++ < 5) hos_log("coop: pvp: hit by %s, but my hearts were not found: not taken", co_hurt_by), hos_log_flush();
        }
    }
}

/* ---- (Navi) players who bump ----
   The other players' bodies have no character controller of their own (nx2/coop_bodies.py: with Dark Link's whole
   physics a body walked off on its own), so Links walk through one another. Here each game keeps its own Link out of
   the others: when another player stands closer than two Links' width at about the same height, my Link is moved out
   along the line between us by half the overlap each frame (their game moves theirs the other half), so two players
   running into each other stop a body's width apart. Moved the way the PvP push-back moves him (co_place). On while PvP is on for both
   players; HOS_COOP_COLLIDE=1 makes players solid always, =0 never. Not in a cutscene, a conversation or a load. */
static void co_collide_step(Ctx *c, uint64_t link, int64_t now) {
    static int mode = -1;   /* 0 never, 1 with PvP, 2 always */
    if (mode < 0) {
        const char *e = getenv("HOS_COOP_COLLIDE");
        mode = e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "1") ? 2 : 1;
        hos_log("coop: bump: players are %s", mode == 0 ? "never solid (HOS_COOP_COLLIDE=0)"
                                            : mode == 2 ? "always solid to each other (HOS_COOP_COLLIDE=1)"
                                                        : "solid to each other while PvP is on for both");
    }
    const int pvp_here = rx_option(HOS_OPT_COOP_PVP, 0) > 0;
    if (!mode || (mode == 1 && !pvp_here) || co_game_busy()) return;
    const float reach = 0.7f;   /* (two Links' width: where the bodies touch, in metres) */
    float mine[3], yaw, out[2] = {0, 0};
    EnterCriticalSection(&co.lock);
    memcpy(mine, co.pos, sizeof mine), yaw = co.yaw;
    for (int i = 0; i < CO_PEERS; i++) {
        const CoPeer *p = &co.peer[i];
        if (!p->used || !p->map[0] || strcmp(p->map, co_map) || now - p->seen > 500000000LL || p->busy) continue;
        if (mode == 1 && !p->pvp) continue;
        if (fabsf(p->pos[1] - mine[1]) > 1.2f) continue;   /* (one above the other: a ledge, a ladder) */
        float dx = mine[0] - p->pos[0], dz = mine[2] - p->pos[2];
        const float apart = sqrtf(dx * dx + dz * dz);
        if (apart >= reach) continue;
        if (apart > 0.02f) dx /= apart, dz /= apart;
        else dx = -sinf(yaw), dz = -cosf(yaw);   /* (stood in one spot: back the way I face) */
        out[0] += dx * (reach - apart) * 0.5f, out[1] += dz * (reach - apart) * 0.5f;
    }
    LeaveCriticalSection(&co.lock);
    float step = sqrtf(out[0] * out[0] + out[1] * out[1]);
    if (step < 0.005f) return;
    /* (all of my half each frame: a running Link covers ~7 cm a frame, and anything less let two run through each other;
       at most 25 cm, so a late position after a network stall does not throw anyone) */
    const float k = (step > 0.25f ? 0.25f : step) / step;
    const float at[3] = {mine[0] + out[0] * k, mine[1], mine[2] + out[1] * k};
    co_place(c, link, at);
}

/* (Navi) the body a look is drawn as: <base>_<look> (nx2/coop_looks.py made those into the co-op mod), when the mod has
   it; Dark Link is adult only, a child who chose him is black; anything else, or green, is the body as it is */
static void co_look_actor(char *out, size_t cap, const char *base, int own, int child, const char *look) {
    int hos_mods_has(const char *prefix, const char *suffix);   /* mods.c */
    snprintf(out, cap, "%s", base);
    if (!own || co_looks_off) return;
    /* (Navi) green is a body of its own too: with your own Link in your colour the plain tile is yours (coop_looks.py SELF) */
    const char *want = !look[0] || !strcmp(look, "-") || !strcmp(look, "green") ? "green"
                     : child && !strcmp(look, "dark") ? "black" : look;
    char name[48], low[64];
    snprintf(name, sizeof name, "%s_%s", base, want);
    snprintf(low, sizeof low, "pack/actor/%s", name);
    for (char *k = low; *k; k++) if (*k >= 'A' && *k <= 'Z') *k = (char)(*k - 'A' + 'a');
    if (hos_mods_has(low, ".pack.zs")) snprintf(out, cap, "%s", name);
    else {
        static int said;
        if (said++ < 4) hos_log("coop: look: no body %s in the co-op mod (build co-op again): drawn as %s", name, base);
    }
}

/* (Navi) the body's sword (slot 0), held item (3) and shield (4) put in hand or away as its player's are (P's drawn bits), by the
   game's change of state; taken when the slot's request byte shows it (2 in hand, 4 away), else asked again after 250 ms, at
   most 6 times for one change. A piece made again (another id) counts as put away. The held item is put on first (the equip
   call, the bow's slot kind 5, as the game's own equip tests slot 3 for). HOS_COOP_HANDS=0: none of it. */
static int co_equip(Ctx *c, uint64_t actor, int slot, const char *name, int number);
static void co_hands_apply(Ctx *c, CoBody *b, int body, unsigned drawn, const char *held, int64_t now) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("HOS_COOP_HANDS"); mode = !(e && !strcmp(e, "0")); hos_log("coop: hands: other players' sword, shield and held item in hand: %s", mode ? "on" : "off (HOS_COOP_HANDS=0)"); }
    if (!mode || !b->actor || co_game_busy()) return;
    const uint64_t user = co_equipment_user(b->actor);
    if (!user || !hos_guest_range(user + 0x13bc, CO_EQ_SLOTS)) return;
    /* (the held item put on by the equip call with the bow's slot kind crashed the game a moment later when it was the
       slingshot, 2026-11-14 (read guest 0x7d8): off unless HOS_COOP_HELD=1, until the slot kind for a body is known) */
    static int held_on = -1;
    if (held_on < 0) { const char *e = getenv("HOS_COOP_HELD"); held_on = e && !strcmp(e, "1"); }
    if (held_on && held[0] && strcmp(held, b->held) && b->held_tries < 3) {
        b->held_tries++;
        const int r = co_equip(c, b->actor, 5, held, 0);
        hos_log("coop: hands: body %d holds %s -> %d (try %d)", body, held, r, b->held_tries);
        if (r != -1) snprintf(b->held, sizeof b->held, "%s", held), b->held_tries = 0;
    }
    static const int slots[] = {0, 3, 4};
    for (unsigned k = 0; k < sizeof slots / sizeof *slots; k++) {
        const int i = slots[k];
        uint32_t id = 0;
        if (!co_slot_piece(user, i, &id)) continue;
        if (b->hand_id[i] != id) b->hand_id[i] = id, b->hand_set[i] = 0, b->hand_tries[i] = 0;   /* (made again: put away) */
        const int want = (drawn >> i) & 1;
        if (want == b->hand_set[i] || now < b->hand_next[i]) continue;
        if (b->hand_tries[i] >= 6) { b->hand_set[i] = (uint8_t)want, b->hand_tries[i] = 0; hos_log("coop: hands: body %d slot %d not taken after 6 asks: left", body, i); continue; }
        b->hand_tries[i]++;
        const int r = co_change_state(c, b->actor, i, want ? 1 : 2);
        /* (the slot's byte is a request the game clears once it has made the change: read at once, as daurosk's Director does,
           not at the next ask, by when it read 00 every time) */
        const uint8_t bits = *(const uint8_t *)GP(user + 0x13bc + (uint64_t)i);
        if (bits & (want ? 2 : 4)) {
            b->hand_set[i] = (uint8_t)want, b->hand_tries[i] = 0;
            hos_log("coop: hands: body %d slot %d %s (call %d, byte %02x)", body, i, want ? "in hand" : "put away", r, bits);
            continue;
        }
        if (b->hand_tries[i] == 1) hos_log("coop: hands: body %d slot %d %s asked: call %d, byte %02x: not yet", body, i, want ? "in hand" : "put away", r, bits);
        b->hand_next[i] = now + 250000000LL;
    }
}

static int co_body_is_foe(int i) { return i >= 0 && i < CO_PEERS && co_body[i].actor && co_body[i].foe; }

/* ---- what a body holds ----
   The game's own "give this character this equipment" (the script node OneShotEquipmentUserRequestCreateDynamicEquipment,
   run function f_828f1880) is f_82e17b20(equipment user, slot, &name, a number, -1, 1, 0): the equipment user is the
   actor's component at [actor+0x270]+0x200, the slot a number under 18 (the node's slots 1..6 are 1, 0, 5, 6, 11, 12
   by its table at 0x83f0a928). Its "change state" (OneShotEquipmentUserRequestChangeState, f_828f1670) is f_806d6820(equipment user, state)
   for the node's kind 1 and f_80e54c70(equipment user, state) for kind 2. */
static uint64_t co_equipment_user(uint64_t actor) {
    const uint64_t parts = hos_guest_range(actor + 0x270, 8) ? G64(actor + 0x270) : 0;
    const uint64_t user = hos_guest_range(parts + 0x200, 8) ? G64(parts + 0x200) : 0;
    return hos_guest_range(user, 0x20) ? user : 0;
}

static int co_equip(Ctx *c, uint64_t actor, int slot, const char *name, int number) {
    const uint64_t user = co_equipment_user(actor);
    A64Fn make = a64_lookup(0x82e17b20ULL);
    if (!user || !make || slot < 0 || slot >= 18 || strlen(name) >= 0x40) return -1;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x100) & ~0xfULL;
    memset(A64_PTR(frame), 0, 0x100);
    memcpy(A64_PTR(frame + 0x40), name, strlen(name) + 1);
    recomp_store64_sp(c, frame + 0x20, frame + 0x40);
    c->x[31] = frame;
    c->x[0] = user, c->x[1] = (uint64_t)slot, c->x[2] = frame + 0x20, c->x[3] = (uint64_t)(uint32_t)number, c->x[4] = 0xffffffffULL, c->x[5] = 1, c->x[6] = 0;
    make(c);
    const int result = (int)c->x[0];
    *c = saved;
    return result;
}

static int co_equip_state(Ctx *c, uint64_t actor, int kind, int state) {
    const uint64_t user = co_equipment_user(actor);
    A64Fn change = a64_lookup(kind == 2 ? 0x80e54c70ULL : 0x806d6820ULL);
    if (!user || !change) return -1;
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x40) & ~0xfULL;
    c->x[0] = user, c->x[1] = (uint64_t)(uint32_t)state;
    change(c);
    const int result = (int)c->x[0];
    *c = saved;
    return result;
}

/* What another player wears, put on their body (B). Each piece goes on through the game's own equip call, as above, in the
   slot kind of its piece (co_gear_slot). A body is made with its age's sword and shield (nx2/coop_bodies.py): a piece
   that is those and was never changed is left as it is. One piece at a time, half a second apart; one that does not take
   is tried three times. HOS_COOP_GEAR=0 turns it all off, =basic puts on only the sword and the shield. */
static void co_gear_apply(Ctx *c, CoBody *b, int body, int child, const char gear[CO_GEAR][CO_GEAR_NAME], int64_t now) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("HOS_COOP_GEAR");
        mode = e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "basic") ? 1 : 2;
        hos_log("coop: gear: other players' gear is %s", mode == 0 ? "off (HOS_COOP_GEAR=0)" : mode == 1 ? "the sword and shield only" : "put on their bodies");
    }
    if (!mode || !b->actor || now < b->wear_next || co_game_busy()) return;
    const int pieces = mode == 1 ? 2 : CO_GEAR;
    for (int k = 0; k < pieces; k++) {
        const char *want = gear[k];
        /* (Navi) a mask, a tunic or boots taken off: there is no "take off" call known, so the body is made again (age,
           look and the rest as they are) and what is still worn goes on it */
        if (!want[0] && k >= 2 && b->worn[k][0]) {
            hos_log("coop: gear: body %d took off %s %s: made again", body, co_gear_kind[k], b->worn[k]);
            b->remake = 1;
            return;
        }
        /* (Navi) Dark Link's body holds his own dark Master Sword and Hylian Shield */
        if (!child && !strcmp(b->look, "dark")) {
            if (!strcmp(want, "Weapon_Sword_Master")) want = "Weapon_Sword_DarkLinkSword";
            else if (!strcmp(want, "Weapon_Shield_Hylian")) want = "Weapon_Shield_DarkLinkShield";
        }
        if (!want[0] || !strcmp(want, b->worn[k])) continue;
        /* (Navi) the sword and shield the body is made with were taken as already on it and never put on: the other players
           were seen with neither, not in hand nor on the back. Every piece now goes on through the game's own equip call,
           the made ones too */
        if (strcmp(want, b->try_name[k])) snprintf(b->try_name[k], CO_GEAR_NAME, "%s", want), b->wear_tries[k] = 0;
        if (b->wear_tries[k] >= 3) continue;
        b->wear_tries[k]++;
        const int r = co_equip(c, b->actor, co_gear_slot[k], want, 0);
        hos_log("coop: gear: body %d (%s) puts on %s %s -> %d (try %d)", body, child ? "child" : "adult", co_gear_kind[k], want, r, b->wear_tries[k]);
        if (r != -1) snprintf(b->worn[k], CO_GEAR_NAME, "%s", want);
        b->wear_next = now + 500000000LL;
        return;
    }
}

/* (test runs) what Link plays, said to the log each time it changes; and a "A <slot> <command>" line plays one on the bodies */
static void co_as_test(Ctx *c, uint64_t link, int allowed) {
    if (!allowed) return;
    static char was[CO_AS_SLOTS][CO_AS_NAME];
    static int said;
    char now[CO_AS_SLOTS][CO_AS_NAME];
    memset(now, 0, sizeof now);
    const int n = co_as_names(link, now);
    if (n && memcmp(was, now, sizeof was) && said < 400) {
        char line[CO_AS_SLOTS * (CO_AS_NAME + 6)];
        size_t used = 0;
        for (int i = 0; i < n; i++) if (now[i][0]) used += (size_t)snprintf(line + used, sizeof line - used, " %d:%s", i, now[i]);
        said++;
        hos_log("coop: test: Link plays (%d slots)%s", n, used ? line : " nothing");
        hos_log_flush();
        memcpy(was, now, sizeof was);
    }
}

/* ---- the name over another player ----
   The menu draws a small bubble with the name (gpu/vk/vk_menu.cpp, the list's n-th player); this says where on the
   screen, from the game's own camera. The camera the game plays through (hos_main.c oot_gameplay_view: the first of
   the camera manager's) has its view at +120: a 3x4 matrix at +8, the world into the camera's space, looking down
   -z. Its projection is at +128: a 4x4 at +0xc whose [0][0] and [1][1] are 1/(aspect tan(fov/2)) and 1/tan(fov/2)
   (read in play: 1.2063 and 2.1445, for 50 degrees at 16:9). So a point v in the camera's space is on the screen at
   x = [0][0] v.x / -v.z, y = [1][1] v.y / -v.z, each from -1 to 1.
   One number a label: the list's place << 28, shown << 26, x << 13, y, x and y in 8191ths of the picture. Said again
   at least ten times a second: the menu takes a label away that has not been said for a moment (the pause menu, a
   cutscene, a map load: Link's update, where this runs, stops). */
static int co_camera(float m[12], float scale[2]) {
    const uint64_t holder = G64(0x84ca6bc0ULL);
    const uint64_t mgr = hos_guest_range(holder, 8) ? G64(holder) : 0;
    if (!hos_guest_range(mgr, 48) || !G32(mgr + 32)) return 0;
    const uint64_t list = G64(mgr + 40);
    const uint64_t camera = hos_guest_range(list, 8) ? G64(list) : 0;
    if (!hos_guest_range(camera, 136)) return 0;
    const uint64_t view = G64(camera + 120), lens = G64(camera + 128);
    if (!hos_guest_range(view, 0x38) || G64(view) != 0x84c18a88ULL || !hos_guest_range(lens, 0x24) || G64(lens) != 0x84c18c70ULL) return 0;
    memcpy(m, GP(view + 8), 48);
    memcpy(&scale[0], GP(lens + 0x0c), 4);
    memcpy(&scale[1], GP(lens + 0x20), 4);
    return scale[0] > 0.05f && scale[0] < 50.0f && scale[1] > 0.05f && scale[1] < 50.0f;
}

/* (Navi) A compass: a player who is on this map but not in the picture (behind the camera, beside it, or too far to read)
   has the name pinned to the picture's edge, the way to turn to see them (bit 27), and HOS_OPT_COOP_DIST says how many
   metres away they are, which the name shows once they are 10 m or more away. */
static void co_label(int order, int shown, const float m[12], const float scale[2], const float place[3], int child, int64_t now) {
    static int was[CO_PEERS], was_dist[CO_PEERS];
    static int64_t said[CO_PEERS];
    if (order < 0 || order >= CO_PEERS) return;
    int packed = order << 28, metres = 0;
    if (shown) {
        const float x = place[0], y = place[1] + (child ? 1.32f : 1.98f), z = place[2];   /* (over the head) */
        const float vx = m[0] * x + m[1] * y + m[2] * z + m[3], vy = m[4] * x + m[5] * y + m[6] * z + m[7];
        const float vz = m[8] * x + m[9] * y + m[10] * z + m[11];
        float sx = 0, sy = 0;
        int on_screen = 0;
        if (vz < -0.4f) {
            sx = scale[0] * vx / -vz, sy = scale[1] * vy / -vz;
            on_screen = vz > -60.0f && fabsf(sx) < 1.0f && fabsf(sy) < 1.0f;   /* (in front, in the picture, near enough to read) */
        }
        if (on_screen) {
            packed |= 1 << 26 | (int)((sx * 0.5f + 0.5f) * 8191.0f) << 13 | (int)((0.5f - sy * 0.5f) * 8191.0f);
        } else {
            /* the way to them on the picture: where they would be drawn, or, behind the camera, which side they are on
               (camera space x: right) along the bottom edge; then out to the picture's edge, a little inside it */
            float dx, dy;
            if (vz < -0.4f) dx = sx, dy = sy;
            else dx = vx, dy = -fabsf(vz) - fabsf(vx) * 0.25f;   /* (behind: low on the picture, to their side) */
            const float big = fmaxf(fabsf(dx), fabsf(dy));
            if (big > 1e-4f) {
                dx = dx / big * 0.9f, dy = dy / big * 0.9f;
                packed |= 1 << 26 | 1 << 27 | (int)((dx * 0.5f + 0.5f) * 8191.0f) << 13 | (int)((0.5f - dy * 0.5f) * 8191.0f);
            }
        }
        float mine[3];
        EnterCriticalSection(&co.lock);
        memcpy(mine, co.pos, sizeof mine);
        LeaveCriticalSection(&co.lock);
        const float ex = place[0] - mine[0], ey = place[1] - mine[1], ez = place[2] - mine[2];
        const float d = sqrtf(ex * ex + ey * ey + ez * ez);
        metres = d < 99999.0f ? (int)(d + 0.5f) : 99999;
    }
    if (packed == was[order] && metres == was_dist[order] && now - said[order] < 100000000LL) return;
    was[order] = packed, was_dist[order] = metres, said[order] = now;
    rx_option_set(HOS_OPT_COOP_DIST, order << 28 | metres);
    rx_option_set(HOS_OPT_COOP_LABEL, packed);
}

static void co_bodies_step(Ctx *c, uint64_t link, int64_t now) {
    if (link != co_link_actor) {                       /* another map: the game took the bodies with the old one */
        co_link_actor = link;
        memset(co_body, 0, sizeof co_body);
    }
    if (co_test_go[0]) {                               /* (test runs: "G <map>") */
        float to[3];
        float away = 0, face[2];
        if (co_test_go[0] == '>' && sscanf(co_test_go + 1, "%f %f %f %f %f", &to[0], &to[1], &to[2], &face[0], &face[1]) == 5) {   /* ("G >x y z tx tz": stood at a place, turned to face a point) */
            float at[12];
            memcpy(at, to, sizeof to);
            co_turn(atan2f(face[0] - to[0], face[1] - to[2]), at + 3);
            const int placed = co_place(c, link, to);
            hos_log("coop: test: stood at %.1f %.1f %.1f (%d) and turned to face %.1f %.1f (%d)", to[0], to[1], to[2], placed, face[0], face[1], co_actor_move(c, link, at));
        } else
        if (co_test_go[0] == '^' && sscanf(co_test_go + 1, "%f %f %f %f", &to[0], &to[1], &to[2], &away) == 4) {
            EnterCriticalSection(&co.lock);
            const float yaw = co.yaw;
            LeaveCriticalSection(&co.lock);
            to[0] -= sinf(yaw) * away, to[2] -= cosf(yaw) * away;
            hos_log("coop: test: stood at %.1f %.1f %.1f, %.1f from the point and facing it (%d)", to[0], to[1], to[2], away, co_place(c, link, to));
        } else
        if (co_test_go[0] == '@' && sscanf(co_test_go + 1, "%f %f %f", &to[0], &to[1], &to[2]) == 3) hos_log("coop: test: stood at %.1f %.1f %.1f (%d)", to[0], to[1], to[2], co_place(c, link, to));
        else hos_log("coop: test: travel to %s (%d)", co_test_go, hos_oot_travel(c, link, co_test_go));
        co_test_go[0] = 0;
    }
    if (InterlockedExchange(&co_test_spawn, 0)) {      /* (test runs: HOS_COOP_TEST=1 and a "Z <actor> <option>" line) */
        float at[3];
        EnterCriticalSection(&co.lock);
        memcpy(at, co.pos, sizeof at);
        const float yaw = co.yaw;
        LeaveCriticalSection(&co.lock);
        at[0] += 2.0f * sinf(yaw), at[2] += 2.0f * cosf(yaw);
        const uint64_t actor = co_create(c, co_test_name, at, yaw, co_test_option);
        hos_log("coop: test: %s made at %.2f %.2f %.2f (option %d): actor %llx, vtable %llx", co_test_name, at[0], at[1], at[2], co_test_option,
                (unsigned long long)actor, (unsigned long long)(actor ? G64(actor) : 0));
        hos_log_flush();
    }
    struct { int want, child, order, foe, id, busy; unsigned drawn; float place[3], yaw, speed, rise; char name[24], look[12], held[CO_GEAR_NAME]; char gear[CO_GEAR][CO_GEAR_NAME]; } who[CO_PEERS];
    /* (CoopFoe_*: the same body with a life and the sensor that takes hits, made by make_bodies.py: a target for all the game
       can hit an enemy with. Used while PvP is on here and there; with it off anywhere the body is CoopPlayer_*, which
       nothing can hit or lock on to.) */
    static int foes = -1;
    if (foes < 0) {
        int hos_mods_has(const char *prefix, const char *suffix);   /* mods.c */
        foes = hos_mods_has("pack/actor/coopfoe_child", ".pack.zs") && hos_mods_has("pack/actor/coopfoe_adult", ".pack.zs");
        hos_log("coop: PvP %s", foes ? "by the game's own combat (the other players' bodies are real targets)" : "by reach (no target bodies installed)");
    }
    const int pvp_here = rx_option(HOS_OPT_COOP_PVP, 0) > 0;
    int listed = 0;
    static int64_t stepped;
    const float dt = stepped ? (float)(now - stepped) * 1e-9f : 0.016f;
    stepped = now;
    float view[12], lens[2];
    const int camera = co_camera(view, lens);
    EnterCriticalSection(&co.lock);
    for (int i = 0; i < CO_PEERS; i++) {
        const CoPeer *p = &co.peer[i];
        who[i].order = p->used ? listed++ : -1;        /* (their place in the menu's list: co_status counts the same way) */
        who[i].want = p->used && p->map[0] && !strcmp(p->map, co_map) && now - p->seen < 3000000000LL;
        if (!who[i].want) continue;
        /* between the last two places said, then on along the same line for a moment: what was said is a thirtieth
           of a second old when it comes, and the next has not come yet */
        const float span = (float)(p->seen - p->seen_before) * 1e-9f, since = (float)(now - p->seen) * 1e-9f;
        const float t = span > 0.005f && span < 0.5f ? (since > 0.15f ? 0.15f : since) / span : 0.0f;
        for (int k = 0; k < 3; k++) who[i].place[k] = p->pos[k] + (p->pos[k] - p->was[k]) * t;
        who[i].yaw = p->yaw + remainderf(p->yaw - p->was_yaw, 6.2831853f) * t;
        {   /* (how fast the body is seen to move, flat and up: what picks its animation. Smoothed, 0 when nothing has come for a moment) */
            static float seen_speed[CO_PEERS], seen_rise[CO_PEERS];
            const float quiet = (float)(now - p->seen) * 1e-9f;
            float rs = 0, rv = 0;
            if (span > 0.005f && span < 0.5f && quiet < 0.35f) {
                const float dx = p->pos[0] - p->was[0], dy = p->pos[1] - p->was[1], dz = p->pos[2] - p->was[2];
                rs = sqrtf(dx * dx + dz * dz) / span, rv = dy / span;
                if (rs > 40.0f) rs = seen_speed[i], rv = 0;   /* (a jump to another place is not a speed) */
            }
            const float k = dt * 10.0f > 1.0f ? 1.0f : dt * 10.0f;
            seen_speed[i] += (rs - seen_speed[i]) * k, seen_rise[i] += (rv - seen_rise[i]) * k;
            who[i].speed = seen_speed[i], who[i].rise = seen_rise[i];
        }
        who[i].child = p->child;
        who[i].foe = foes && pvp_here && p->pvp, who[i].id = p->id, who[i].busy = p->busy;
        snprintf(who[i].name, sizeof who[i].name, "%s", p->name);
        snprintf(who[i].look, sizeof who[i].look, "%s", p->look);
        who[i].drawn = p->drawn, snprintf(who[i].held, sizeof who[i].held, "%s", p->held);
        memcpy(who[i].gear, p->gear, sizeof who[i].gear);
    }
    LeaveCriticalSection(&co.lock);
    for (int i = 0; i < CO_PEERS; i++) {
        CoBody *b = &co_body[i];
        if (b->actor && (!hos_guest_range(b->actor, 0x360) || G64(b->actor) != b->made_of)) b->actor = 0;   /* (gone, or its memory is something else now) */
        if (b->actor && who[i].want && (b->child != who[i].child || b->foe != who[i].foe || strcmp(b->look, who[i].look) || b->remake)) {   /* (they changed age, or PvP went on or off, or their look: this body goes under, the other is made) */
            const float under[3] = {0, -5000, 0};
            co_body_put(b->actor, under, 0);
            b->actor = 0;
        }
        if (!b->actor && who[i].want && now - b->tried > 3000000000LL) {
            b->tried = now;
            /* (CoopPlayer_*: Link's model with Link's animation graph, made by nx2/coop_bodies.py. Without that
               mod the pause menu's Link is used, as before: it stands and slides, it cannot be animated.) */
            static int own = -1;
            if (own < 0) {
                int hos_mods_has(const char *prefix, const char *suffix);   /* mods.c */
                own = hos_mods_has("pack/actor/coopplayer_child", ".pack.zs") && hos_mods_has("pack/actor/coopplayer_adult", ".pack.zs");
                hos_log("coop: the other players are drawn as %s", own ? "CoopPlayer (they move as they play)" : "PouchPlayer (no co-op bodies installed: they stand and slide)");
            }
            const char *base = own && who[i].foe ? (who[i].child ? "CoopFoe_Child" : "CoopFoe_Adult")
                             : own ? (who[i].child ? "CoopPlayer_Child" : "CoopPlayer_Adult") : (who[i].child ? "PouchPlayer_Child" : "PouchPlayer_Adult");
            char name[48];
            co_look_actor(name, sizeof name, base, own, who[i].child, who[i].look);   /* (Navi) */
            char mark[MAX_PATH + 40] = "";
            if (strcmp(name, base)) {                     /* (Navi) noted while it is made: a crash in there turns the colours off next start */
                char game[MAX_PATH];
                co_game_dir(game, sizeof game);
                snprintf(mark, sizeof mark, "%s\\coop", game);
                CreateDirectoryA(mark, NULL);
                snprintf(mark, sizeof mark, "%s\\coop\\look_making.txt", game);
                FILE *m = fopen(mark, "wb");
                if (m) fprintf(m, "%s\n", name), fclose(m);
                else mark[0] = 0;
            }
            b->actor = co_create(c, name, who[i].place, who[i].yaw, 0);
            if (mark[0]) remove(mark);
            if (!b->actor && strcmp(name, base)) {        /* (a look's body that would not be made: the plain one) */
                hos_log("coop: look: %s was not made: drawn as %s", name, base);
                snprintf(name, sizeof name, "%s", base);
                b->actor = co_create(c, name, who[i].place, who[i].yaw, 0);
            }
            b->made_of = b->actor ? G64(b->actor) : 0, b->child = who[i].child, b->posed = 0;
            snprintf(b->look, sizeof b->look, "%s", who[i].look);
            b->foe = own && who[i].foe, b->life_full = 0, b->struck = 0;
            memset(b->worn, 0, sizeof b->worn), memset(b->try_name, 0, sizeof b->try_name), memset(b->wear_tries, 0, sizeof b->wear_tries), b->wear_next = 0;
            b->remake = 0;
            memset(b->hand_set, 0, sizeof b->hand_set), memset(b->hand_tries, 0, sizeof b->hand_tries), memset(b->hand_id, 0, sizeof b->hand_id);
            memset(b->hand_next, 0, sizeof b->hand_next), b->held[0] = 0, b->held_tries = 0;
            hos_log("coop: %s drawn as %s: actor %llx", who[i].name, name, (unsigned long long)b->actor);
            hos_log_flush();
        }
        co_label(who[i].order, camera && who[i].want, view, lens, who[i].place, who[i].child, now);   /* (Navi: the compass shows them before their body is made too) */
        if (!b->actor) continue;
        if (b->foe) {
            /* What my game's combat took off this body's life is what its player loses. The life is the actor's Life
               component's, where the game's own "get life" reads it (QueryGetLife, f_82a36880): [[actor+0x270]+0x2b8] the
               component, [[component+0x1c10]+8] the number, in the units an enemy's life is counted in (the Kokiri Sword
               takes 1, a jump attack 2). Put back at once, so the body never dies here: its player's game decides that.
               One unit is half a heart for a player. */
            const uint64_t parts = G64(b->actor + 0x270);
            const uint64_t life = co_ok(parts + 0x2b8, 8) ? G64(parts + 0x2b8) : 0;
            const uint64_t holder = co_ok(life + 0x1c10, 8) ? G64(life + 0x1c10) : 0;
            if (co_ok(holder, 16)) {
                const uint32_t left = G32(holder + 8);
                if (!b->life_full && left > 0 && left < 100000) b->life_full = left;
                if (b->life_full && left < b->life_full) {
                    const uint32_t taken = b->life_full - left;
                    *(uint32_t *)GP(holder + 8) = b->life_full;
                    HOS_WRITTEN(GP(holder + 8), 4);
                    if (!who[i].busy && now - b->struck > 250000000LL) {   /* (not in their cutscene; and one hit is one hit) */
                        const int quarters = taken >= 4 ? 8 : (int)taken * 2;
                        b->struck = now;
                        co_flinch_until[i] = now + 600000000LL, co_flinch_hard[i] = 1;
                        {   /* (knocked back from me, at once: their own game's push comes a moment later and takes over) */
                            float mine[3];
                            EnterCriticalSection(&co.lock);
                            memcpy(mine, co.pos, sizeof mine);
                            LeaveCriticalSection(&co.lock);
                            float dx = who[i].place[0] - mine[0], dz = who[i].place[2] - mine[2];
                            const float gap = sqrtf(dx * dx + dz * dz);
                            if (gap > 0.05f) dx /= gap, dz /= gap; else dx = 0, dz = 1;
                            b->push[0] = dx, b->push[1] = dz, b->push_far = quarters >= 4 ? 2.2f : 1.4f;
                        }
                        co_send("D %d %d", who[i].id, quarters);
                        co_note("You hit %s", who[i].name);
                        hos_log("coop: pvp: my hit took %u off %s's body: %d quarter hearts", taken, who[i].name, quarters);
                    }
                }
            }
        }
        if (co_test_equip) {                            /* (test runs) */
            int a = 0, n = 0;
            char name[64] = "";
            if (co_test_equip_line[0] == 'E' && sscanf(co_test_equip_line + 2, "%d %63s %d", &a, name, &n) >= 2) {
                const int r = co_equip(c, b->actor, a, name, n);
                hos_log("coop: test: body %d equip slot %d %s number %d -> %d (user %llx)", i, a, name, n, r, (unsigned long long)co_equipment_user(b->actor));
            } else if (co_test_equip_line[0] == 'S' && sscanf(co_test_equip_line + 2, "%d %d", &a, &n) == 2) {
                const int r = co_equip_state(c, b->actor, a, n);
                hos_log("coop: test: body %d equipment state kind %d state %d -> %d", i, a, n, r);
            }
            hos_log_flush();
        }
        if (i == 0 || co_test_play) {                   /* (test runs) */
            static int test = -1;
            if (test < 0) { const char *e = getenv("HOS_COOP_TEST"); test = e && !strcmp(e, "1"); }
            if (test && co_test_play) {
                char names[CO_AS_SLOTS][CO_AS_NAME];
                const int slots = co_as_names(b->actor, names);
                const int ok = co_as_play(c, b->actor, co_test_play_slot, co_test_play_name);
                char after[CO_AS_SLOTS][CO_AS_NAME];
                co_as_names(b->actor, after);
                hos_log("coop: test: body %d (%d slots, slot %d played '%s'): play '%s' -> %d, now '%s'", i, slots, co_test_play_slot,
                        slots > co_test_play_slot ? names[co_test_play_slot] : "?", co_test_play_name, ok,
                        slots > co_test_play_slot ? after[co_test_play_slot] : "?");
                hos_log_flush();
            }
        }
        if (who[i].want) {
            const float since_hit = b->struck ? (float)(now - b->struck) * 1e-9f : 9.0f;
            if (b->push_far > 0 && since_hit < 0.6f) {      /* (out fast, held, then back to where their game says they are) */
                const float k = since_hit < 0.12f ? since_hit / 0.12f : since_hit < 0.3f ? 1.0f : 1.0f - (since_hit - 0.3f) / 0.3f;
                const float out = b->push_far * (since_hit < 0.12f ? 1.0f - (1.0f - k) * (1.0f - k) : k);
                who[i].place[0] += b->push[0] * out, who[i].place[2] += b->push[1] * out;
            }
            co_body_put(b->actor, who[i].place, who[i].yaw);
            co_anim_put(c, b->actor, i, who[i].child, who[i].speed, who[i].rise, who[i].busy);
            co_gear_apply(c, b, i, who[i].child, who[i].gear, now);
            co_sword_probe_body(b->actor);   /* (Navi) reads only */
            co_hands_apply(c, b, i, who[i].drawn, who[i].held, now);   /* (Navi) */
            b->posed = 1;
        } else { const float under[3] = {0, -5000, 0}; co_body_put(b->actor, under, 0); }
    }
}

/* Is the game in the middle of something it must be left alone in: an event playing (a cutscene, a conversation,
   a chest opening: the event manager's own "an event is playing", as hos_main.c's teleport asks it, EventMgr +104),
   or a map loading (Sequence_IsLoadingBanc). What the others found and did is handed over once it is over: a new
   file that was given the host's whole progress in the middle of its opening scene did not get out of that scene. */
#define CO_EVENTMGR_SLOT 0x84c9e090ULL
static int co_game_busy(void) {
    const uint64_t holder = G64(CO_EVENTMGR_SLOT);
    const uint64_t events = hos_guest_range(holder, 8) ? G64(holder) : 0;
    if (hos_guest_range(events, 112) && G64(events + 104)) return 1;
    CoFlags f;
    const uint8_t *loading = co_flags_open(&f) ? co_flag_value(&f, 0xef2b60c2u, NULL) : NULL;
    return loading && (*loading & 1);
}

/* ---- one enemy for everyone ----
   Each game runs its own copy of an enemy. What makes it one enemy is that its life is one: a hit that takes life off it
   in one game takes the same off it in every game, and it dies for all when all the hits together have used it up.
   nx2/oot_patches.py has f_804bd8c0, the Life component's update (every actor with a life, each frame it is updated),
   call oot_coop_life_seen at its head and oot_coop_life_write where it takes life away. The component's actor is at +0x18 and
   its life at [+0x1c10]+8; an actor's name is at [+0x20] and its place at +796.
   The same enemy in two games: the same name, first seen at the same place (an enemy is made where the map puts it, in
   every game alike). Told as "U <name's hash> <x> <y> <z> <life taken>".
   A hit from another player is owed to the enemy until its next update, and given there the game's own way: the Life
   component's message handler (vtable +0x90, f_82e67f60) takes {-, kind 0x46030000, -, -> {amount, -, flags 1}} and adds
   the amount to what the update then applies to the life: a negative amount is damage, a positive one heals, and the
   life stays between 0 and its most (tried: -1000 left a Deku Baba at 0, +1000 at its full 2). A life
   written as 0 by hand does not kill: the enemy stood there with no life. An enemy far from Link is not updated: it
   takes what it is owed when Link comes near. What the update then takes off is not told back to the others (expect).
   Several of the game's threads run the life function at once: co_lives_lock. */
#define CO_LIVES 256
#define CO_LEAD_SLOTS 6            /* (an enemy's animation slots said and followed; its inputs' words) */
#define CO_LEAD_IN 96
#define CO_BLOWS 128
typedef struct {
    uint64_t life, actor; uint32_t hash, owed, expect; float born[3]; int64_t seen, expect_until;
    /* (in one place) */
    int64_t first, said, heard, put, noted;             /* first updated this time; my last word; the leader's last word; my last put; a command last asked */
    int heard_from, mine, placed;
    float lead[12], at[12];                             /* the leader's place and turn; where my copy is on its way there */
    char lead_cmd[CO_LEAD_SLOTS][CO_AS_NAME], tried_cmd[CO_LEAD_SLOTS][CO_AS_NAME], said_cmd[CO_AS_NAME];
    int64_t tried_at[CO_LEAD_SLOTS];
    uint8_t lead_slots, tried_bad[CO_LEAD_SLOTS];
    uint32_t lead_in[CO_LEAD_IN], lead_words, said_n;   /* the leader's animation inputs, and how many words they are */
    float lead_far;                                     /* how far the leader's Link is from it */
    int boss;                                           /* a boss: its place is left to each game */
    int64_t hit_at, hurt_at, claim_until;               /* my own attack last hit it; my hit last took life off it; I say it is mine (a boss) until then */
    int64_t nearer_since;                               /* since when my Link has been clearly nearer than the leader's */
} CoLife;
typedef struct { uint32_t hash, taken; float at[3]; int64_t since; } CoBlow;
static CoLife co_lives[CO_LIVES];
static CoBlow co_blows[CO_BLOWS];
static SRWLOCK co_lives_lock = SRWLOCK_INIT;
static volatile LONG co_lives_said;
#define CO_BLOW_KEPT 1200000000000LL            /* (a hit on an enemy this game has not loaded: kept twenty minutes) */

/* (the net's lines, taken on the game's side) another player's hit */
static void co_enemy_hit(uint32_t hash, const float at[3], uint32_t taken) {
    const int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&co_lives_lock);
    for (int i = 0; i < CO_LIVES; i++) {            /* (an enemy this game knows: owed to it) */
        CoLife *e = &co_lives[i];
        const float dx = at[0] - e->born[0], dy = at[1] - e->born[1], dz = at[2] - e->born[2];
        if (!e->life || e->hash != hash || dx * dx + dy * dy + dz * dz > 6.25f) continue;
        e->owed += taken;
        ReleaseSRWLockExclusive(&co_lives_lock);
        hos_log("coop: enemy %x: another player's hit took %u (given at its next update)", hash, taken);
        return;
    }
    CoBlow *b = &co_blows[0];                           /* (not loaded here: kept for when it is) */
    for (int i = 0; i < CO_BLOWS; i++) {
        if (!co_blows[i].taken || now - co_blows[i].since > CO_BLOW_KEPT) { b = &co_blows[i]; break; }
        if (co_blows[i].since < b->since) b = &co_blows[i];
    }
    b->hash = hash, b->taken = taken, b->since = now;
    memcpy(b->at, at, sizeof b->at);
    ReleaseSRWLockExclusive(&co_lives_lock);
}

/* The game's own life message: so much taken off this life at its update. */
static int co_enemy_damage(Ctx *c, uint64_t life, uint32_t amount) {
    const uint64_t vtable = G64(life);
    A64Fn tell = co_ok(vtable, 0x98) ? a64_lookup(G64(vtable + 0x90)) : NULL;
    if (!tell) return 0;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x80) & ~0xfULL;
    memset(A64_PTR(frame), 0, 0x80);
    recomp_store32_sp(c, frame + 0, 0xffffffffu);
    recomp_store32_sp(c, frame + 4, 0x46030000u);
    recomp_store32_sp(c, frame + 8, 0xffffffffu);
    recomp_store64_sp(c, frame + 0x10, frame + 0x20);
    recomp_store32_sp(c, frame + 0x20, (uint32_t)-(int32_t)(amount > 100000 ? 100000 : amount));
    recomp_store32_sp(c, frame + 0x28, 1);
    c->x[31] = frame;
    c->x[0] = life, c->x[1] = frame;
    tell(c);
    *c = saved;
    return 1;
}

/* ---- one enemy, in one place ----
   Every game still has its own copy of an enemy, and one of them leads it: its game alone decides where the enemy goes
   and what it does, and says so fifteen times a second:
       "Q <name's hash> <x> <y> <z where it was made> <how far the leader's Link is from it> <its place and turn: 12
          floats, base64> <what each animation slot plays: names with commas between, - for none>
          [<words>:<its animation inputs, base64>]"
   (the inputs with every third line: the values its blends run on, as "how another player moves" has them for Link).
   The other games move their copy there by the game's own request (co_actor_move), have it play the same commands
   and give it the same inputs, every frame it is updated, over whatever their own copy's mind would do.
   Who leads: a game that starts updating an enemy listens for 0.4 s first. If somebody is saying it, they lead it and
   this game follows; if nobody is, this game leads. After that the nearest player's game leads, so the enemy fights
   whoever is closest as it does alone: a follower whose Link has been two metres nearer than the leader's for half a
   second takes it, and a leader that hears another say it from a metre nearer lets go (within a metre of each other,
   the lower number keeps it). A leader that stops saying it (walked off, left the map) is followed for 0.7 s more,
   then the next game takes it from where it stands.
   All of it is done inside the enemy's own update (the life function's head), so the actor is there. */
static void co_enemy_led(int from, const char *text) {     /* (the net thread) */
    unsigned hash = 0;
    float born[3] = {0, 0, 0}, lead[12], far_off = 0;
    char packed[80] = "", cmds[CO_LEAD_SLOTS * CO_AS_NAME + 8] = "", inputs[700] = "";
    if (sscanf(text, "%x %f %f %f %f %79s %295s %699s", &hash, &born[0], &born[1], &born[2], &far_off, packed, cmds, inputs) < 7) return;
    if (co_unbase64(packed, (uint8_t *)lead, sizeof lead) != sizeof lead) return;
    for (int i = 0; i < 12; i++) if (!(lead[i] > -1e6f && lead[i] < 1e6f)) return;
    char cmd[CO_LEAD_SLOTS][CO_AS_NAME];
    int slots = 0;
    memset(cmd, 0, sizeof cmd);
    for (char *at = cmds; at && *at && slots < CO_LEAD_SLOTS; slots++) {
        char *end = strchr(at, ',');
        const size_t n = end ? (size_t)(end - at) : strlen(at);
        if (n < CO_AS_NAME && !(n == 1 && at[0] == '-')) memcpy(cmd[slots], at, n);
        at = end ? end + 1 : NULL;
    }
    uint32_t in[CO_LEAD_IN];
    unsigned words = 0;
    const char *colon = strchr(inputs, ':');
    if (colon && sscanf(inputs, "%u", &words) == 1 && words > 0 && words <= CO_LEAD_IN) {
        if (co_unbase64(colon + 1, (uint8_t *)in, sizeof in) != words * 4) words = 0;
    } else words = 0;
    const int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&co_lives_lock);
    CoLife *best = NULL;
    float best_d = 6.25f;
    for (int i = 0; i < CO_LIVES; i++) {
        CoLife *e = &co_lives[i];
        if (!e->life || e->hash != hash) continue;
        const float dx = born[0] - e->born[0], dy = born[1] - e->born[1], dz = born[2] - e->born[2], d = dx * dx + dy * dy + dz * dz;
        if (d <= best_d) best = e, best_d = d;
    }
    if (best) {
        memcpy(best->lead, lead, sizeof lead);
        memcpy(best->lead_cmd, cmd, sizeof cmd), best->lead_slots = (uint8_t)slots;
        if (words) memcpy(best->lead_in, in, words * 4), best->lead_words = words;
        best->heard = now, best->heard_from = from, best->lead_far = far_off;
    }
    ReleaseSRWLockExclusive(&co_lives_lock);
}

static void co_upright(float m[9]) {                    /* (a turn mixed from two turns: made square again) */
    float *a = m, *b = m + 3, *d = m + 6;
    float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (n < 1e-4f) { const float one[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(m, one, sizeof one); return; }
    for (int i = 0; i < 3; i++) a[i] /= n;
    const float along = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    for (int i = 0; i < 3; i++) b[i] -= a[i] * along;
    n = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (n < 1e-4f) { const float one[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(m, one, sizeof one); return; }
    for (int i = 0; i < 3; i++) b[i] /= n;
    d[0] = a[1] * b[2] - a[2] * b[1], d[1] = a[2] * b[0] - a[0] * b[2], d[2] = a[0] * b[1] - a[1] * b[0];
}

/* Moving an actor that has physics (an enemy): the fields co_body_put writes are not looked at, the game's own request
   is: f_80c8fd60(actor, a 3x4 matrix: the turn's rows, each with the place's part after it, 0). With a physics part
   ([actor+0x270]+0x50) it hands the place to that (f_804e6630); without, it writes the fields. All 21 of its callers
   pass 0 last. */
static int co_actor_move(Ctx *c, uint64_t actor, const float at[12]) {
    A64Fn ask = a64_lookup(0x80c8fd60ULL);
    if (!ask) return 0;
    float m[12];
    for (int i = 0; i < 3; i++) memcpy(m + i * 4, at + 3 + i * 3, 12), m[i * 4 + 3] = at[i];
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x40) & ~0xfULL;
    memcpy(A64_PTR(frame), m, sizeof m);
    c->x[31] = frame;
    c->x[0] = actor, c->x[1] = frame, c->x[2] = 0;
    ask(c);
    *c = saved;
    return 1;
}

/* What hit an actor this frame, as the game keeps it for its own questions (the script node QuerySensorHitIsHitTg,
   f_82afe200): [[[actor+0x270]+0x3b8]+0x68] is the list of hits taken: +8 its room, +0x10 the records (0x1d0 bytes
   each), +0x18 how many now. A record's +0x60 is a link to the one who hit: +8 the actor, +0x10 a number (-1: none).
   1 when one of them is this game's own doing (Link, or a thing of his: not an enemy, not another player's body). */
static int co_enemy_hits(uint64_t actor, uint32_t hash) {
    static volatile LONG said;
    const uint64_t parts = G64(actor + 0x270);
    const uint64_t part = co_ok(parts + 0x3b8, 8) ? G64(parts + 0x3b8) : 0;
    const uint64_t list = co_ok(part + 0x68, 8) ? G64(part + 0x68) : 0;
    if (!co_ok(list, 0x20)) return 0;
    const uint32_t count = G32(list + 0x18), room = G32(list + 8);
    const uint64_t records = G64(list + 0x10);
    if (!count || !room || room > 64 || count > room || !co_ok(records, room * 0x1d0ULL)) return 0;
    int mine = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t at = records + i * 0x1d0ULL;
        const uint64_t by = G32(at + 0x70) != 0xffffffffu ? G64(at + 0x68) : 0;
        char name[48] = "";
        if (co_ok(by, 0x30) && G64(by) == 0x84b45e18ULL) {
            const uint64_t name_at = G64(by + 0x20);
            if (co_ok(name_at, 48)) snprintf(name, sizeof name, "%.47s", (const char *)GP(name_at));
        }
        /* (the one who hit is most often a thing with no name to read here: a seed's link is not to an actor. Whatever is
           not an enemy or another player's body is this game's own doing: nothing else in a game hits an enemy) */
        const int own = strncmp(name, "Enemy_", 6) && strncmp(name, "Coop", 4);
        mine |= own;
        if (InterlockedIncrement(&said) <= 6)             /* (the first few, for the log) */
            hos_log("coop: enemy %x: hit by %s (link %x), record %u of %u", hash, name[0] ? name : "?", G32(at + 0x70), i + 1, count);
    }
    return mine;
}

static void co_enemy_lead_step(Ctx *c, uint64_t life, uint64_t actor, int64_t now, int hit_by_me) {
    static int follow_only = -1;                        /* (test runs: HOS_COOP_FOLLOW=1, this game never leads; 2: only by its own hit on a boss) */
    static volatile LONG said_roles, said_plays;
    if (follow_only < 0) { const char *t = getenv("HOS_COOP_TEST"), *f = getenv("HOS_COOP_FOLLOW"); follow_only = t && !strcmp(t, "1") && f ? atoi(f) : 0; }
    /* (On: in test runs with a stand-in for the other player a copy was seen to follow its leader's place and to play its commands, and the lead to pass to
       the nearer player and back. Two real games have not tried it yet: HOS_COOP_ENEMY_PLACE=0 in longshot_env.txt
       turns it off; health and death are shared either way. Bosses too (HOS_COOP_BOSS=0 leaves them to each game):
       Gohma's fight was watched through a following copy, see below.) */
    static int on = -1;
    if (on < 0) { const char *e = getenv("HOS_COOP_ENEMY_PLACE"); on = !(e && !strcmp(e, "0")); hos_log("coop: enemies in one place: %s", on ? "on" : "off (HOS_COOP_ENEMY_PLACE=0)"); }
    if (!on || co.links <= 0) return;
    /* (My game is in a cutscene or a conversation: what my copy does there is the scene's, not the fight's. It is neither
       said to the others nor moved by them until the scene is over; another game leads it meanwhile.) */
    if (co_busy_now) return;
    int role = 0, took = 0, note = 0, want_slots = 0, with_inputs = 0, changed = 0;
    uint32_t hash = 0, in[CO_LEAD_IN], in_words = 0;
    float born[3], at[12];
    char want[CO_LEAD_SLOTS][CO_AS_NAME], was[CO_AS_NAME] = "";
    uint8_t may_play[CO_LEAD_SLOTS] = {0};
    float my_far;
    {   /* (how far my Link is from it) */
        float mine[3], its[3];
        EnterCriticalSection(&co.lock);
        memcpy(mine, co.pos, sizeof mine);
        LeaveCriticalSection(&co.lock);
        memcpy(its, GP(actor + CO_ACTOR_PLACE), sizeof its);
        my_far = sqrtf((mine[0] - its[0]) * (mine[0] - its[0]) + (mine[1] - its[1]) * (mine[1] - its[1]) + (mine[2] - its[2]) * (mine[2] - its[2]));
    }
    AcquireSRWLockExclusive(&co_lives_lock);
    CoLife *e = NULL;
    for (int i = 0; i < CO_LIVES && !e; i++) if (co_lives[i].life == life && co_lives[i].actor == actor) e = &co_lives[i];
    static int bosses = -1;                             /* (bosses too, unless HOS_COOP_BOSS=0) */
    if (bosses < 0) { const char *b = getenv("HOS_COOP_BOSS"); bosses = !(b && !strcmp(b, "0")); }
    if (!e || (e->boss && !bosses)) { ReleaseSRWLockExclusive(&co_lives_lock); return; }
    hash = e->hash, memcpy(born, e->born, sizeof born);
    if (hit_by_me && !e->hit_at) e->hit_at = now;
    const int heard = e->heard && now - e->heard < 700000000LL;
    /* (A boss keeps its leader: its fight is a string of states only the leader's copy is in, and a new leader's copy
       would go on from its own. Two that lead it: the lower number keeps it. It changes hands only when its leader
       stops saying it.) */
    /* (But a follower whose own attack hit it and took no life takes it: a seed in Gohma's eye, say. What a boss does
       about such a hit, going down stunned, is decided by the copy that is in the fight's state, and a follower's copy
       is not: it blocks. So the boss becomes that player's, and his next shot stuns it as it does alone: seen in
       a test run (GuardShock on the shot that took it over, Down_Start on the next). A hit that
       takes life, a sword on a boss already down, changes nothing here: the life is shared, and the boss stays with
       the one whose copy has it down. The new leader says so with a distance below zero for six seconds; a leader
       that hears that lets go, and nobody else takes it meanwhile: two that say it at once, the lower number keeps
       it. The hit is judged an update late: the life it takes is taken in the update it is seen in.) */
    const int is_boss = e->boss, claiming = now < e->claim_until, they_claim = e->lead_far < 0;
    if (is_boss && !e->mine && heard && follow_only != 1 && !they_claim && e->hit_at && now - e->hit_at > 60000000LL) {
        if (now - e->hit_at < 500000000LL && e->hurt_at < e->hit_at) e->mine = 1, e->claim_until = now + 6000000000LL, took = 5;
        e->hit_at = 0;
    } else if (e->hit_at && now - e->hit_at > 500000000LL) e->hit_at = 0;
    if (took == 5) ;
    else if (e->mine && heard && (is_boss ? (they_claim && (!claiming || e->heard_from < (int)co.my_id)) || (!they_claim && !claiming && e->heard_from < (int)co.my_id)
                                     : e->lead_far + 1.0f < my_far || (e->lead_far <= my_far + 1.0f && e->heard_from < (int)co.my_id))) e->mine = 0, e->placed = 0, took = 2;
    if (!e->mine && heard && !follow_only && !is_boss && my_far + 2.0f < e->lead_far) {
        if (!e->nearer_since) e->nearer_since = now;
        else if (now - e->nearer_since > 500000000LL) e->mine = 1, e->nearer_since = 0, took = 4;
    } else e->nearer_since = 0;
    if (!e->mine && !heard && now - e->first > 400000000LL && !follow_only) e->mine = 1, took = 1;
    if (e->mine && now < e->claim_until) my_far = -1.0f;
    if (e->mine) {
        if (now - e->said >= 66000000LL) e->said = now, role = 1, with_inputs = e->said_n++ % 3 == 0, memcpy(was, e->said_cmd, CO_AS_NAME);
    } else if (heard) {
        role = 2;
        if (!e->placed) memcpy(e->at, GP(actor + CO_ACTOR_PLACE), sizeof e->at), took = 3;   /* (from where my copy stands) */
        float dt = e->placed ? (float)(now - e->put) * 1e-9f : 0.016f;
        if (dt > 0.1f) dt = 0.1f;
        const float dx = e->lead[0] - e->at[0], dy = e->lead[1] - e->at[1], dz = e->lead[2] - e->at[2];
        const float k = dx * dx + dy * dy + dz * dz > 64.0f ? 1.0f : 1.0f - expf(-dt * 16.0f);   /* (far off: there at once) */
        for (int i = 0; i < 12; i++) e->at[i] += (e->lead[i] - e->at[i]) * k;
        co_upright(e->at + 3);
        e->put = now, e->placed = 1;
        memcpy(at, e->at, sizeof at);
        memcpy(want, e->lead_cmd, sizeof want), want_slots = e->lead_slots;
        for (int i = 0; i < want_slots; i++) may_play[i] = strcmp(want[i], e->tried_cmd[i]) || !e->tried_bad[i] || now - e->tried_at[i] > 1000000000LL;
        in_words = e->lead_words;
        if (in_words) memcpy(in, e->lead_in, in_words * 4);
        if (now - e->noted > 500000000LL) e->noted = now, note = 1;
    }
    ReleaseSRWLockExclusive(&co_lives_lock);
    if (took && InterlockedIncrement(&said_roles) <= 60)
        hos_log("coop: enemy %x: %s (my Link is %.1f from it)", hash, took == 1 ? "this game leads it" : took == 2 ? "another game leads it now (nearer, or as near with the lower number)"
                : took == 4 ? "my Link is the nearer one: this game leads it now" : took == 5 ? "my own attack hit it: this game leads it now" : "another game leads it: my copy follows", my_far);
    if (role == 1) {
        char names[CO_AS_SLOTS][CO_AS_NAME], packed[72];
        const int slots = co_as_names(actor, names);
        static volatile LONG said_parts;
        if (took == 1 && InterlockedIncrement(&said_parts) <= 30) {   /* (what there is to say of how it moves) */
            char all[400] = "";
            size_t used = 0;
            for (int i = 0; i < slots && used + 60 < sizeof all; i++) if (names[i][0]) used += (size_t)snprintf(all + used, sizeof all - used, " %d:%s", i, names[i]);
            const uint64_t parts = G64(actor + 0x270), as0 = co_ok(parts + 0x40, 8) ? G64(parts + 0x40) : 0, ctl = co_as_ctl(actor);
            uint32_t words = 0;
            const uint64_t inputs = co_inputs_at(actor, &words);
            hos_log("coop: enemy %x: animation part %llx, controller %llx (vtable %llx), %d slots:%s; inputs %llx, %u words", hash, (unsigned long long)as0,
                    (unsigned long long)ctl, (unsigned long long)(ctl ? G64(ctl) : 0), slots, all, (unsigned long long)inputs, words);
        }
        co_base64((const uint8_t *)GP(actor + CO_ACTOR_PLACE), 48, packed);
        char cmds[CO_LEAD_SLOTS * CO_AS_NAME + 8] = "", inputs[600] = "";
        size_t used = 0;
        for (int i = 0; i < slots && i < CO_LEAD_SLOTS; i++) used += (size_t)snprintf(cmds + used, sizeof cmds - used, "%s%s", i ? "," : "", names[i][0] ? names[i] : "-");
        if (!used) snprintf(cmds, sizeof cmds, "-");
        if (with_inputs) {
            uint32_t words = 0;
            const uint64_t from = co_inputs_at(actor, &words);
            if (from && words && words <= CO_LEAD_IN) {
                const int n = snprintf(inputs, sizeof inputs, " %u:", words);
                co_base64((const uint8_t *)GP(from), words * 4ULL, inputs + n);
            }
        }
        co_send("Q %x %.1f %.1f %.1f %.1f %s %s%s", hash, born[0], born[1], born[2], my_far, packed, cmds, inputs);
        if (slots > 0 && strcmp(was, names[0])) {       /* (for the log: what it goes through) */
            static volatile LONG said_cmds;
            AcquireSRWLockExclusive(&co_lives_lock);
            if (e->life == life && e->actor == actor) memcpy(e->said_cmd, names[0], CO_AS_NAME);
            ReleaseSRWLockExclusive(&co_lives_lock);
            if (InterlockedIncrement(&said_cmds) <= 150 || is_boss) hos_log("coop: enemy %x: plays %s", hash, cmds);
        }
    } else if (role == 2) {
        uint8_t *a = (uint8_t *)GP(actor);
        static volatile LONG said_places;
        if (note && InterlockedIncrement(&said_places) <= 60) {
            float is[3];
            memcpy(is, a + CO_ACTOR_PLACE, sizeof is);
            hos_log("coop: enemy %x: my copy is at %.2f %.2f %.2f, the leader's wanted at %.2f %.2f %.2f", hash, is[0], is[1], is[2], at[0], at[1], at[2]);
        }
        if (!co_actor_move(c, actor, at)) {
            memcpy(a + CO_ACTOR_ASK_TURN + 4, at + 4, 32);
            memcpy(a + CO_ACTOR_ASK_PLACE + 4, at + 1, 8);
            memcpy(a + CO_ACTOR_ASK_TURN, at + 3, 4);
            memcpy(a + CO_ACTOR_ASK_PLACE, at, 4);
            HOS_WRITTEN(a + CO_ACTOR_ASK_PLACE, 48);
        }
        uint32_t words = 0;
        const uint64_t to = in_words ? co_inputs_at(actor, &words) : 0;
        if (to && words == in_words) {                  /* (the leader's inputs: the same graph has as many) */
            for (uint32_t i = 0; i < words; i++)
                if (!co_input_is_address(to + i * 4ULL)) *(uint32_t *)GP(to + i * 4ULL) = in[i];
            HOS_WRITTEN(GP(to), words * 4ULL);
        }
        char names[CO_AS_SLOTS][CO_AS_NAME];
        const int slots = co_as_names(actor, names);
        uint8_t bad[CO_LEAD_SLOTS] = {0};
        for (int i = 0; i < slots && i < want_slots; i++) {
            if (!strcmp(want[i], names[i])) continue;
            if (!want[i][0]) { co_as_remove(c, actor, i); continue; }
            if (!may_play[i]) continue;
            const int ok = co_as_play(c, actor, i, want[i]);
            char after[CO_AS_SLOTS][CO_AS_NAME];
            bad[i] = (co_as_names(actor, after) <= i || strcmp(after[i], want[i])) ? 2 : 1;
            changed = 1;
            if (InterlockedIncrement(&said_plays) <= 80)
                hos_log("coop: enemy %x: slot %d: my copy played '%s', the leader's plays '%s': asked (%d), now '%s'", hash, i, names[i], want[i], ok, bad[i] == 2 ? after[i] : want[i]);
        }
        if (changed) {
            AcquireSRWLockExclusive(&co_lives_lock);
            if (e->life == life && e->actor == actor)
                for (int i = 0; i < CO_LEAD_SLOTS; i++)
                    if (bad[i]) memcpy(e->tried_cmd[i], want[i], CO_AS_NAME), e->tried_at[i] = now, e->tried_bad[i] = bad[i] == 2;
            ReleaseSRWLockExclusive(&co_lives_lock);
        }
    }
}

void oot_coop_life_seen(Ctx *c, uint64_t life) {
    if (!co.ready || !co.want || !co_ok(life, 0x1c20)) return;
    const uint64_t actor = G64(life + 0x18);
    if (!co_ok(actor, 0x390)) return;
    const uint64_t name_at = G64(actor + 0x20);
    if (!co_ok(name_at, 48) || memcmp(GP(name_at), "Enemy_", 6)) return;
    const uint64_t holder = G64(life + 0x1c10);
    if (!co_ok(holder, 16)) return;
    const int64_t now = hos_now_ns();
    int fresh = 0;
    char name[48] = "";
    AcquireSRWLockExclusive(&co_lives_lock);
    CoLife *e = NULL, *old = &co_lives[0];
    for (int i = 0; i < CO_LIVES && !e; i++) {
        if (co_lives[i].life == life && co_lives[i].actor == actor) e = &co_lives[i];
        else if (co_lives[i].seen < old->seen) old = &co_lives[i];
    }
    if (!e) e = old, memset(e, 0, sizeof *e), e->life = life, e->actor = actor, fresh = 1;
    else if (now - e->seen > 3000000000LL) {            /* (not updated for a while: is the memory still this enemy's?) */
        snprintf(name, sizeof name, "%.47s", (const char *)GP(name_at));
        if (co_mm3(name) != e->hash) fresh = 1, e->owed = e->expect = 0, e->heard = 0;
    }
    if (fresh || now - e->seen > 500000000LL) e->mine = 0, e->first = now, e->placed = 0;   /* (updated again: who leads it is asked again) */
    e->seen = now;
    if (fresh) {
        snprintf(name, sizeof name, "%.47s", (const char *)GP(name_at));
        e->hash = co_mm3(name);
        memcpy(e->born, GP(actor + 796), sizeof e->born);
        e->boss = strstr(name, "Boss") != NULL;
    }
    for (int i = 0; i < CO_BLOWS; i++) {                /* (hits that came before this game had it) */
        CoBlow *b = &co_blows[i];
        if (!b->taken || b->hash != e->hash || now - b->since > CO_BLOW_KEPT) continue;
        const float dx = b->at[0] - e->born[0], dy = b->at[1] - e->born[1], dz = b->at[2] - e->born[2];
        if (dx * dx + dy * dy + dz * dz > 6.25f) continue;   /* (another of its kind) */
        e->owed += b->taken, b->taken = 0;
    }
    const uint32_t owed = e->owed, hash = e->hash;
    if (owed) e->owed = 0, e->expect = (now < e->expect_until ? e->expect : 0) + owed, e->expect_until = now + 1500000000LL;
    float born[3];
    memcpy(born, e->born, sizeof born);
    ReleaseSRWLockExclusive(&co_lives_lock);
    if (fresh && InterlockedIncrement(&co_lives_said) <= 80)
        hos_log("coop: enemy: %s, life %u: U %x %.1f %.1f %.1f", name, G32(holder + 8), hash, born[0], born[1], born[2]);
    if (owed) {
        const uint32_t left = G32(holder + 8);
        const int told = co_enemy_damage(c, life, owed);
        hos_log("coop: enemy %x: the others' hits, %u in all, given to it (%d): it had %u", hash, owed, told, left);
    }
    co_enemy_lead_step(c, life, actor, now, co_enemy_hits(actor, hash));
}

void oot_coop_life_write(Ctx *c, uint64_t life, uint32_t was, uint32_t now_left) {
    (void)c;
    if (!co.ready || !co.want || now_left >= was || !co_ok(life, 0x20)) return;
    const uint64_t actor = G64(life + 0x18);
    const int64_t now = hos_now_ns();
    int found = 0;
    uint32_t hash = 0, mine = was - now_left;
    float born[3] = {0, 0, 0};
    AcquireSRWLockExclusive(&co_lives_lock);
    for (int i = 0; i < CO_LIVES && !found; i++) {
        CoLife *e = &co_lives[i];
        if (e->life != life || e->actor != actor) continue;
        found = 1, hash = e->hash, memcpy(born, e->born, sizeof born);
        if (now >= e->expect_until) e->expect = 0;
        const uint32_t theirs = e->expect < mine ? e->expect : mine;   /* (the others' hits being taken: not mine to tell) */
        e->expect -= theirs, mine -= theirs;
        if (mine) e->hurt_at = now;
        if (!now_left) e->expect = 0;
    }
    ReleaseSRWLockExclusive(&co_lives_lock);
    if (!found) return;                                 /* (not an enemy: Link, a sign, a rock) */
    if (mine && co.links > 0) co_send("U %x %.1f %.1f %.1f %u", hash, born[0], born[1], born[2], mine);
    hos_log("coop: enemy %x: life %u -> %u (%u of it my hit%s)", hash, was, now_left, mine, mine && co.links > 0 ? ", told to the others" : "");
}

/* The game's side is two callers. oot_coop_frame runs every frame shown, on the thread that shows it (host/nn_vi.c
   hos_vi_native_presented): the menu's rows, the link's comings and goings, the notes. It runs at the title screen too, so a
   link is up before a save is loaded. co_tick runs from Link's own update, so only while a save is being
   played: everything that reads or changes the game. co.game keeps the two apart. */

/* ==== (Navi) the world: placed objects broken, taken, moved and cut, for everyone =================================
   After daurosk's oot-remake-multiplayer (GPL-3.0), docs/world.md, objects.md, flags.md and dungeon.md: what each
   kind of placed object is, the rules for when one counts as broken here, and the game's own calls. Their addresses
   are for this same build (e0f3ccfa, 1.1.1) as offsets from `main`; here main is at 0x80020000 (their GameDataMgr slot
   main+0x4c7e180 is CO_GAMEDATA_SLOT 0x84c9e180, their physics setter main+0x4c6630 is f_804e6630).

   THE OBJECTS: every actor, from the game's one list of them, a few times a second: [[[0x84eb1888]+0x118]+0x670] is
   the list's head (a ring of {next, prev, owner}; an actor's own node is at actor+0x538; +0x10 how many). An actor's
   name is at [+0x20], its place +0x31c, its turn +0x328, its state +0x2c (7..11: on its way out), its placement hash
   [+0x3d8]+0x88 (one number for the same placed object in every game; ~0 for one the game made). A walk whose count
   does not match is dropped (the list changes on other threads); an object is gone only after two walks without it.

   SAID (beside the enemies' "U" and "Q"):
     wb <map> <hash> <x> <y> <z> <flags>   broken or taken here (flags 1: back again - forget it; 2: kept, a key or lock)
     wm <map> <hash> <12 floats, base64>   I am moving it (place and turn), up to 20 times a second
     wp <map> <hash> <12 floats, base64>   it came to rest here
     wg <map> <the 0x3c-byte cut, base64>  I cut field grass (the game's foliage cut, as its queue holds it)
   Every game keeps what it knows per map and says it all again when somebody joins; what is not kept (pots, grass,
   moved blocks) is forgotten for a map that this game enters while no other player is on it: the game puts those
   back by itself on a load, as it does alone.

   NOT DONE HERE: signposts cut by the sword (their own runs show a replayed hit does not cut), a key given back when
   two players took the same key at once, and the drops of a broken pot (each game drops its own). */
#define CO_WORLD_N 512
#define CO_WB_N 1024
#define CO_WP_N 256
#define CO_WG_N 512
enum { CW_NONE, CW_BREAK, CW_GRASS, CW_NOLIFE, CW_KEY, CW_LOCK, CW_TOKEN, CW_FALLER, CW_BLOCK, CW_SHIFTER, CW_FLOOR, CW_ROLLER };
typedef struct {
    uint64_t actor, hash;
    uint8_t kind, carry, missed, gone_said, applied, follow, ai;
    char name[48];
    float first[3], rest[3], at[12];
    int64_t first_seen, gone_at, still_since, sent_at, follow_until, hold_until, settle_until, delete_at, faller_next;
    int faller_tries, broken_here;
    float old_rest[3], last_at[3];
} CoThing;
typedef struct { char map[64]; uint64_t hash; float at[3]; int flags; } CoBreak;
typedef struct { char map[64]; uint64_t hash; float m[12]; int64_t heard; int moving, from; } CoPlace;
typedef struct { char map[64]; uint8_t raw[0x3c]; int64_t made; int mine; } CoCut;
static CoThing co_things[CO_WORLD_N];
static CoBreak co_wb[CO_WB_N]; static int co_wb_n;
static CoPlace co_wp[CO_WP_N]; static int co_wp_n;
static CoCut co_wg[CO_WG_N]; static int co_wg_n;
static int64_t co_world_map_since;                      /* (when this game came onto the map it is on) */

static int co_is(const char *name, const char *part) { return strstr(name, part) != NULL; }

/* What kind of shared object a placed actor is, by its name (their tables, checked against this game's actor list). */
static int co_world_kind(const char *n, uint8_t *carry) {
    *carry = 0;
    if (!strncmp(n, "Enemy_", 6) || !strncmp(n, "Coop", 4) || !strncmp(n, "Player", 6)) return CW_NONE;
    if (!strncmp(n, "Obj_Dungeon_SmallKey", 20)) return CW_KEY;
    if ((!strncmp(n, "Obj_DoorChainLock_", 18) || !strncmp(n, "Obj_SpiritTemple_DoorChainLock_", 31))) return CW_LOCK;
    if (!strcmp(n, "Obj_Token_Gold")) return CW_TOKEN;
    if (!strncmp(n, "Obj_FireTemple_HammerSlipPillar_", 32) || !strncmp(n, "Obj_HammerSwitch_", 17)) return CW_FALLER;
    if ((co_is(n, "MovableBox_") && !co_is(n, "BasePosAnchor")) || !strncmp(n, "Obj_SliderBox_", 14) || !strncmp(n, "Obj_AmosStatue_", 15))
        return CW_BLOCK;
    if (!strncmp(n, "Obj_FireTemple_HammerDropFloor_", 31) || !strncmp(n, "Obj_FireTemple_HammerDropPillar_", 32) ||
        !strncmp(n, "Obj_FireTemple_HammerStair_", 27) || !strncmp(n, "Obj_InsideTheDekuTree_FallingLadder_A_01", 40) ||
        !strncmp(n, "Obj_InsideTheDekuTree_FallingFloor_A_0", 38) || !strncmp(n, "Obj_ShadowTemple_FallingFloor_A_03", 34))
        return CW_SHIFTER;
    if (!strncmp(n, "Obj_Deku_MoveFloor_A_0", 22)) return CW_FLOOR;
    if (!strncmp(n, "Obj_", 4) && co_is(n, "RollingRock_")) return CW_ROLLER;
    if (!strncmp(n, "Obj_RedIce_", 11) || !strcmp(n, "Obj_InsideTheDekuTree_SpiderWeb_A_01") || co_is(n, "SpiderWeb_A_01")) return CW_NOLIFE;
    if (!strncmp(n, "Obj_Plant_Grass_", 16) || !strcmp(n, "Land_kokiri_Plant_A_02_DropFixed_03") || !strcmp(n, "Land_Kokiri_Plant_G_01") ||
        !strcmp(n, "Land_Kokiri_Zenmai_A_02") || !strcmp(n, "Obj_Plant_DestructibleGrass_Base")) return CW_GRASS;
    if (!strcmp(n, "Obj_TBox_A_04") || !strcmp(n, "Obj_Well_CandleStick_A_03")) return CW_NONE;
    if (!strncmp(n, "Obj_Pot_", 8) || co_is(n, "SkullPot_") || co_is(n, "HorsebackArcheryPot_")) return *carry = 1, CW_BREAK;
    if (!strncmp(n, "Obj_LiftRock_", 13)) return *carry = 1, CW_BREAK;
    if (!strncmp(n, "Obj_BrokenBox_", 14) || (!strncmp(n, "Obj_", 4) && co_is(n, "BrokenRock_")) || co_is(n, "CrackWall_") ||
        co_is(n, "CrackFloor_") || !strncmp(n, "Obj_BrokenIcePillar_", 20) || co_is(n, "_Beehive_") || co_is(n, "SpiderWeb_"))
        return CW_BREAK;
    return CW_NONE;
}

static uint64_t co_life_holder(uint64_t actor) {        /* (the Life component's value: [[[actor+0x270]+0x2b8]+0x1c10]+8) */
    const uint64_t parts = co_ok(actor + 0x270, 8) ? G64(actor + 0x270) : 0;
    const uint64_t life = co_ok(parts + 0x2b8, 8) ? G64(parts + 0x2b8) : 0;
    const uint64_t holder = co_ok(life + 0x1c10, 8) ? G64(life + 0x1c10) : 0;
    return co_ok(holder, 16) ? holder : 0;
}

/* The game's own "this actor goes": f_800c6490(actor, why). It does nothing to one already on its way out (state 7..11);
   most of its 113 callers pass why 0. */
static void co_world_delete(Ctx *c, uint64_t actor) {
    A64Fn del = a64_lookup(0x800c6490ULL);
    if (!del || !co_ok(actor, 0x540)) return;
    const uint32_t state = G32(actor + 0x2c);
    if (state >= 7 && state <= 11) return;
    const Ctx saved = *c;
    c->x[0] = actor, c->x[1] = 0;
    del(c);
    *c = saved;
}

static float co_dist2(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static CoBreak *co_wb_find(const char *map, uint64_t hash) {
    for (int i = 0; i < co_wb_n; i++) if (co_wb[i].hash == hash && !strcmp(co_wb[i].map, map)) return &co_wb[i];
    return NULL;
}
static void co_wb_keep(const char *map, uint64_t hash, const float at[3], int flags) {
    CoBreak *b = co_wb_find(map, hash);
    if (flags & 1) {                                    /* (back again: forgotten) */
        if (b) *b = co_wb[--co_wb_n];
        return;
    }
    if (!b) {
        if (co_wb_n == CO_WB_N) memmove(co_wb, co_wb + 1, sizeof co_wb[0] * (CO_WB_N - 1)), co_wb_n--;
        b = &co_wb[co_wb_n++];
    }
    snprintf(b->map, sizeof b->map, "%s", map), b->hash = hash, memcpy(b->at, at, sizeof b->at), b->flags = flags;
}
static CoPlace *co_wp_find(const char *map, uint64_t hash, int make) {
    for (int i = 0; i < co_wp_n; i++) if (co_wp[i].hash == hash && !strcmp(co_wp[i].map, map)) return &co_wp[i];
    if (!make) return NULL;
    if (co_wp_n == CO_WP_N) memmove(co_wp, co_wp + 1, sizeof co_wp[0] * (CO_WP_N - 1)), co_wp_n--;
    CoPlace *p = &co_wp[co_wp_n++];
    memset(p, 0, sizeof *p), snprintf(p->map, sizeof p->map, "%s", map), p->hash = hash;
    return p;
}

/* (the net's lines, on the game's side: co_events, under co.game as co_tick is) */
static void co_world_take(int from, const char *text) {
    char map[64] = "", packed[96] = "";
    unsigned long long hash = 0;
    if (text[1] == 'b') {
        float at[3]; int flags = 0;
        if (sscanf(text + 3, "%63s %llx %f %f %f %d", map, &hash, &at[0], &at[1], &at[2], &flags) != 6 || !hash || !~hash) return;
        co_wb_keep(map, hash, at, flags);
        for (int i = 0; i < CO_WORLD_N; i++)            /* (one of mine: done again for this copy) */
            if (co_things[i].actor && co_things[i].hash == hash) co_things[i].applied = 0;
        hos_log("coop: world: %s's %016llx in %s %s", from ? "a player" : "the host", hash, map, flags & 1 ? "is back: forgotten" : "was broken or taken");
    } else if (text[1] == 'm' || text[1] == 'p') {
        float m[12];
        if (sscanf(text + 3, "%63s %llx %95s", map, &hash, packed) != 3 || !hash || !~hash) return;
        if (co_unbase64(packed, (uint8_t *)m, sizeof m) != sizeof m) return;
        for (int i = 0; i < 12; i++) if (!(m[i] > -1e6f && m[i] < 1e6f)) return;
        CoPlace *p = co_wp_find(map, hash, 1);
        memcpy(p->m, m, sizeof m), p->heard = hos_now_ns(), p->moving = text[1] == 'm', p->from = from;
    } else if (text[1] == 'g') {
        uint8_t raw[0x3c];
        if (sscanf(text + 3, "%63s %95s", map, packed) != 2 || co_unbase64(packed, raw, sizeof raw) != sizeof raw) return;
        float r; memcpy(&r, raw + 0xc, 4);
        if (!(r > 0.01f && r <= 100.0f)) return;
        if (co_wg_n == CO_WG_N) memmove(co_wg, co_wg + 1, sizeof co_wg[0] * (CO_WG_N - 1)), co_wg_n--;
        CoCut *g = &co_wg[co_wg_n++];
        snprintf(g->map, sizeof g->map, "%s", map), memcpy(g->raw, raw, sizeof raw), g->made = 0, g->mine = 0;
    }
}

static void co_world_say_place(const char *kind, uint64_t hash, uint64_t actor) {
    char packed[80];
    co_base64((const uint8_t *)GP(actor + CO_ACTOR_PLACE), 48, packed);
    co_send("%s %s %016llx %s", kind, co_map, (unsigned long long)hash, packed);
}

/* ---- grass in the field: not actors (the foliage module), a cut is a 0x3c-byte slot on a queue ----
   Module at [0x84e42540]; with its byte +0x30 bit 0 clear, [[module+0xf0]+0x338] is the foliage scene: +0x20 the queue's
   room, +0x28 its entries (16 bytes: the slot, a number), +0x30 read and +0x38 write counts. A slot: place, radius
   (+0xc), 0.3, flags (+0x14), three sword points (+0x18 on, NaN when only a sphere). A cut of another player's is
   made here by the game's own call f_80ba0800(module, request), the request a whole 0x3c-byte slot. */
static uint64_t co_foliage(uint64_t *scene) {
    const uint64_t module = co_ok(0x84e42540ULL, 8) ? G64(0x84e42540ULL) : 0;
    if (!co_ok(module, 0x100) || (*(const uint8_t *)GP(module + 0x30) & 1)) return 0;
    const uint64_t holder = G64(module + 0xf0);
    *scene = co_ok(holder + 0x338, 8) ? G64(holder + 0x338) : 0;
    return co_ok(*scene, 0x40) ? module : 0;
}

static void co_grass_step(Ctx *c, int64_t now) {
    static uint64_t last_scene, block;
    static uint64_t next_read;
    uint64_t scene = 0;
    const uint64_t module = co_foliage(&scene);
    if (!module) return;
    const uint32_t room = G32(scene + 0x20);
    const uint64_t entries = G64(scene + 0x28), written = G64(scene + 0x38);
    if (!room || room > 1024 || !co_ok(entries, room * 16ULL)) return;
    if (scene != last_scene) last_scene = scene, next_read = written;   /* (another field: what is queued now is not new) */
    if (written - next_read > room) next_read = written - room;         /* (fell behind: the oldest are gone) */
    for (; next_read < written; next_read++) {                          /* mine: said, unless it is one of theirs made here */
        const uint64_t slot = G64(entries + (next_read % room) * 16);
        if (!co_ok(slot, 0x3c)) continue;
        uint8_t raw[0x3c];
        memcpy(raw, GP(slot), sizeof raw);
        float r; memcpy(&r, raw + 0xc, 4);
        if (!(r > 0.01f && r <= 100.0f)) continue;
        int theirs = 0;
        for (int i = 0; i < co_wg_n && !theirs; i++)
            theirs = co_wg[i].made && now - co_wg[i].made < 3000000000LL && !memcmp(co_wg[i].raw, raw, 0x10);
        if (theirs) continue;
        if (co_wg_n == CO_WG_N) memmove(co_wg, co_wg + 1, sizeof co_wg[0] * (CO_WG_N - 1)), co_wg_n--;
        CoCut *g = &co_wg[co_wg_n++];
        snprintf(g->map, sizeof g->map, "%s", co_map), memcpy(g->raw, raw, sizeof raw), g->made = now, g->mine = 1;
        if (co.links > 0) {
            char packed[96];
            co_base64(raw, sizeof raw, packed);
            co_send("wg %s %s", co_map, packed);
        }
    }
    /* theirs, for this map: made here (at most 8 a frame; a joiner's after its field has settled two seconds) */
    if (now - co_world_map_since < 2000000000LL) return;
    A64Fn cut = a64_lookup(0x80ba0800ULL);
    if (!cut) return;
    if (!block) block = hos_sys_alloc(0x40, 16);
    if (!block) return;
    int made = 0;
    for (int i = 0; i < co_wg_n && made < 8; i++) {
        CoCut *g = &co_wg[i];
        if (g->made || g->mine || strcmp(g->map, co_map)) continue;
        memset(A64_PTR(block), 0, 0x40), memcpy(A64_PTR(block), g->raw, sizeof g->raw);
        const Ctx saved = *c;
        c->x[0] = module, c->x[1] = block;
        cut(c);
        *c = saved;
        g->made = now, made++;
    }
    if (made) hos_log("coop: grass: %d cuts of the others made here", made);
}

/* ---- blocks: where a follower's game remembers the last rest (their issue 147) ----
   A pushed block's AI node (ExecuteMovableBoxControl: vtables 0x849ce6f8 and, at +0x20, 0x849ce8b8; its actor at
   [[[node+8]+8]+0x1f0]) keeps where the push starts at +0x108 and its target at +0xfc. A follower's block was moved by
   the setter, so both still said the old rest and that player's next push snapped it back. Found from the block's
   components (breadth first, three steps, at most 2048 objects), written only when both still hold the old rest. Its
   base anchor (Obj_MovableBox_BasePosAnchor near the old rest) is moved along too. */
static uint64_t co_block_node(uint64_t actor) {
    static uint64_t seen[2048], queue[2048];
    int n_seen = 0, head = 0, tail = 0;
    const uint64_t table = co_ok(actor + 0x270, 8) ? G64(actor + 0x270) : 0;
    if (!co_ok(table, 0x300)) return 0;
    for (uint64_t off = 0; off < 0x300 && tail < 2048; off += 8) {
        const uint64_t p = G64(table + off);
        if (co_ok(p, 0x200)) queue[tail++] = p;
    }
    for (int depth = 0; depth < 3 && head < tail; depth++) {
        const int level_end = tail;
        for (; head < level_end; head++) {
            const uint64_t o = queue[head];
            int dup = 0;
            for (int i = 0; i < n_seen && !dup; i++) dup = seen[i] == o;
            if (dup || n_seen >= 2048) continue;
            seen[n_seen++] = o;
            if (G64(o) == 0x849ce6f8ULL && co_ok(o + 0x20, 8) && G64(o + 0x20) == 0x849ce8b8ULL) {
                const uint64_t a = co_ok(o + 8, 8) ? G64(o + 8) : 0, b = co_ok(a + 8, 8) ? G64(a + 8) : 0;
                if (co_ok(b + 0x1f0, 8) && G64(b + 0x1f0) == actor) return o;
            }
            for (uint64_t off = 0; off < 0x200 && tail < 2048; off += 8) {
                const uint64_t p = G64(o + off);
                if (p >= 0x1000000000ULL && p < 0x10000000000ULL && co_ok(p, 0x200)) queue[tail++] = p;
            }
        }
    }
    return 0;
}

static int co_near3(const float *a, const float b[3]) {
    return fabsf(a[0] - b[0]) < 0.02f && fabsf(a[1] - b[1]) < 0.02f && fabsf(a[2] - b[2]) < 0.02f;
}

static void co_block_fix(Ctx *c, CoThing *t) {
    const float *now_at = t->at;
    if (co_dist2(t->old_rest, now_at) < 0.0025f) return;   /* (moved under 5 cm) */
    const uint64_t node = co_block_node(t->actor);
    if (node && co_ok(node + 0x110, 4)) {
        float *target = (float *)GP(node + 0xfc), *start = (float *)GP(node + 0x108);
        if (co_near3(target, t->old_rest) && co_near3(start, t->old_rest)) {
            memcpy(target, now_at, 12), memcpy(start, now_at, 12);
            HOS_WRITTEN(target, 24);
            hos_log("coop: world: %016llx %s push node: its rest moved to where another player left it", (unsigned long long)t->hash, t->name);
        } else hos_log("coop: world: %016llx %s push node busy (mid-push here): not written", (unsigned long long)t->hash, t->name);
    } else hos_log("coop: world: %016llx %s push node not found: its next push may start from the old place", (unsigned long long)t->hash, t->name);
    for (int i = 0; i < CO_WORLD_N; i++) {                 /* (its base anchor) */
        CoThing *a = &co_things[i];
        if (!a->actor || strcmp(a->name, "Obj_MovableBox_BasePosAnchor") || co_dist2(a->at, t->old_rest) > 0.25f) continue;
        float m[12];
        memcpy(m, GP(a->actor + CO_ACTOR_PLACE), sizeof m), memcpy(m, now_at, 12);
        co_actor_move(c, a->actor, m);
        break;
    }
}

/* ---- the walk, and what is done with it ---- */
static void co_world_step(Ctx *c, int64_t now) {
    static int64_t next_walk;
    static char last_map[64];
    if (strcmp(last_map, co_map)) {                     /* (another map) */
        snprintf(last_map, sizeof last_map, "%s", co_map);
        co_world_map_since = now;
        memset(co_things, 0, sizeof co_things);
        int others = 0;                                  /* (nobody else here: what the game puts back by itself is forgotten) */
        EnterCriticalSection(&co.lock);
        for (int i = 0; i < CO_PEERS; i++) others |= co.peer[i].used && !strcmp(co.peer[i].map, co_map) && now - co.peer[i].seen < 3000000000LL;
        LeaveCriticalSection(&co.lock);
        if (!others) {
            int k = 0;
            for (int i = 0; i < co_wb_n; i++) if (strcmp(co_wb[i].map, co_map) || (co_wb[i].flags & 2)) co_wb[k++] = co_wb[i];
            co_wb_n = k, k = 0;
            for (int i = 0; i < co_wp_n; i++) if (strcmp(co_wp[i].map, co_map)) co_wp[k++] = co_wp[i];
            co_wp_n = k, k = 0;
            for (int i = 0; i < co_wg_n; i++) if (strcmp(co_wg[i].map, co_map)) co_wg[k++] = co_wg[i];
            co_wg_n = k;
        }
    }
    if (co_world_resay && co.links > 0) {              /* (somebody joined: everything known, said again) */
        co_world_resay = 0;
        for (int i = 0; i < co_wb_n; i++)
            co_send("wb %s %016llx %.2f %.2f %.2f %d", co_wb[i].map, (unsigned long long)co_wb[i].hash, co_wb[i].at[0], co_wb[i].at[1], co_wb[i].at[2], co_wb[i].flags);
        for (int i = 0; i < co_wp_n; i++) {
            char packed[80];
            co_base64((const uint8_t *)co_wp[i].m, 48, packed);
            co_send("wp %s %016llx %s", co_wp[i].map, (unsigned long long)co_wp[i].hash, packed);
        }
        for (int i = 0; i < co_wg_n; i++) {
            char packed[96];
            co_base64(co_wg[i].raw, 0x3c, packed);
            co_send("wg %s %s", co_wg[i].map, packed);
        }
    }
    co_grass_step(c, now);
    if (now < next_walk) goto apply;
    next_walk = now + 100000000LL;                      /* (ten walks a second) */
    {
        const uint64_t root = co_ok(0x84eb1888ULL, 8) ? G64(0x84eb1888ULL) : 0;
        const uint64_t mid = co_ok(root + 0x118, 8) ? G64(root + 0x118) : 0;
        const uint64_t head = mid + 0x670;
        if (!mid || !co_ok(head, 0x18)) goto apply;
        const uint32_t count = G32(head + 0x10);
        if (!count || count > 8192) goto apply;
        float mine[3];
        EnterCriticalSection(&co.lock);
        memcpy(mine, co.pos, sizeof mine);
        LeaveCriticalSection(&co.lock);
        static uint8_t seen_now[CO_WORLD_N];
        memset(seen_now, 0, sizeof seen_now);
        uint32_t walked = 0;
        for (uint64_t node = G64(head); node != head && walked <= count + 1; walked++) {
            if (!co_ok(node, 0x18)) { walked = ~0u; break; }
            const uint64_t here = node, actor = G64(node + 0x10);
            node = G64(node);
            if (!co_ok(actor, 0x540) || actor + 0x538 != here) continue;   /* (not an actor: a few other objects share the ring) */
            const uint64_t pl = co_ok(actor + 0x3d8, 8) ? G64(actor + 0x3d8) : 0;
            const uint64_t hash = co_ok(pl + 0x88, 8) ? G64(pl + 0x88) : 0;
            const uint64_t name_at = co_ok(actor + 0x20, 8) ? G64(actor + 0x20) : 0;
            if (!co_ok(name_at, 48)) continue;
            char name[48];
            snprintf(name, sizeof name, "%.47s", (const char *)GP(name_at));
            uint8_t carry;
            const int kind = co_world_kind(name, &carry);
            const int anchor = !strcmp(name, "Obj_MovableBox_BasePosAnchor");
            if (kind == CW_NONE && !anchor) continue;
            if (kind != CW_TOKEN && !anchor && (!hash || !~hash)) continue;   /* (one the game made: not shared) */
            int slot = -1, free_slot = -1;
            for (int i = 0; i < CO_WORLD_N; i++) {
                if (co_things[i].actor == actor) { slot = i; break; }
                if (!co_things[i].actor && free_slot < 0) free_slot = i;
            }
            if (slot < 0) {
                if (free_slot < 0) continue;
                CoThing *t = &co_things[free_slot];
                memset(t, 0, sizeof *t);
                t->actor = actor, t->hash = hash, t->kind = (uint8_t)(anchor ? CW_NONE : kind), t->carry = carry;
                snprintf(t->name, sizeof t->name, "%s", name);
                memcpy(t->first, GP(actor + CO_ACTOR_PLACE), 12), memcpy(t->rest, t->first, 12), memcpy(t->old_rest, t->first, 12);
                t->first_seen = now, t->still_since = now;
                if (kind == CW_TOKEN) {                     /* (a token has no placement: it is its Gold Skulltula's) */
                    float best = 9.0f;
                    AcquireSRWLockShared(&co_lives_lock);
                    for (int i = 0; i < CO_LIVES; i++) {
                        if (!co_lives[i].life) continue;
                        const float d = co_dist2(co_lives[i].born, t->first);
                        if (d < best) best = d, t->hash = 0x70000000000000ULL ^ co_lives[i].hash ^ ((uint64_t)(uint32_t)(int)(co_lives[i].born[0] * 8) << 32);
                    }
                    ReleaseSRWLockShared(&co_lives_lock);
                    if (!t->hash) { t->actor = 0; continue; }
                }
                slot = free_slot;
            }
            CoThing *t = &co_things[slot];
            seen_now[slot] = 1, t->missed = 0;
            memcpy(t->at, GP(actor + CO_ACTOR_PLACE), 48);
            if (!t->last_at[0] && !t->last_at[1] && !t->last_at[2]) memcpy(t->last_at, t->at, 12);
            if (t->kind == CW_NONE) continue;
            /* broken here: its life ran out */
            const uint64_t holder = co_life_holder(actor);
            const int has_life = holder && G32(holder + 8) < 100000;
            if (has_life && (int32_t)G32(holder + 8) <= 0 && !t->broken_here && t->kind != CW_FALLER) {
                t->broken_here = 1;
                const int flags = (t->kind == CW_KEY || t->kind == CW_LOCK || t->kind == CW_TOKEN) ? 2 : 0;
                if (!co_wb_find(co_map, t->hash)) {
                    co_wb_keep(co_map, t->hash, t->at, flags);
                    co_send("wb %s %016llx %.2f %.2f %.2f %d", co_map, (unsigned long long)t->hash, t->at[0], t->at[1], t->at[2], flags);
                    hos_log("coop: world: %016llx %s broken here (life 0): sent", (unsigned long long)t->hash, t->name);
                }
            }
            if (t->kind == CW_FALLER && has_life && (int32_t)G32(holder + 8) <= 0 && !t->broken_here) {
                t->broken_here = 1;
                co_wb_keep(co_map, t->hash, t->at, 2);
                co_send("wb %s %016llx %.2f %.2f %.2f 2", co_map, (unsigned long long)t->hash, t->at[0], t->at[1], t->at[2]);
            }
            /* moved here: I am its mover (my Link near, nobody else moving it) */
            const int mover_kind = t->kind == CW_BLOCK || t->kind == CW_SHIFTER || t->kind == CW_FLOOR || t->kind == CW_ROLLER || t->carry;
            if (mover_kind && !t->broken_here) {
                const float far2 = (t->kind == CW_SHIFTER || t->kind == CW_FLOOR) ? 1600.0f : 225.0f;
                const float step = t->kind == CW_BLOCK ? 0.0025f : t->kind == CW_FLOOR ? 0.25f : 0.0225f;
                CoPlace *p = co_wp_find(co_map, t->hash, 0);
                const int followed = p && p->from >= 0 && now - p->heard < 500000000LL;
                const int settling = now < t->settle_until || now < t->hold_until;
                if (co_dist2(t->at, t->last_at) > 0.0001f) t->still_since = now;   /* (moved more than a centimetre since the last walk) */
                memcpy(t->last_at, t->at, 12);
                if (co_dist2(t->at, t->rest) > step && co_dist2(mine, t->at) < far2 && !followed && !settling) {
                    if (now - t->sent_at > 50000000LL && now - t->still_since < 400000000LL) {
                        t->sent_at = now;
                        co_world_say_place("wm", t->hash, actor);
                    }
                    t->gone_said = 2;                   /* (2: I moved it; the rest place is said when it stops) */
                }
                if (t->gone_said == 2 && now - t->still_since > 400000000LL) {
                    t->gone_said = 0;
                    memcpy(t->rest, t->at, 12);
                    co_world_say_place("wp", t->hash, actor);
                    CoPlace *q = co_wp_find(co_map, t->hash, 1);
                    memcpy(q->m, t->at, 48), q->heard = now, q->moving = 0, q->from = -1;
                    hos_log("coop: world: %016llx %s at rest: place sent", (unsigned long long)t->hash, t->name);
                }
            }
        }
        if (walked != ~0u && walked + 2 >= count && walked <= count + 2) {   /* (a whole walk: who is gone) */
            int gone_n = 0, far_gone = 0;
            for (int i = 0; i < CO_WORLD_N; i++) {
                CoThing *t = &co_things[i];
                if (!t->actor || seen_now[i]) continue;
                if (++t->missed < 2) continue;
                if (!t->gone_at) {
                    t->gone_at = now;
                    gone_n++;
                    if (co_dist2(mine, t->at) > 64.0f) far_gone = 1;
                }
            }
            for (int i = 0; i < CO_WORLD_N; i++) {     /* (gone next to my Link, held 0.7 s: an unloaded area is many at once) */
                CoThing *t = &co_things[i];
                if (!t->actor || !t->gone_at || seen_now[i]) continue;
                if (gone_n >= 4 && far_gone) { t->actor = 0; continue; }
                if (now - t->gone_at < 700000000LL) continue;
                const int moved = co_dist2(t->at, t->first) > 0.25f;
                const float reach2 = (moved || t->kind == CW_KEY || t->kind == CW_TOKEN || t->kind == CW_LOCK) ? 64.0f : 9.0f;
                const int breakable = t->kind == CW_BREAK || t->kind == CW_GRASS || t->kind == CW_NOLIFE || t->kind == CW_KEY ||
                                      t->kind == CW_LOCK || t->kind == CW_TOKEN;
                if (breakable && !t->broken_here && co_dist2(mine, t->at) < reach2 && !co_wb_find(co_map, t->hash)) {
                    const int flags = (t->kind == CW_KEY || t->kind == CW_LOCK || t->kind == CW_TOKEN) ? 2 : 0;
                    co_wb_keep(co_map, t->hash, t->at, flags);
                    co_send("wb %s %016llx %.2f %.2f %.2f %d", co_map, (unsigned long long)t->hash, t->at[0], t->at[1], t->at[2], flags);
                    hos_log("coop: world: %016llx %s gone next to Link: broken or taken here, sent", (unsigned long long)t->hash, t->name);
                }
                t->actor = 0;                           /* (forgotten: met again, it is a new one) */
            }
        }
    }
apply:
    /* ---- the others': broken, taken, moved ---- */
    {
        float mine[3];
        EnterCriticalSection(&co.lock);
        memcpy(mine, co.pos, sizeof mine);
        LeaveCriticalSection(&co.lock);
        for (int i = 0; i < CO_WORLD_N; i++) {
            CoThing *t = &co_things[i];
            if (!t->actor || t->kind == CW_NONE || !co_ok(t->actor, 0x540)) continue;
            const CoBreak *b = co_wb_find(co_map, t->hash);
            if (b && !t->broken_here) {
                const int at_once = t->kind == CW_KEY || t->kind == CW_TOKEN || now - t->first_seen < 300000000LL;   /* (taken; or met after its break) */
                if (t->kind == CW_FALLER) {                /* (a hammer pillar, a hammer switch: life 0 here too, once a second, at most 10 times) */
                    const uint64_t holder = co_life_holder(t->actor);
                    if (holder && (int32_t)G32(holder + 8) > 0 && now >= t->faller_next && t->faller_tries < 10) {
                        const uint64_t parts = G64(t->actor + 0x270), life = G64(parts + 0x2b8);
                        co_enemy_damage(c, life, 100000);
                        t->faller_next = now + 1000000000LL, t->faller_tries++;
                    }
                    continue;
                }
                if (t->kind == CW_LOCK && co_dist2(mine, t->at) < 6.25f) continue;   /* (never while my Link is at it) */
                if (!t->applied) {
                    t->applied = 1;
                    const uint64_t holder = co_life_holder(t->actor);
                    if (!at_once && holder && (int32_t)G32(holder + 8) > 0 && t->kind != CW_NOLIFE) {
                        const uint64_t parts = G64(t->actor + 0x270), life = G64(parts + 0x2b8);
                        co_enemy_damage(c, life, 100000);   /* (the game's own break: its sound, its pieces) */
                        t->delete_at = now + (t->kind == CW_GRASS ? 400000000LL : 6000000000LL);
                    } else t->delete_at = at_once ? now : now + 400000000LL;
                    hos_log("coop: world: %016llx %s %s", (unsigned long long)t->hash, t->name,
                            at_once ? "was broken or taken before we saw it: deleted" : "broken by another player");
                }
                if (t->delete_at && now >= t->delete_at) {
                    const uint64_t holder = co_life_holder(t->actor);
                    const int standing = !holder || (int32_t)G32(holder + 8) > 0 || t->kind == CW_GRASS;
                    if (standing || t->kind == CW_KEY || t->kind == CW_LOCK || t->kind == CW_TOKEN || t->kind == CW_NOLIFE) {
                        co_world_delete(c, t->actor);
                        hos_log("coop: world: %016llx %s still stood after the break: deleted", (unsigned long long)t->hash, t->name);
                    }
                    t->delete_at = 0;
                }
                continue;
            }
            /* moved by another player: follow where they say, then hold it at rest a moment */
            CoPlace *p = co_wp_find(co_map, t->hash, 0);
            if (!p || p->from < 0) continue;
            if (t->gone_said == 2 && (int)co.my_id < p->from) continue;   /* (both moving it: the lower number keeps it) */
            if (p->moving && now - p->heard < 500000000LL) {
                if (!t->follow) memcpy(t->old_rest, t->rest, 12);
                t->follow = 1;
                co_actor_move(c, t->actor, p->m);
                memcpy(t->rest, p->m, 12), t->settle_until = now + 1500000000LL;
            } else if (!p->moving && (t->follow || now - t->first_seen < 300000000LL || co_dist2(t->at, p->m) > 0.0025f) && !t->hold_until) {
                if (!t->follow) memcpy(t->old_rest, t->rest, 12);
                t->follow = 0;
                t->hold_until = now + 500000000LL;
                co_actor_move(c, t->actor, p->m);
                memcpy(t->rest, p->m, 12), memcpy(t->at, p->m, 48);
                hos_log("coop: world: %016llx %s put where another player left it", (unsigned long long)t->hash, t->name);
                if (t->kind == CW_BLOCK && strncmp(t->name, "Obj_AmosStatue_", 15)) co_block_fix(c, t);
            } else if (t->hold_until && now < t->hold_until) {
                co_actor_move(c, t->actor, p->m);
            } else if (t->hold_until && now >= t->hold_until) {
                t->hold_until = 0, t->settle_until = now + 1500000000LL;
                p->from = -1;                           /* (let go: what moves it now is this game's own) */
            }
        }
    }
}

void oot_coop_frame(void) {
    static int64_t next_slow;
    static int shown;
    {   /* (co-op's corner and the names are not drawn over the game's own menus: the pause menu, the map. Not at the title,
           where the corner says whether the link is up before a save is loaded) */
        int hos_oot_menu_in_front(void);               /* host/oot_menu.c */
        static int hidden = -1;
        const int hide = co_playing && hos_oot_menu_in_front();
        if (hide != hidden) hidden = hide, rx_option_set(HOS_OPT_COOP_HIDE, hide);
    }
    const int mode = rx_option(HOS_OPT_COOP_MODE, 0);
    /* (From the game's first frame, connected or not: the name and the address are read, shown in their rows and can be
       typed before Connect is ever pressed. The port is set before the link's thread can start, and what is wanted of
       the thread after: it never sees one without the other.) */
    { const int port = rx_option(HOS_OPT_COOP_PORT, 43400); InterlockedExchange(&co.port, port >= 1024 && port <= 65535 ? port : 43400); }
    co_start(mode == 1 || mode == 2);
    InterlockedExchange(&co.want, mode == 1 || mode == 2 ? mode : 0);
    if (!shown) shown = 1, co_menu_text(2, co.name), co_menu_text(3, "Set (kept off the screen)");   /* (an address is not for a stream) */
    const int action = rx_option(HOS_OPT_COOP_ACTION, 0);
    if (action == 3 || action == 4) co_typed(action - 2);
    else if (action >= 10 && action < 10 + CO_PEERS) co_goto_ask(action - 10);
    const int64_t now = hos_now_ns();
    EnterCriticalSection(&co.game);
    co_events();
    if (now >= next_slow) {
        next_slow = now + 1000000000LL;
        const int playing = co_playing && co.player_seen && now - co.player_seen < 1000000000LL;
        co_status(NULL, now, co_map, playing);
    }
    LeaveCriticalSection(&co.game);
}

/* (oot_coop_player: Link's own update, every frame of play, on whichever of the game's workers runs it.
   The game's "is an event playing?" question is asked only now and then.) At most 100 times a second, one caller at a time. */
static void co_tick(Ctx *c, uint64_t link) {
    static int64_t next_map, next_place, next_flags, playing_since, last_playing;
    static volatile int64_t last_tick;
    static int fresh;                                  /* a save was just loaded: its counts are where it starts */
    static int64_t quiet_until;                        /* nothing is changed in the game before this: an event or a load, and two seconds after */
    if (!co.ready || !co.want) return;
    const int64_t now = hos_now_ns();
    if (now - last_tick < 10000000LL || !TryEnterCriticalSection(&co.game)) return;
    last_tick = now;
    if (!co_shared_loaded) co_shared_load();

    /* in the game: Link was updated a moment ago, and for three seconds on end (a save just loaded settles first) */
    const int playing_now = co.player_seen && now - co.player_seen < 1000000000LL;
    if (playing_now && (!last_playing || now - last_playing > 3000000000LL)) {
        playing_since = now;
        co_theirs_done = co_ktheirs_done = 0;          /* this may be another save: the others' flags and items, asked of it again */
        for (int i = 0; i < CO_ITEMS && i < 256; i++) co_to_check[i] = co_they_hold[i];
        for (int i = 0; i < co_more_n; i++) co_more[i].to_check = co_more[i].they_hold;
        fresh = 1;
    }
    if (playing_now) last_playing = now;
    const int playing = playing_now && now - playing_since > 3000000000LL;
    co_playing = playing;
    {   /* (Link is another actor: another map. Its name is read at once, so no body is made for the map just left) */
        static uint64_t last_link;
        if (link != last_link) last_link = link, next_map = 0;
    }
    {   /* (once a minute while co-op is on: what the game's side sees, for the log) */
        static int64_t next_say;
        if (now >= next_say) {
            next_say = now + 60000000000LL;
            hos_log("coop: tick: Link seen %lld ms ago, playing %d, map '%s', links %ld, state %ld", co.player_seen ? (long long)((now - co.player_seen) / 1000000) : -1LL,
                    playing, co_map, (long)co.links, (long)co.state);
        }
    }
    if (playing && now >= next_map) next_map = now + 1000000000LL, co_current_map(c, co_map, sizeof co_map);
    if (playing && co_map[0]) {
        if (now >= next_place && (co.links > 0 || co_mirror)) {
            next_place = now + 33000000LL;
            float p[3], yaw;
            EnterCriticalSection(&co.lock);
            memcpy(p, co.pos, sizeof p), yaw = co.yaw;
            LeaveCriticalSection(&co.lock);
            const int child = co_my_child(c);   /* (Navi: the map's age first, then the game's own) */
            co_send("P %s %.2f %.2f %.2f %.3f %d %d %d %s %u", co_map, p[0], p[1], p[2], yaw, child, co_game_busy(), rx_option(HOS_OPT_COOP_PVP, 0) > 0,
                    co.look[0] ? co.look : "-", co_my_drawn(link));   /* (Navi: how I look, then what is in my hands) */
            co_anim_say(link);
            co_hearts_say(c, link, now);   /* (Navi) */
            co_gear_say(link);
            co_pose_learn(link, child);
        }
        { const int busy = co_game_busy(); InterlockedExchange(&co_busy_now, busy); if (busy) quiet_until = now + 2000000000LL; }
        const int quiet = now < quiet_until;
        if (now >= next_flags && !quiet) {
            next_flags = now + 500000000LL;
            const int levelling = co_level && co.links > 0;
            if (levelling) co_level = 0;
            co_flags_step(levelling);
            co_keys_step(levelling);
            co_counts_step(fresh, levelling);
            fresh = 0;
            if (levelling) co_say_items(c);
        }
        if (!quiet) co_items_step(c, now);
        if (!quiet) co_trade_step(c, now);
        co_bodies_step(c, link, now);
        co_pvp_step(c, link, now);
        co_collide_step(c, link, now);   /* (Navi) */
        { static int world = -1;                    /* (Navi) the world's objects: HOS_COOP_WORLD=0 leaves them to each game */
          if (world < 0) { const char *e = getenv("HOS_COOP_WORLD"); world = !(e && !strcmp(e, "0")); hos_log("coop: world: placed objects shared: %s", world ? "on" : "off (HOS_COOP_WORLD=0)"); }
          if (world && !quiet && co.links > 0) co_world_step(c, now); }
        InterlockedExchange(&co_test_play, 0);
        InterlockedExchange(&co_test_equip, 0);
        { static int test = -1; if (test < 0) { const char *e = getenv("HOS_COOP_TEST"); test = e && !strcmp(e, "1"); } co_as_test(c, link, test); }
        co_goto_step(c, link, now);
    }
    LeaveCriticalSection(&co.game);
}

/* (nx2/oot_patches.py patch_coop_pickup: the head of f_8112d570, where every pickup enters the pouch. x1 points at the
   item's name, w3 is how many.) An item this side is handing over is not a find of mine. */
void oot_coop_pouch_add(Ctx *c) {
    if (!co.ready || co.links <= 0 || co_applying) return;
    const uint64_t ref = c->x[1];
    const int many = (int)(int32_t)c->x[3];
    if (!ref || many < 1) return;
    const uint64_t at = G64(ref);   /* (the game's own argument, read as the game is about to read it) */
    if (!at) return;
    const char *name = (const char *)GP(at);
    const size_t len = strnlen(name, 0x60);
    if (!len || len >= 0x60) return;
    co_pickup(name, many);
}
#else   /* (not Windows: no link yet. The lifted game's hooks and the frame's call still land here) */
void oot_coop_frame(void) {}
void oot_coop_player(Ctx *c, uint64_t actor) { (void)c, (void)actor; }
void oot_coop_pouch_add(Ctx *c) { (void)c; }
void oot_coop_life_seen(Ctx *c, uint64_t life) { (void)c, (void)life; }
void oot_coop_life_write(Ctx *c, uint64_t life, uint32_t was, uint32_t now) { (void)c, (void)life, (void)was, (void)now; }
#endif
