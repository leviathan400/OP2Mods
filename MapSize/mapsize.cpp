// mapsize.cpp  -  MapSize: let Outpost 2 run maps up to 1024 x 1024 tiles
// ======================================================================
// An op2ext module for Outpost 2 OPU 1.4.1 (Outpost2.exe, fixed image base 0x00400000).
// All addresses below are in that Outpost2.exe. Each patch checks the bytes it replaces at
// startup and is left out (with a log line) if they aren't what it expects.
//
// WHY
// ---
// The game loads a map of any size: Map.Load (0x00435DC0) and Map.AllocateSpaceForMap (0x004354F0)
// never check the width or height (the width is a power of two, the height is rounded up to one).
// But several parts of the engine were sized for the largest stock map, 512 x 256:
//
//   1. Light tables: two per-column tables of 512 bytes in the Map object. OPU 1.4.0 already
//      doubled them (its changelog: "allowing a potential map size of 1024x512"), so maps 1024
//      wide light correctly; this module only checks that OPU's change is there.
//   2. Tube / power flood fill. Its "visited" bitmap (0x0054AFB8) is 512 x 256 bits. Columns past
//      511 land in the next row (wrong power and connections); rows past 255 write over the memory
//      after it.
//   3. Pathfinder corridor. The corridor A* (used for RCC owners and the AI) keeps a 256-entry
//      row table on the stack, indexed by the absolute tile row: rows past 255 write over the
//      search's own node pool.
//   4. Blight / lava spread. The tile picker is a 17-bit LFSR, which can only reach 2^17 tiles:
//      on a bigger map most tiles could never be picked.
//   5. Wrap-around. Only maps narrower than 512 have hard edges: the game stores them at twice
//      their width with a 32-tile pad, so the other side is never in reach. A map 512 wide or
//      more wraps (east of the last column is the first). Storing a 1024-wide map at 2048 would
//      break waypoints, the light tables and the 16-bit pixel coordinates in several places.
//   6. Waypoints. A waypoint packs pixel x in 15 bits and pixel y in 14 bits, so it can't point
//      below row 511: on a taller map every order to the lower rows lands somewhere else.
//
// WHAT IT CHANGES (numbered as above; the source below is in the same sections)
// ---------------
//   1. Nothing - OPU's light tables are used; CheckLight() logs whether they are there.
//   2. The visited bitmap moves into this module (g_tubeVisited, 1024 x 1024 bits): the base
//      address in its 14 instructions, the row stride (64 -> 128 bytes: `shl 6` -> `shl 7` for
//      byte offsets, `shl 4` -> `shl 5` for dword offsets) and the clear (0x1000 -> 0x8000 dwords).
//      The fill's results are the same on every map; it just has room.
//   3. The corridor row table is indexed by (row & 0xFF). A corridor covers at most ~50 rows (a
//      move under 24 tiles, or the next 48 steps of a route), so rows never collide; on maps up
//      to 256 tall the index is the row, exactly as before. Four small stubs mask the row where it
//      is used, and the corridor's numbering loop runs over the 256 table rows instead of the
//      map's height.
//   4. On maps of more than 2^17 tiles the LFSR's taps become x^20 + x^17 + 1 (maximal, period
//      2^20 - 1), so every tile can be reached. Maps of 2^17 tiles or fewer keep the stock taps,
//      so blight and lava spread exactly as in the stock game there. The shared game-cycle call
//      (0x0049C4BD, chained like the other op2-patchs modules) checks the map size too, as a
//      fallback if the allocation hook below couldn't be installed.
//   5. Bounded wide maps: a map 512 or more wide whose file sets an x clip rect (x1 >= 0) has
//      hard edges. Most of that is the map's own doing: Map.Load takes the clip rect from the
//      file, vehicles never step outside it (Unit:Vehicle.StartNextTileStep) and the planner
//      keeps to it (PathFinder.PlanCoarsePath); a band of impassable columns outside the clip
//      (make_bigmap.py writes 16) keeps Blight and every distance check apart across the seam.
//      What the game doesn't do is stop the VIEW: IWnd:Pane:Detail.ClampViewPos (0x00407990)
//      wraps X on every map without a pad. The module wraps that function: on a bounded map X is
//      clamped to the clip rect first, then the game's own function runs (it still clamps Y), so
//      the view scrolls to the last column and stops. It must stay the game's own function: other
//      modules (Zoom) change the view size it reads, by recognising calls made from inside it.
//      Routes: the pathfinder measures every east-west distance the short way round (it was
//      written for wrapping maps), so on a bounded map wider than 512 a goal more than half the
//      width away looks like it is the other way, across the seam. PathFinder.PlanPath
//      (0x00440300) is wrapped: for such a move it is given a leg goal LEG_TILES along the true
//      direction instead, and the plan is marked unfinished so the vehicle plans the next leg when
//      it gets there. Moves under LEG_LIMIT columns are planned exactly as before.
//   6. On maps taller than 512, waypoints use a tall layout: pixel y gets 15 bits (bits 15-29) and
//      the "close enough" tolerance keeps 2 bits (30-31, 0-3; the game writes 0 or 3 apart from
//      the last waypoint of an attack, whose range is kept elsewhere). The 52 instructions that
//      pack or unpack the field are switched when the map is made (WP_SITES), so maps up to 512
//      tall keep the stock layout - orders, network packets and saved games exactly as stock.
//
// Per-map settings (4 and 6) are set as each map is allocated (a hook in AllocateSpaceForMap at
// 0x00435728), before the mission or a saved game puts anything on it.
//
// DETERMINISM / MULTIPLAYER: on stock-size maps nothing the simulation computes changes. On a big
// map every player needs the module (as everyone needs the map).
//
// INSTALL (OPU 1.4.1, in OPU\outpost2.ini, with MapSize.dll in the OPU folder)
//   [ExternalModules]
//   MapSize=yes
//   [MapSize]
//   Dll=MapSize.dll
//   Enabled=1
//   Debug=0          1 = log every vehicle every DebugEvery ticks, and run test orders from
//   DebugEvery=200       OPU\logs\MapSize-cmd.txt (see "debug" below)
// Log: OPU\logs\MapSize.log (patches installed, and the size of each map played).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define EXPORT extern "C" __declspec(dllexport)

// ---- game addresses ---------------------------------------------------------------------------
static const uintptr_t ADDR_MapTileWidth  = 0x0054F808;   // Map+0x10 tileWidth (in memory)
static const uintptr_t ADDR_MapTileHeight = 0x0054F810;   // Map+0x18 tileHeight
static const uintptr_t ADDR_MapLightLevel = 0x0054F854;   // Map+0x5C lightLevel[512] in the stock game
static const uintptr_t ADDR_LightRead     = 0x0044BBB3;   // Unit:Vehicle.StartNextTileStep: movsx eax,[eax+lightLevel]
static const uintptr_t ADDR_TubeVisited   = 0x0054AFB8;   // tube flood-fill visited bits, 512 x 256 (replaced)

static const uintptr_t ADDR_ProcessGameCycle = 0x00489EA0;  // TethysGame::ProcessGameCycle(), __thiscall
static const uintptr_t CALLSITE_PGC          = 0x0049C4BD;  // the one `call ProcessGameCycle` (E8 rel32)

// Tube flood fill (TubeFill_* / tube group functions, 0x42ED70-0x430290).
static const uintptr_t TUBE_REFS[] = {     // instructions with 0x0054AFB8 in them
    0x0042ED73,                                         // ResetFloodFillVisited: mov edi, base
    0x0042EDB3, 0x0042EEB3, 0x0042F043, 0x00430183,     // tests: [eax+esi+base]
    0x0042F5CF, 0x0042F5DA, 0x0043026A, 0x00430278,     // marks: [reg+base], read and write
    0x0042F18F, 0x0042F81B, 0x0042FA5F, 0x0042FE5E, 0x00430103,   // dirty-range clears: lea edi,[reg*4+base]
};
static const uintptr_t TUBE_ROW_BYTES[]  = { 0x0042EDA6, 0x0042EEA6, 0x0042F036, 0x00430176 };  // mov cl,6 (row * 64 bytes)
static const uintptr_t TUBE_ROW_DWORDS[] = { 0x0042F588, 0x0043021C };                          // mov cl,4 (row * 16 dwords)
static const uintptr_t ADDR_TubeClear    = 0x0042ED78;                                          // mov ecx,1000h

// Pathfinder corridor (row table = the first 256 dwords of the corridor object).
static const uintptr_t ADDR_InsertInterval  = 0x00440B42;   // Corridor.InsertInterval: mov esi,[esp+0Ch]; push edi
static const uintptr_t ADDR_RefineRow       = 0x00440F1B;   // RefineCoarseSteps: test edi,edi; jge 0x44102B
static const uintptr_t ADDR_SearchRow1      = 0x00441432;   // SearchCorridor: start tile's row
static const uintptr_t ADDR_SearchRow2      = 0x0044168E;   // SearchCorridor: neighbour's row
static const uintptr_t ADDR_SearchLoopFirst = 0x004413BE;   // numbering loop: cmp [eax+C20h],ecx; jle
static const uintptr_t ADDR_SearchLoopNext  = 0x004413FD;   // numbering loop: cmp [eax+C20h],edx; jg

// IWnd:Pane:Detail.ClampViewPos (0x00407990), __thiscall(pane, int viewPos[2]), ret 4.
static const uintptr_t ADDR_ClampViewPos = 0x00407990;
static const uintptr_t ADDR_MapPixelWidthMask = 0x0054F7FC;  // Map+0x04
static const uintptr_t ADDR_MapClipX1 = 0x0054F814;          // Map+0x1C
static const uintptr_t ADDR_MapClipX2 = 0x0054F81C;          // Map+0x24
static const uintptr_t ADDR_MapPadX   = 0x0054F830;          // Map+0x38 paddingOffsetTileX

// PathFinder.PlanPath(Unit*, WaypointBlock* wp), __thiscall, ret 8. wp+0x48/+0x4C = the unit's pixel
// x/y, wp+0x50/+0x54 = the destination tile, wp+0x38 = "the plan reaches the goal".
static const uintptr_t ADDR_PlanPath = 0x00440300;
static const int LEG_LIMIT = 448;   // moves this many columns or more (east-west) are split into legs
static const int LEG_TILES = 384;   // ... of this length (well under half of a 1024-wide map)

// BlightOrLava.Spread (0x0049DCF0): `mov ecx, 12000h` (the LFSR taps).
static const uintptr_t ADDR_SpreadTaps = 0x0049DD0F;
static const DWORD TAPS_STOCK = 0x12000;   // bits 16 and 13: x^17 + x^14 + 1
static const DWORD TAPS_BIG   = 0x90000;   // bits 19 and 16: x^20 + x^17 + 1

// ---- state ----------------------------------------------------------------------------------
typedef void (__thiscall *ProcGameCycle_t)(void* self);
static ProcGameCycle_t g_nextCycle = NULL;    // what the cycle call pointed at before us
static DWORD g_tubeVisited[1024 * 32];        // 1024 rows x 1024 bits (32 dwords a row), replaces 0x0054AFB8
static bool  g_installed;                     // InitMod has run (op2ext calls it once; guard anyway)
static bool  g_tapsOk;                        // the spread-taps instruction checked out and may be switched
static int   g_lastW = -1, g_lastH = -1;      // map size the per-map settings were last set for
static bool  g_edgesLogged = true;            // the map's edges (clip rect) logged since it was made
static char  g_gameDir[MAX_PATH];

// ---- game folder, log ----------------------------------------------------------------
// The OPU folder: the current directory when it holds outpost2.ini (OPU starts the game with the
// OPU folder current - the live ini and logs\ are there), else the exe's own folder.
static const char* GameDir() {
    if (!g_gameDir[0]) {
        char dir[MAX_PATH], ini[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(MAX_PATH, dir);
        if (n > 0 && n < MAX_PATH - 16) {
            if (dir[n - 1] == '\\') dir[n - 1] = '\0';
            snprintf(ini, MAX_PATH, "%s\\outpost2.ini", dir);
            if (GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES) { strcpy(g_gameDir, dir); return g_gameDir; }
        }
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        char* bs = strrchr(dir, '\\'); if (bs) *bs = '\0'; else strcpy(dir, ".");
        strcpy(g_gameDir, dir);
    }
    return g_gameDir;
}

// Append one timestamped line to logs\MapSize.log (opened per line so it survives a crash).
static void Log(const char* fmt, ...) {
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\logs", GameDir());
    CreateDirectoryA(path, NULL);
    snprintf(path, MAX_PATH, "%s\\logs\\MapSize.log", GameDir());
    FILE* f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// ---- patch helpers ------------------------------------------------------------------------------
// Write `n` bytes into the game's code (made writable for the write, then protected again).
// Callers check the bytes they replace first; this only writes.
static void WriteCode(uintptr_t at, const void* with, int n) {
    DWORD op;
    VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &op);
    memcpy((void*)at, with, n);
    VirtualProtect((void*)at, n, op, &op);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
}

// Replace some instruction bytes, but only if they are what is expected.
static bool PatchBytes(uintptr_t at, const BYTE* expect, const BYTE* with, int n, const char* what) {
    if (memcmp((void*)at, expect, n) != 0) { Log("patch: %s - unexpected bytes at 0x%08X - not installed", what, (unsigned)at); return false; }
    WriteCode(at, with, n);
    return true;
}

// Check `n` bytes, then put a JMP to `to` at `at` and NOP the rest.
static bool PatchJmp(uintptr_t at, const BYTE* expect, int n, void* to, const char* what) {
    if (n < 5 || n > 16 || memcmp((void*)at, expect, n) != 0) {
        Log("patch: %s - unexpected bytes at 0x%08X - not installed", what, (unsigned)at); return false;
    }
    BYTE b[16];
    memset(b, 0x90, sizeof(b));
    b[0] = 0xE9;
    *(int*)(b + 1) = (int)((uintptr_t)to - (at + 5));
    WriteCode(at, b, n);
    return true;
}

// Does the instruction at `at` hold the 32-bit value `v` in its bytes 1-4 (a displacement or immediate)?
static int FindDword(uintptr_t at, DWORD v) {
    for (int i = 1; i <= 4; i++) if (*(DWORD*)(at + i) == v) return i;
    return 0;
}

// ---- 1. light tables (OPU's) -----------------------------------------------------------------
// OPU 1.4.0+ moves the light tables to a bigger buffer of its own, so the game's reads no longer
// use 0x0054F854. If they still do, maps over 512 wide will crash as they load - say so.
static void CheckLight() {
    if (FindDword(ADDR_LightRead, ADDR_MapLightLevel))
        Log("light: the stock 512-column light tables are in use (no OPU 1.4 patch?) - maps over 512 wide will crash");
    else
        Log("light: OPU's larger light tables are in use (maps up to 1024 wide)");
}

// ---- 2. tube flood fill --------------------------------------------------------------------------
// The flood fill that finds which buildings a tube network connects (power, connection to the
// Command Center) marks the tiles it has visited in a bitmap, one bit per tile. The game's bitmap
// is 512 x 256: a row is 64 bytes (16 dwords), and the code finds a tile's bit with
//     test sites: byte offset  = tileY << 6  + ((tileX & ~0x18) >> 3)    (`mov cl,6` / `shl esi,cl`)
//     mark sites: dword offset = tileY << 4  + (tileX >> 5)              (`mov cl,4` / `shl edx,cl`)
// plus a "dirty" range of dwords (0x0054AFB0 / 0x0054AFB4) that is cleared after each fill. The
// module's bitmap is 1024 x 1024: a row is 128 bytes (32 dwords), so the two shifts become 7 and 5,
// the full clear covers 0x8000 dwords, and every instruction that names the bitmap names the
// module's. The dirty range is kept in dwords by the same code, so it needs no change.
static void InstallTubes() {
    // Check everything first, so the bitmap is either fully moved or not at all.
    for (int i = 0; i < (int)(sizeof(TUBE_REFS) / sizeof(TUBE_REFS[0])); i++)
        if (!FindDword(TUBE_REFS[i], ADDR_TubeVisited)) { Log("patch: tubes - no bitmap reference at 0x%08X - NOT moved", (unsigned)TUBE_REFS[i]); return; }
    for (int i = 0; i < 4; i++) {
        BYTE* p = (BYTE*)TUBE_ROW_BYTES[i];
        if (p[0] != 0xB1 || p[1] != 6) { Log("patch: tubes - 0x%08X isn't mov cl,6 - NOT moved", (unsigned)TUBE_ROW_BYTES[i]); return; }
    }
    for (int i = 0; i < 2; i++) {
        BYTE* p = (BYTE*)TUBE_ROW_DWORDS[i];
        if (p[0] != 0xB1 || p[1] != 4) { Log("patch: tubes - 0x%08X isn't mov cl,4 - NOT moved", (unsigned)TUBE_ROW_DWORDS[i]); return; }
    }
    static const BYTE clr[]    = { 0xB9, 0x00, 0x10, 0x00, 0x00 };
    static const BYTE clrNew[] = { 0xB9, 0x00, 0x80, 0x00, 0x00 };   // 1024 rows x 32 dwords
    if (!PatchBytes(ADDR_TubeClear, clr, clrNew, 5, "tube bitmap clear")) return;
    DWORD base = (DWORD)(uintptr_t)g_tubeVisited;
    for (int i = 0; i < (int)(sizeof(TUBE_REFS) / sizeof(TUBE_REFS[0])); i++)
        WriteCode(TUBE_REFS[i] + FindDword(TUBE_REFS[i], ADDR_TubeVisited), &base, 4);
    BYTE seven = 7, five = 5;
    for (int i = 0; i < 4; i++) WriteCode(TUBE_ROW_BYTES[i] + 1, &seven, 1);
    for (int i = 0; i < 2; i++) WriteCode(TUBE_ROW_DWORDS[i] + 1, &five, 1);
    Log("patch: tube flood-fill bitmap moved into the module (1024 x 1024)");
}

// ---- 3. corridor rows -----------------------------------------------------------------------
// The corridor A* (PathFinder.SearchStraightCorridor 0x00440770 and RefineCoarseSteps 0x00440D60
// build a corridor; SearchCorridor 0x00441360 searches it) keeps the corridor as a list of tile
// intervals per row: a table of 256 row pointers at the start of a corridor object on the stack,
// indexed by the tile's absolute row. Each place that indexes the table is sent through a stub
// that uses (row & 0xFF) instead. The stubs are reached by a JMP over the original instructions
// and end with a JMP back to the next original instruction (the g_*Back addresses below); each
// does exactly what the instructions it covers did, plus the mask.
static uintptr_t g_insertBack  = 0x00440B47;   // InsertInterval, after `mov esi,[esp+0Ch]; push edi`
static uintptr_t g_refineNext  = 0x00440F23;   // RefineCoarseSteps, `test edi,edi; jge` not taken
static uintptr_t g_refineSkip  = 0x0044102B;   //   ... and taken (the jge's target)
static uintptr_t g_search1Back = 0x0044143F;   // SearchCorridor, after the start row's lookup
static uintptr_t g_search2Back = 0x00441697;   // SearchCorridor, after a neighbour row's lookup

// Corridor.InsertInterval(row, x1, x2), __thiscall (ecx = corridor): row = row & 0xFF.
// Covers `mov esi,[esp+0Ch]` (the row argument) + `push edi`.
static __declspec(naked) void InsertIntervalStub() {
    __asm {
        movzx esi, byte ptr [esp + 0Ch]     // was: mov esi,[esp+0Ch]
        push edi
        jmp dword ptr [g_insertBack]
    }
}

// RefineCoarseSteps: esi is the row. It has already been checked against the map height (just
// before 0x440F1B) and from here to 0x441070, where esi is reloaded, it is only used to index the
// row table (`[esp+esi*4+30h]`) and passed to InsertInterval - so it can be masked here.
// Covers `test edi,edi` + `jge 0x44102B` (8 bytes).
static __declspec(naked) void RefineRowStub() {
    __asm {
        and esi, 0FFh
        test edi, edi
        jge skip
        jmp dword ptr [g_refineNext]
    skip:
        jmp dword ptr [g_refineSkip]
    }
}

// SearchCorridor: the start tile's row lookup. Covers `mov edx,[eax+10h]` (edx = the tile's y, left
// unmasked - the code after uses it as the real row), `mov ebx,[esp+16888h]` (the corridor object)
// and `mov eax,[ebx+edx*4]` (its row pointer), 13 bytes; the lookup uses dl (= y & 0xFF).
static __declspec(naked) void SearchRow1Stub() {
    __asm {
        mov edx, dword ptr [eax + 10h]
        mov ebx, dword ptr [esp + 16888h]   // the corridor object
        movzx eax, dl
        mov eax, dword ptr [ebx + eax * 4]
        jmp dword ptr [g_search1Back]
    }
}

// SearchCorridor: a neighbour tile's row lookup (eax = the corridor object, ebp = the tile's y).
// Covers `mov ecx,[eax+0C1Ch]` (the copied x mask) and `mov eax,[eax+ebp*4]`, 9 bytes; ebp is
// masked only for the lookup and given back unchanged.
static __declspec(naked) void SearchRow2Stub() {
    __asm {
        mov ecx, dword ptr [eax + 0C1Ch]
        push ebp
        and ebp, 0FFh
        mov eax, dword ptr [eax + ebp * 4]
        pop ebp
        jmp dword ptr [g_search2Back]
    }
}

// The bytes each patch site must hold, checked all together before anything is changed.
// The numbering loop in SearchCorridor walks the row table from row 0 while row < the corridor's
// copy of the map height (+0xC20), giving each interval a node number. With rows masked the table
// has 256 rows whatever the height, so the loop is bounded at 256 instead: its first test
// (`cmp [eax+C20h],ecx; jle` with ecx = edx = 0) and its loop test (`cmp [eax+C20h],edx; jg`) become
// `cmp edx,100h` with the jump turned round. Rows the corridor doesn't use are null and skipped, so
// on maps up to 256 tall the loop numbers exactly what it did.
static void InstallCorridor() {
    static const BYTE ins[] = { 0x8B, 0x74, 0x24, 0x0C, 0x57 };                     // mov esi,[esp+0Ch]; push edi
    static const BYTE ref[] = { 0x85, 0xFF, 0x0F, 0x8D, 0x08, 0x01, 0x00, 0x00 };   // test edi,edi; jge 0x44102B
    static const BYTE s1[]  = { 0x8B, 0x50, 0x10,                                   // mov edx,[eax+10h]
                                0x8B, 0x9C, 0x24, 0x88, 0x68, 0x01, 0x00,           // mov ebx,[esp+16888h]
                                0x8B, 0x04, 0x93 };                                 // mov eax,[ebx+edx*4]
    static const BYTE s2[]  = { 0x8B, 0x88, 0x1C, 0x0C, 0x00, 0x00,                 // mov ecx,[eax+0C1Ch]
                                0x8B, 0x04, 0xA8 };                                 // mov eax,[eax+ebp*4]
    static const BYTE l1[]  = { 0x39, 0x88, 0x20, 0x0C, 0x00, 0x00, 0x7E, 0x3F };   // cmp [eax+C20h],ecx; jle
    static const BYTE l1n[] = { 0x81, 0xFA, 0x00, 0x01, 0x00, 0x00, 0x7D, 0x3F };   // cmp edx,100h; jge (edx = ecx = 0 here)
    static const BYTE l2[]  = { 0x39, 0x90, 0x20, 0x0C, 0x00, 0x00, 0x7F, 0xC3 };   // cmp [eax+C20h],edx; jg
    static const BYTE l2n[] = { 0x81, 0xFA, 0x00, 0x01, 0x00, 0x00, 0x7C, 0xC3 };   // cmp edx,100h; jl
    if (memcmp((void*)ADDR_InsertInterval, ins, 5) || memcmp((void*)ADDR_RefineRow, ref, 8) ||
        memcmp((void*)ADDR_SearchRow1, s1, 13) || memcmp((void*)ADDR_SearchRow2, s2, 9) ||
        memcmp((void*)ADDR_SearchLoopFirst, l1, 8) || memcmp((void*)ADDR_SearchLoopNext, l2, 8)) {
        Log("patch: corridor - unexpected bytes - NOT patched (corridor searches on maps over 256 tall may crash)");
        return;
    }
    PatchJmp(ADDR_InsertInterval, ins, 5, (void*)&InsertIntervalStub, "corridor InsertInterval");
    PatchJmp(ADDR_RefineRow, ref, 8, (void*)&RefineRowStub, "corridor RefineCoarseSteps row");
    PatchJmp(ADDR_SearchRow1, s1, 13, (void*)&SearchRow1Stub, "corridor search start row");
    PatchJmp(ADDR_SearchRow2, s2, 9, (void*)&SearchRow2Stub, "corridor search neighbour row");
    PatchBytes(ADDR_SearchLoopFirst, l1, l1n, 8, "corridor numbering loop (first)");
    PatchBytes(ADDR_SearchLoopNext, l2, l2n, 8, "corridor numbering loop (next)");
    Log("patch: corridor rows indexed by row & 0xFF");
}

// ---- 5. bounded wide maps: the view ------------------------------------------------------------
// A wide map (no pad) with an x clip rect from its file. A stock 512-wide map has x1 = -1 and
// x2 = 0x7FFFFFFF ("no x clip"), so this is false for it. Read live: the clip rect comes from the
// map file, after the map is allocated.
static bool BoundedWide() {
    int x1 = *(int*)ADDR_MapClipX1, x2 = *(int*)ADDR_MapClipX2, w = *(int*)ADDR_MapTileWidth;
    return *(int*)ADDR_MapPadX == 0 && x1 >= 0 && x2 < w;
}

// IWnd:Pane:Detail.ClampViewPos, wrapped: on a bounded wide map X is clamped to the clip rect first,
// then the game's own function runs. It wraps X (already in range, so no change) and clamps Y. The
// game's function must do the rest itself, not a copy of it here: it reads the view size with
// GetClientRect, and other modules (Zoom) answer that call with their own view size, recognising
// the call by its return address inside this function.
// viewPos = the view's top-left pixel {x, y}; pane+0x14 = the detail surface, whose bitmap width
// (+0xCC, already divided by the zoom, and set by whoever sized the surface) is the view's width in
// map pixels.
// The game's function is __thiscall(pane, viewPos) and pops its one stack argument; a __fastcall
// with a dummy edx has the same calling shape, which is how it is called and replaced here.
typedef void (__fastcall *ClampViewPos_t)(BYTE* pane, int edx, int* viewPos);
static BYTE g_clampTramp[16];            // ClampViewPos's first 7 bytes + a jump back (the original function)
static void __fastcall ClampViewPos(BYTE* pane, int edx, int* viewPos) {
    if (*(int*)ADDR_MapPadX == 0 && BoundedWide()) {
        BYTE* surface = *(BYTE**)(pane + 0x14);
        int viewW = surface ? *(int*)(surface + 0xCC) : 0;
        int lo = *(int*)ADDR_MapClipX1 * 32;
        int hi = (*(int*)ADDR_MapClipX2 + 1) * 32 - viewW;
        if (viewPos[0] > hi) viewPos[0] = hi;
        if (viewPos[0] < lo) viewPos[0] = lo;            // a view wider than the map: the left edge wins
    }
    ((ClampViewPos_t)(void*)g_clampTramp)(pane, edx, viewPos);
}

// ---- 5b. bounded wide maps: long routes in legs ------------------------------------------------
// PlanPath is __thiscall(pathfinder, unit, moveState) and pops 8 bytes: called and replaced as a
// __fastcall with a dummy edx. g_planTramp is the original function (its first instruction, then a
// jump back), so the wrapper can run it with a different destination.
typedef void (__fastcall *PlanPath_t)(void* pf, int edx, void* unit, BYTE* wp);
static BYTE  g_planTramp[16];            // PlanPath's first instruction + a jump back
static long  g_legs;                     // leg goals handed out (for the log)

// Plans a route; on a bounded wide map a move LEG_LIMIT or more columns east or west is planned to
// a leg goal instead (see the header, part 5). Everything else goes straight to the game's planner.
// The leg goal can be on rock: the planner then heads for it and stops short, which is fine, since
// the next leg is planned from wherever the vehicle gets to.
static void __fastcall PlanPathLegs(void* pf, int edx, void* unit, BYTE* wp) {
    PlanPath_t orig = (PlanPath_t)(void*)g_planTramp;
    if (!BoundedWide()) { orig(pf, edx, unit, wp); return; }
    int sx = *(int*)(wp + 0x48) >> 5, sy = *(int*)(wp + 0x4C) >> 5;
    int dx = *(int*)(wp + 0x50), dy = *(int*)(wp + 0x54);
    int ddx = dx - sx;                    // the true distance: nothing crosses the seam
    if (ddx < LEG_LIMIT && ddx > -LEG_LIMIT) { orig(pf, edx, unit, wp); return; }
    // A leg goal LEG_TILES along the straight line to the destination.
    int adx = ddx < 0 ? -ddx : ddx;
    int lx = sx + (ddx < 0 ? -LEG_TILES : LEG_TILES);
    int ly = sy + (int)((long long)(dy - sy) * LEG_TILES / adx);
    *(int*)(wp + 0x50) = lx;
    *(int*)(wp + 0x54) = ly;
    orig(pf, edx, unit, wp);
    *(int*)(wp + 0x50) = dx;              // the real waypoint back
    *(int*)(wp + 0x54) = dy;
    *(int*)(wp + 0x38) = 0;               // not at the goal yet: plan the next leg at the end of this one
    if ((++g_legs & 0xFF) == 1) Log("route: long move (%d,%d) -> (%d,%d) planned in legs, via (%d,%d) [%ld legs so far]",
                                     sx, sy, dx, dy, lx, ly, g_legs);
}

// Builds the trampoline (PlanPath's first instruction, `sub esp,968h`, which runs the same
// anywhere, then a JMP to the instruction after it) and points PlanPath's entry at the wrapper.
// Every caller of PlanPath goes through the wrapper, including any other module that calls
// 0x00440300 itself.
static void InstallRouteLegs() {
    static const BYTE entry[] = { 0x81, 0xEC, 0x68, 0x09, 0x00, 0x00 };   // sub esp,968h
    if (memcmp((void*)ADDR_PlanPath, entry, 6) != 0) {
        Log("patch: route legs - unexpected bytes at 0x%08X - not installed (long moves on bounded maps may go the wrong way)", (unsigned)ADDR_PlanPath);
        return;
    }
    DWORD op;                            // the trampoline lives in this DLL's data: make it executable
    VirtualProtect(g_planTramp, sizeof(g_planTramp), PAGE_EXECUTE_READWRITE, &op);
    memcpy(g_planTramp, entry, 6);
    g_planTramp[6] = 0xE9;
    *(int*)(g_planTramp + 7) = (int)((ADDR_PlanPath + 6) - ((uintptr_t)g_planTramp + 11));
    FlushInstructionCache(GetCurrentProcess(), g_planTramp, sizeof(g_planTramp));
    PatchJmp(ADDR_PlanPath, entry, 6, (void*)&PlanPathLegs, "route legs");
    Log("patch: route legs - long east-west moves on bounded maps are planned in legs of %d tiles", LEG_TILES);
}

// Builds the ClampViewPos trampoline the same way (its first two instructions, 7 bytes, then a JMP
// back) and points the function's entry at the wrapper.
static void InstallViewClamp() {
    static const BYTE entry[] = { 0x83, 0xEC, 0x10, 0x8D, 0x44, 0x24, 0x00 };   // sub esp,10h; lea eax,[esp]
    if (memcmp((void*)ADDR_ClampViewPos, entry, 7) != 0) {
        Log("patch: view clamp - unexpected bytes at 0x%08X - not installed (bounded maps wrap the view)", (unsigned)ADDR_ClampViewPos);
        return;
    }
    // Both instructions are position-independent, so they run as they are from the trampoline.
    DWORD op;
    VirtualProtect(g_clampTramp, sizeof(g_clampTramp), PAGE_EXECUTE_READWRITE, &op);
    memcpy(g_clampTramp, entry, 7);
    g_clampTramp[7] = 0xE9;
    *(int*)(g_clampTramp + 8) = (int)((ADDR_ClampViewPos + 7) - ((uintptr_t)g_clampTramp + 12));
    FlushInstructionCache(GetCurrentProcess(), g_clampTramp, sizeof(g_clampTramp));
    if (PatchJmp(ADDR_ClampViewPos, entry, 7, (void*)&ClampViewPos, "view clamp"))
        Log("patch: view clamp - bounded wide maps stop at their clip rect");
}

// ---- 6. waypoint layout, set per map -----------------------------------------------------------
// A waypoint is one dword. Stock:      bits 0-14 pixel x | 15-28 pixel y | 29-31 tolerance (0-7)
//                     Tall maps (>512): bits 0-14 pixel x | 15-29 pixel y | 30-31 tolerance (0-3)
// Every instruction that packs or unpacks the y field or the tolerance holds one of these
// constants; on a tall map each is switched to its tall value, back to stock on any other map. The
// tolerance is the "close enough" box for passing a waypoint on the way; the game writes 0 or 3,
// except for an attack / guard target, which carries the weapon range - but that is always the
// order's last waypoint, which uses the move state's own range field (+0x3C) instead, so capping
// it at 3 (PackTargetAsWaypoint, below) changes nothing.
struct WpSite { uintptr_t at; DWORD stock, tall; int size; };   // size 4 = imm32, 1 = shift count
static const WpSite WP_SITES[] = {
    // pixel-y field: and reg, 1FFF8000h (insert, and decode before shr 0Fh)
    { 0x0040E431, 0x1FFF8000, 0x3FFF8000, 4 },   // Player.ProcessCommandPacket (orders from the net)
    { 0x0040FB38, 0x1FFF8000, 0x3FFF8000, 4 },   //   ...
    { 0x0042340B, 0x1FFF8000, 0x3FFF8000, 4 },   // Unit:Vehicle.SetMoveCommand
    { 0x004244DE, 0x1FFF8000, 0x3FFF8000, 4 },   // ScatterUnits
    { 0x0042601A, 0x1FFF8000, 0x3FFF8000, 4 },   // BuildGroup.ProduceAndDock
    { 0x00426412, 0x1FFF8000, 0x3FFF8000, 4 },   // BuildGroup.Build
    { 0x0042869C, 0x1FFF8000, 0x3FFF8000, 4 },   // CombatBase.SetWaypoints
    { 0x0042A191, 0x1FFF8000, 0x3FFF8000, 4 },   // Unit.IssueMove
    { 0x0042A621, 0x1FFF8000, 0x3FFF8000, 4 },   // UnitNode.IssueDock
    { 0x0042A8A8, 0x1FFF8000, 0x3FFF8000, 4 },   // UnitNode.IssueBuild
    { 0x00439953, 0x1FFF8000, 0x3FFF8000, 4 },   // PackTargetAsWaypoint
    { 0x004399C4, 0x1FFF8000, 0x3FFF8000, 4 },   //   ...
    { 0x0044C128, 0x1FFF8000, 0x3FFF8000, 4 },   // Unit:Vehicle.InsertDetourWaypoint
    { 0x0044C183, 0x1FFF8000, 0x3FFF8000, 4 },   //   ...
    { 0x0044D5BB, 0x1FFF8000, 0x3FFF8000, 4 },   // GetMoveDestination
    { 0x00451597, 0x1FFF8000, 0x3FFF8000, 4 },   // MouseCommand.WriteWaypointList
    { 0x004515E7, 0x1FFF8000, 0x3FFF8000, 4 },   //   ...
    { 0x00451CE4, 0x1FFF8000, 0x3FFF8000, 4 },   // Build.OnClickDetailPane
    { 0x004521C4, 0x1FFF8000, 0x3FFF8000, 4 },   // BuildGeoPlantOrMine.OnClickDetailPane
    { 0x00452EA1, 0x1FFF8000, 0x3FFF8000, 4 },   // DestroyTubeOrWall.OnClickDetailPane
    { 0x00454317, 0x1FFF8000, 0x3FFF8000, 4 },   // SetOreRoute.OnClickDetailPane
    { 0x004547FC, 0x1FFF8000, 0x3FFF8000, 4 },   // Patrol.OnClickDetailPane
    { 0x004762FC, 0x1FFF8000, 0x3FFF8000, 4 },   // Unit.DoMove
    { 0x00476427, 0x1FFF8000, 0x3FFF8000, 4 },   // Unit.DoBuild
    // tile-y field: and reg, 1FF00000h (then shr 14h / 0Fh)
    { 0x004232F1, 0x1FF00000, 0x3FF00000, 4 },   // Unit:Vehicle.SetMoveCommand (last waypoint's tile)
    { 0x0043F396, 0x1FF00000, 0x3FF00000, 4 },   // Unit.DrawSelectionBoxAndMouseOverText (waypoint lines)
    { 0x0044C011, 0x1FF00000, 0x3FF00000, 4 },   // Unit:Vehicle.SkipReachedWaypoints
    { 0x0044C43D, 0x1FF00000, 0x3FF00000, 4 },   // Unit:Vehicle.PathfindToNextWaypoint
    // clear the tolerance: and reg, 1FFFFFFFh
    { 0x00424490, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // ScatterUnits
    { 0x004286A6, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // CombatBase.SetWaypoints
    { 0x0042A19C, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // Unit.IssueMove
    { 0x0042A62C, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // UnitNode.IssueDock
    { 0x0042A8BA, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // UnitNode.IssueBuild
    { 0x004399DA, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // PackTargetAsWaypoint
    { 0x0044C198, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // Unit:Vehicle.InsertDetourWaypoint
    { 0x004515A6, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // MouseCommand.WriteWaypointList
    { 0x004515F5, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   //   ...
    { 0x00451CEE, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // Build.OnClickDetailPane
    { 0x004521CE, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // BuildGeoPlantOrMine.OnClickDetailPane
    { 0x00452EAC, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // DestroyTubeOrWall.OnClickDetailPane
    { 0x00454325, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // SetOreRoute.OnClickDetailPane
    { 0x0045480A, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // Patrol.OnClickDetailPane
    { 0x00476313, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // Unit.DoMove
    { 0x0047643D, 0x1FFFFFFF, 0x3FFFFFFF, 4 },   // Unit.DoBuild
    // the tolerance itself
    { 0x004286AB, 0x60000000, 0xC0000000, 4 },   // CombatBase.SetWaypoints: or eax, tolerance 3
    { 0x0045480F, 0x60000000, 0xC0000000, 4 },   // Patrol.OnClickDetailPane: xor eax, tolerance 3
    { 0x0044C41F, 0xE0000000, 0xC0000000, 4 },   // PathfindToNextWaypoint: test eax, any tolerance
    { 0x0044C42C, 0x1D, 0x1E, 1 },               // PathfindToNextWaypoint: shr eax, 29 -> 30
    { 0x0044C09B, 0x1D, 0x1E, 1 },               // SkipReachedWaypoints: shr eax, 29 -> 30
    { 0x0044C18F, 0x1D, 0x1E, 1 },               // InsertDetourWaypoint: shl eax, 29 -> 30 (radius 0)
    { 0x00451571, 0x1D, 0x1E, 1 },               // WriteWaypointList: shl ebx, 29 -> 30 (radius 0)
    { 0x004515E2, 0x1D, 0x1E, 1 },               //   ...
};
static const int NUM_WP_SITES = (int)(sizeof(WP_SITES) / sizeof(WP_SITES[0]));
static int  g_wpOffset[NUM_WP_SITES];    // where in each instruction the constant is (found at install)
static bool g_wpOk;                      // every site checked out: the layout may be switched
static bool g_wpTall;                    // the tall layout is in the game's code now (read by PackToleranceStub)

// PackTargetAsWaypoint (0x00439910): `movsx eax, byte ptr [esp+20h]` + `shl eax, 1Dh` at 0x4399CD
// (8 bytes) shift the tolerance argument into place. This is the one site that can be given a
// tolerance over 3 (an attack's weapon range), and in 2 bits 4-7 would turn into 0-3, so the stub
// caps it at 3 on a tall map (an unsigned compare also caps a negative one). On a stock map it
// does exactly what the two instructions did. It is a stub rather than a WP_SITES entry because
// the cap needs code, not a different constant.
static const uintptr_t ADDR_PackTolerance = 0x004399CD;
static uintptr_t g_packTolBack = 0x004399D5;
static __declspec(naked) void PackToleranceStub() {
    __asm {
        movsx eax, byte ptr [esp + 20h]
        cmp byte ptr [g_wpTall], 0
        je stock
        cmp eax, 3
        jbe small
        mov eax, 3
    small:
        shl eax, 1Eh
        jmp dword ptr [g_packTolBack]
    stock:
        shl eax, 1Dh
        jmp dword ptr [g_packTolBack]
    }
}

// Write every site's tall or stock constant. Called as a map is allocated, before anything on
// that map packs a waypoint, so all of a map's waypoints are in one layout.
static void SetWaypointLayout(bool tall) {
    if (!g_wpOk || tall == g_wpTall) return;
    for (int i = 0; i < NUM_WP_SITES; i++) {
        DWORD v = tall ? WP_SITES[i].tall : WP_SITES[i].stock;
        WriteCode(WP_SITES[i].at + g_wpOffset[i], &v, WP_SITES[i].size);
    }
    g_wpTall = tall;
}

static void InstallWaypoints() {
    // Find each constant in its instruction (immediate after a 1- or 2-byte opcode); all or none.
    for (int i = 0; i < NUM_WP_SITES; i++) {
        const WpSite& s = WP_SITES[i];
        int off = 0;
        if (s.size == 4) {
            for (int j = 1; j <= 2 && !off; j++) if (*(DWORD*)(s.at + j) == s.stock) off = j;
        } else if (((BYTE*)s.at)[0] == 0xC1 && ((BYTE*)s.at)[2] == (BYTE)s.stock) {
            off = 2;
        }
        if (!off) { Log("patch: waypoints - 0x%08X isn't what was expected - NOT installed (maps over 512 tall won't work)", (unsigned)s.at); return; }
        g_wpOffset[i] = off;
    }
    static const BYTE tol[] = { 0x0F, 0xBE, 0x44, 0x24, 0x20, 0xC1, 0xE0, 0x1D };
    if (!PatchJmp(ADDR_PackTolerance, tol, 8, (void*)&PackToleranceStub, "waypoint tolerance cap")) {
        Log("patch: waypoints - NOT installed (maps over 512 tall won't work)"); return;
    }
    g_wpOk = true;
    Log("patch: waypoints - %d sites switch to the tall layout on maps over 512 tall", NUM_WP_SITES);
}

// ---- 4 and 6: per-map settings -----------------------------------------------------------------
// Sets the blight/lava spread taps (4) and the waypoint layout (6) for the map's size. Called when
// a map is allocated (new game or saved game, before anything is placed on it) and, as a fallback,
// from the game-cycle hook; it does nothing while the size is the one it last saw.
static void UpdateForMap() {
    int w = *(int*)ADDR_MapTileWidth, h = *(int*)ADDR_MapTileHeight;
    if (w == g_lastW && h == g_lastH) return;
    g_lastW = w; g_lastH = h;
    bool big = (long long)w * h > 0x20000;
    if (g_tapsOk) {
        DWORD taps = big ? TAPS_BIG : TAPS_STOCK;
        if (*(DWORD*)(ADDR_SpreadTaps + 1) != taps) WriteCode(ADDR_SpreadTaps + 1, &taps, 4);
    }
    SetWaypointLayout(h > 512);
    g_edgesLogged = false;                // the clip rect is read from the file after this: logged later
    Log("map: %d x %d tiles in memory, blight/lava spread taps %s, waypoints %s", w, h,
        big ? "20-bit" : "stock 17-bit", g_wpTall ? "tall layout (y 15 bits)" : "stock layout");
    if (w > 1024 || h > 1024)
        Log("map: WARNING - bigger than 1024 x 1024: the tube bitmap only covers 1024 x 1024");
    else if (h > 512 && !g_wpTall)
        Log("map: WARNING - taller than 512 but the waypoint patch isn't installed: rows 512 and below can't be reached");
}

// Map.AllocateSpaceForMap (0x004354F0), at 0x435728 (`cmp [ebx+10h],esi / jle 0x435793`, the start
// of the light-table loop): the map's size is set by now. Run UpdateForMap (all registers kept),
// then those 5 bytes: compare the width with esi (0) and skip the loop if it is 0, as the game does.
static const uintptr_t ADDR_AllocHook = 0x00435728;
static uintptr_t g_allocLoop = 0x0043572D, g_allocSkip = 0x00435793;
static __declspec(naked) void AllocHookStub() {
    __asm {
        pushad
        call UpdateForMap
        popad
        cmp dword ptr [ebx + 10h], esi
        jle skip
        jmp dword ptr [g_allocLoop]
    skip:
        jmp dword ptr [g_allocSkip]
    }
}

static void InstallAllocHook() {
    static const BYTE loop[] = { 0x39, 0x73, 0x10, 0x7E, 0x66 };    // cmp [ebx+10h],esi; jle 0x435793
    if (PatchJmp(ADDR_AllocHook, loop, 5, (void*)&AllocHookStub, "map allocation hook"))
        Log("patch: map allocation hook - per-map settings set as each map is made");
    else
        Log("patch: map allocation hook not installed - per-map settings wait for the first game cycle");
}

// ---- debug: test orders from a file, and a vehicle log ---------------------------------------
// With [MapSize] Debug=1, every DebugEvery ticks (default 200 = 2 marks) each vehicle is logged:
// its pool index, owner, tile, order and action, the move state's destination tile and its current
// waypoint decoded in both layouts. And OPU\logs\MapSize-cmd.txt, if it exists, is read every 20
// ticks on the game thread, run and deleted. Its lines (tiles are the game's own, 0-based):
//     move <index> <x> <y>     the vehicle with that pool index moves to tile (x, y)
//     moveall <x> <y>          every vehicle of player 0 moves there
//     log                      log the vehicles now
//     view <x> <y>             put the view's top-left at map pixel (x, y), through the game's
//                              ClampViewPos (as scrolling does), and log where it ends up
// Orders go through the game's own Unit::DoMove (0x00476240, the mission SDK's), which packs the
// waypoint with the instructions this module switches - the same as an order from a mission DLL.
static const uintptr_t ADDR_UnitDoMove  = 0x00476240;   // __thiscall(Unit{int index}*, int tileX, int tileY), ret 8
static const uintptr_t ADDR_MapHead     = 0x0054F84C;   // Map+0x54 head sentinel of the unit list (sorted by pixel y)
static const uintptr_t ADDR_MapTail     = 0x0054F850;   // Map+0x58 tail sentinel
static const uintptr_t ADDR_GameTick    = 0x0056EB1C;   // TethysGame tick
// Unit::DoMove takes the mission SDK's Unit (one int: the pool index) as `this` and the tile by
// value (LOCATION = two ints), and pops them: a __fastcall with a dummy edx has the same shape.
typedef void (__fastcall *UnitDoMove_t)(int* sdkUnit, int edx, int tileX, int tileY);
static int  g_debug;                     // [MapSize] Debug
static int  g_debugEvery = 200;          // [MapSize] DebugEvery: ticks between vehicle logs (0 = only on `log`)
static int  g_nextDebugTick, g_nextCmdTick;

// Log every live vehicle, walking the game's unit list (head sentinel -> +0x04 next -> tail).
// Unit fields used: +0x10 pool index, +0x14/+0x18 pixel x/y, +0x1D owner (low 4 bits), +0x21 order
// (command packet type), +0x22 action, +0x34 move state, +0x38 order waypoints, +0x44 flags
// (0x2 vehicle, 0x20000 dead). Move state: +0x50/+0x54 destination tile, +0x74/+0x78 steps and the
// current one, +0xBC/+0xC0 coarse plan steps and its cursor, +0x38 "plan reaches the goal",
// +0x6C bump retries. Order waypoints: +0x00 count, +0x08 current, +0x14 the packed waypoints.
static void LogVehicles(const char* why) {
    BYTE* head = *(BYTE**)ADDR_MapHead;
    BYTE* tail = *(BYTE**)ADDR_MapTail;
    if (!head || !tail) return;
    int n = 0;
    Log("vehicles (%s), tick %d:", why, *(int*)ADDR_GameTick);
    for (BYTE* u = *(BYTE**)(head + 4); u && u != tail && n < 2048; u = *(BYTE**)(u + 4), n++) {
        DWORD flags = *(DWORD*)(u + 0x44);
        if (!(flags & 2) || (flags & 0x20000)) continue;               // vehicles that are alive
        BYTE* ms = *(BYTE**)(u + 0x34);                                // move state
        BYTE* wb = *(BYTE**)(u + 0x38);                                // order waypoints
        char wp[96] = "-";
        if (wb && (uintptr_t)wb > 0x10000) {
            int cur = *(int*)(wb + 8), num = *(int*)wb;
            if (cur >= 0 && cur < 8 && cur < num) {
                DWORD w = *(DWORD*)(wb + 0x14 + cur * 4);
                snprintf(wp, sizeof(wp), "wp %d/%d 0x%08X = stock (%d,%d) tall (%d,%d)", cur, num, (unsigned)w,
                         (int)(w & 0x7FFF) >> 5, (int)((w >> 15) & 0x3FFF) >> 5,
                         (int)(w & 0x7FFF) >> 5, (int)((w >> 15) & 0x7FFF) >> 5);
            }
        }
        Log("  #%d owner %d tile (%d,%d) cmd 0x%02X action 0x%02X %s", *(int*)(u + 0x10), u[0x1D] & 15,
            *(int*)(u + 0x14) >> 5, *(int*)(u + 0x18) >> 5, u[0x21], u[0x22], wp);
        if (ms && (uintptr_t)ms > 0x10000)
            Log("      move state: dest tile (%d,%d) steps %d/%d coarse %d/%d status %d bumps %d", *(int*)(ms + 0x50), *(int*)(ms + 0x54),
                *(int*)(ms + 0x78), *(int*)(ms + 0x74), *(int*)(ms + 0xC0), *(int*)(ms + 0xBC), *(int*)(ms + 0x38), *(int*)(ms + 0x6C));
    }
}

// `view x y`: the view's top-left goes to map pixel (x, y) through ClampViewPos (0x00407990, so
// this module's wrapper and then the game's function), and is written to the detail pane as the
// game's scrolling does. Not CenterViewOn (0x00408210): that one ignores calls after the first
// tick while the pane's view lock (+0x134) is set, which it normally is.
static const uintptr_t ADDR_DetailPane = 0x005758B4;  // gameFrame (0x005756C0) + 0x1F4; +0x140/+0x144 view pixel x/y
static void DebugView(int x, int y) {
    int v[2] = { x, y };
    ((ClampViewPos_t)ADDR_ClampViewPos)((BYTE*)ADDR_DetailPane, 0, v);
    *(int*)(ADDR_DetailPane + 0x140) = v[0];
    *(int*)(ADDR_DetailPane + 0x144) = v[1];
    BYTE* surface = *(BYTE**)(ADDR_DetailPane + 0x14);
    Log("cmd: view (%d,%d) -> clamped to (%d,%d), view %d x %d map pixels, so it shows to (%d,%d); map %d x %d px",
        x, y, v[0], v[1], surface ? *(int*)(surface + 0xCC) : 0, surface ? -*(int*)(surface + 0xD0) : 0,
        v[0] + (surface ? *(int*)(surface + 0xCC) : 0), v[1] + (surface ? -*(int*)(surface + 0xD0) : 0),
        *(int*)ADDR_MapTileWidth * 32, *(int*)ADDR_MapTileHeight * 32);
}

// `move index x y`: the game's Unit::DoMove (it does nothing for a dead unit or a tile off the map).
static void DebugMove(int index, int x, int y) {
    int sdk = index;
    ((UnitDoMove_t)ADDR_UnitDoMove)(&sdk, 0, x, y);
    Log("cmd: #%d move to (%d,%d)", index, x, y);
}

// Run OPU\logs\MapSize-cmd.txt, if there is one, line by line, then delete it (so each file runs
// once; write a new one for the next commands). Unknown lines are skipped.
static void RunDebugCommands() {
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\logs\\MapSize-cmd.txt", GameDir());
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int a, b, c;
        if (sscanf(line, "move %d %d %d", &a, &b, &c) == 3) DebugMove(a, b, c);
        else if (sscanf(line, "moveall %d %d", &a, &b) == 2) {
            BYTE* head = *(BYTE**)ADDR_MapHead; BYTE* tail = *(BYTE**)ADDR_MapTail;
            int idx[256], k = 0;                                       // collect first: orders re-sort the list
            for (BYTE* u = *(BYTE**)(head + 4); u && u != tail && k < 256; u = *(BYTE**)(u + 4)) {
                DWORD fl = *(DWORD*)(u + 0x44);
                if ((fl & 2) && !(fl & 0x20000) && (u[0x1D] & 15) == 0) idx[k++] = *(int*)(u + 0x10);
            }
            for (int i = 0; i < k; i++) DebugMove(idx[i], a, b);
        }
        else if (sscanf(line, "view %d %d", &a, &b) == 2) DebugView(a, b);
        else if (strncmp(line, "log", 3) == 0) LogVehicles("asked");
    }
    fclose(f);
    DeleteFileA(path);
}

// Each game cycle with Debug=1: commands every 20 ticks, the vehicle log every DebugEvery ticks.
static void DebugTick() {
    int t = *(int*)ADDR_GameTick;
    if (t < g_nextCmdTick - 1000) g_nextCmdTick = g_nextDebugTick = 0;   // the tick went back: a new game or a loaded save
    if (t >= g_nextCmdTick) { g_nextCmdTick = t + 20; RunDebugCommands(); }
    if (g_debugEvery > 0 && t >= g_nextDebugTick) { g_nextDebugTick = t + g_debugEvery; LogVehicles("every"); }
}

// The shared game-cycle call: check the map size, then run the cycle (and any module chained before us).
static void __fastcall HookedProcessGameCycle(void* self, void* /*edx*/) {
    UpdateForMap();
    if (!g_edgesLogged) {                 // first cycle on this map: its clip rect is loaded by now
        g_edgesLogged = true;
        Log("map: %s, clip x %d..%d, y %d..%d",
            *(int*)ADDR_MapPadX ? "padded (hard edges)" : BoundedWide() ? "bounded (hard edges)" : "wraps around east-west",
            *(int*)ADDR_MapClipX1, *(int*)ADDR_MapClipX2, *(int*)(ADDR_MapClipX1 + 4), *(int*)(ADDR_MapClipX2 + 4));
    }
    if (g_debug) DebugTick();
    g_nextCycle(self);
}

// Point the game's one ProcessGameCycle call at HookedProcessGameCycle, remembering where it went
// before (the engine, or another op2-patchs module that chained it first - each module calls on
// to the one before it, so they all run).
static bool HookGameCycle() {
    BYTE* site = (BYTE*)CALLSITE_PGC;
    if (site[0] != 0xE8) { Log("hook: no CALL at 0x%08X - not installed", (unsigned)CALLSITE_PGC); return false; }
    uintptr_t cur = CALLSITE_PGC + 5 + *(int*)(site + 1);
    g_nextCycle = (ProcGameCycle_t)cur;
    int rel = (int)((uintptr_t)&HookedProcessGameCycle - (CALLSITE_PGC + 5));
    WriteCode(CALLSITE_PGC + 1, &rel, 4);
    Log("hook: game cycle call @ 0x%08X (chains to 0x%08X%s)", (unsigned)CALLSITE_PGC, (unsigned)cur,
        cur == ADDR_ProcessGameCycle ? ", the engine" : ", another module");
    return true;
}

// Check the spread-taps instruction (`mov ecx, 12000h`) and chain the game-cycle call. The cycle
// hook also carries the edges log line and the debug commands, so those need it too.
static void InstallTaps() {
    BYTE* p = (BYTE*)ADDR_SpreadTaps;
    if (p[0] != 0xB9 || *(DWORD*)(p + 1) != TAPS_STOCK) {
        Log("patch: spread - 0x%08X isn't mov ecx,12000h - left stock", (unsigned)ADDR_SpreadTaps); return;
    }
    if (!HookGameCycle()) return;
    g_tapsOk = true;
    Log("patch: blight/lava spread taps set per map (stock up to 2^17 tiles)");
}

// ---- op2ext entry points -------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hInst);
    return TRUE;
}

// op2ext calls this once at load with the module's ini section name ("MapSize").
EXPORT void InitMod(char* iniSectionName) {
    if (g_installed) return;
    g_installed = true;
    const char* sec = (iniSectionName && *iniSectionName) ? iniSectionName : "MapSize";
    char ini[MAX_PATH];
    snprintf(ini, MAX_PATH, "%s\\outpost2.ini", GameDir());
    int enabled = GetPrivateProfileIntA(sec, "Enabled", 1, ini);
    g_debug      = GetPrivateProfileIntA(sec, "Debug", 0, ini);
    g_debugEvery = GetPrivateProfileIntA(sec, "DebugEvery", 200, ini);
    Log("=== MapSize start (section [%s], Enabled=%d, Debug=%d, DebugEvery=%d) ===", sec, enabled, g_debug, g_debugEvery);
    if (!enabled) { Log("DISABLED: Enabled=0 - nothing patched"); return; }
    // Each part checks its own bytes and is left out alone if they are wrong. The allocation hook
    // goes last: it calls UpdateForMap, which needs the waypoint sites and the taps checked first.
    CheckLight();
    InstallTubes();
    InstallCorridor();
    InstallViewClamp();
    InstallRouteLegs();
    InstallWaypoints();
    InstallTaps();
    InstallAllocHook();
}

// op2ext calls this at shutdown. The patches stay (the game is exiting).
EXPORT bool DestroyMod() {
    Log("=== MapSize stop ===");
    return true;
}
