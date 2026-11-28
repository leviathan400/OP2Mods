// rally.cpp  -  Rally: newly built vehicles drive to a rally point
// =================================================================
// An op2ext module for Outpost 2 OPU 1.4.1 (Outpost2.exe, fixed image base 0x00400000).
// Every address below was checked against the OPU 1.4.1 exe (the exported Unit /
// TethysGame / enumerator methods by their export names).
//
// WHAT THE MOD DOES
// -----------------
// A vehicle that rolls out of one of your Vehicle or Arachnid Factories drives to a rally
// point instead of sitting at the factory. There are two rally points:
//     MILITARY - combat vehicles (Lynx, Panther, Tiger, Scorpion)
//     COLONY   - everything else (ConVec, Cargo Truck, Robo-Miner, Robo-Dozer, Scout, Spider, ...)
// A group whose point isn't set (in the ini or in game) is left alone.
//
// SETTING A RALLY POINT IN GAME
// -----------------------------
// Select a Vehicle or Arachnid Factory, point the mouse at a tile on the map, and press
//     [   to set the MILITARY rally point
//     ]   to set the COLONY rally point
// You get a message in the message log and (single player) a marker on the tile for
// MARKER_LIFETIME ticks. While a Vehicle/Arachnid Factory is selected, both rally points are
// marked on the map, so you can see where its vehicles will go. Points set in game last until the game ends; each new game (or
// loaded save) starts again from the ini values.
//
// HOW IT WORKS
// ------------
// It hooks the one call to TethysGame::ProcessGameCycle (0x0049C4BD) and, after each game
// cycle, runs a few small passes:
//   RallyTick     - lists the local player's vehicles; one that wasn't there the cycle before
//                   AND stands next to one of the player's Vehicle/Arachnid Factories was just
//                   built, so it gets Unit::DoMove to its group's rally tile (a real ctMoMove
//                   command through the game's command queue). Vehicles that appear anywhere
//                   else (mission reinforcements, a loaded save, a unit leaving a garage away
//                   from a factory) are left alone.
//   SelectionTick - is a Vehicle/Arachnid Factory selected? (gates the hotkeys; shows the markers)
//   HotkeyTick    - [ / ] while the game window has focus; reads the tile under the mouse.
//   MarkerTick    - removes a marker once its time is up (MARKER_LINGER ticks after the
//                   factory is deselected, MARKER_LIFETIME after a point is set).
//   Publish       - the UI feed (below).
// A new game or a loaded save is noticed by the game tick going back (or jumping ahead):
// everything per-game is reset and the vehicles already there are taken as they are.
//
// UI FEED (for ui\Rally-UI.exe)
// --------------------------------------
// Every RL_PUBLISH_EVERY cycles Publish() writes a snapshot (both rally points, the selected
// factory, the player's factories and the last vehicles sent, with where they are now) into
// the named shared memory "Local\OP2Rally" (layout and protocol: rl_shared.h). It only reads
// game state; if the mapping can't be created the module works as before, without the feed.
// (Rally-UI shows just the two points and the status line.)
//
// COORDINATES
// -----------
// The engine's tile numbers ("absolute") are offset from what the status bar shows
// ("displayed") by (+31 X, -1 Y). The ini, the log, the messages and the UI feed use the
// displayed numbers; the module works in absolute tiles and converts at those edges.
//
// MULTIPLAYER
// -----------
// The moves are ordinary commands for the player's own units, so they are lockstep-safe in
// principle (not yet tested in a real multiplayer game). The marker is NOT: PlaceMarker /
// DoDeath create and remove a real unit on one machine only. Use Marker=0 in multiplayer.
//
// SHARING THE GAME-CYCLE HOOK
// ---------------------------
// BuildQueue and AutoRepair patch the same call. This module CHAINS: it reads whatever the call
// points at (the engine's function, or another module's hook that chains the same way) and
// calls that. BuildQueue and AutoRepair chain the same way, so all three run together.
//
// INSTALL (OPU 1.4.1, in OPU\outpost2.ini, with Rally.dll in the OPU folder)
// --------------------------------------------------------------------------
//     [ExternalModules]  Rally=yes          ; OPU's loader: must be yes/no
//     [Rally]            Dll=Rally.dll
//                        Enabled=1          ; 1 = on, 0 = the DLL loads but patches nothing
//                        Marker=1           ; 0 = none, 1 = circle, 2 = DNA, 3 = beaker
//                        RallyMilitaryX=    ; optional starting points (displayed tiles);
//                        RallyMilitaryY=    ;   unset = no point until you set one in game
//                        RallyColonyX=
//                        RallyColonyY=
//                        Debug=0            ; 1 = also log selection changes and every new vehicle
// Settings are read once at startup. Log: <game dir>\logs\Rally.log (OPU\logs\Rally.log).
//
// Build: build.bat (32-bit DLL).

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "rl_shared.h"   // the snapshot layout shared with Rally-UI.exe

#define EXPORT extern "C" __declspec(dllexport)

// ---- engine addresses (OPU 1.4.1 Outpost2.exe) --------------------------------------
static const uintptr_t ADDR_ProcessGameCycle = 0x00489EA0;  // TethysGame::ProcessGameCycle(), __thiscall
static const uintptr_t CALLSITE_PGC          = 0x0049C4BD;  // the one `call ProcessGameCycle` (E8 rel32)
static const uintptr_t ADDR_LocalPlayer      = 0x00477F10;  // static int TethysGame::LocalPlayer()      (exported)
static const uintptr_t ADDR_PVE_ctor         = 0x0047A480;  // PlayerVehicleEnum(int player)            (exported)
static const uintptr_t ADDR_PVE_GetNext      = 0x0047A4B0;  // int PlayerVehicleEnum::GetNext(Unit&)    (exported)
static const uintptr_t ADDR_PBE_ctor         = 0x0047A4E0;  // PlayerBuildingEnum(int player, map_id)   (exported)
static const uintptr_t ADDR_PBE_GetNext      = 0x0047A510;  // int PlayerBuildingEnum::GetNext(Unit&)   (exported)
static const uintptr_t ADDR_Unit_IsLive      = 0x00475DD0;  // int Unit::IsLive()      (zeroes the handle if dead)
static const uintptr_t ADDR_Unit_Location    = 0x00475E20;  // LOCATION Unit::Location() const (hidden return ptr)
static const uintptr_t ADDR_Unit_IsBusy      = 0x00475E60;  // int Unit::IsBusy() const
static const uintptr_t ADDR_Unit_OwnerID     = 0x00475EC0;  // int Unit::OwnerID() const
static const uintptr_t ADDR_Unit_GetType     = 0x00475EE0;  // map_id Unit::GetType() const
static const uintptr_t ADDR_Unit_GetWeapon   = 0x00475F20;  // map_id Unit::GetWeapon() const
static const uintptr_t ADDR_Unit_DoMove      = 0x00476240;  // void Unit::DoMove(LOCATION)
static const uintptr_t ADDR_Unit_DoDeath     = 0x004769F0;  // void Unit::DoDeath()   (removes the marker)
static const uintptr_t ADDR_PlaceMarker      = 0x00478BB0;  // static int TethysGame::PlaceMarker(Unit&, x, y, type)
static const uintptr_t ADDR_MsgLogAdd        = 0x00439070;  // MessageLog::AddMessage(px, py, text, sound), __thiscall
static const uintptr_t ADDR_MessageLog       = 0x0054FCF0;  //   the MessageLog object (ECX for the above)

// engine globals (exe data, fixed base)
static const uintptr_t ADDR_SelCount         = 0x00547210;  // number of selected units (0..32)
static const uintptr_t ADDR_SelArray         = 0x00547214;  // their unit numbers
static const uintptr_t ADDR_HoverValid       = 0x0057B7AC;  // nonzero = the mouse is over the map
static const uintptr_t ADDR_HoverPixelX      = 0x0057B7B4;  // map pixel under the mouse (live, follows the mouse)
static const uintptr_t ADDR_HoverPixelY      = 0x0057B7B8;
static const uintptr_t ADDR_MapTileWidth     = 0x0054F808;  // map size in (absolute) tiles
static const uintptr_t ADDR_MapTileHeight    = 0x0054F810;
static const uintptr_t ADDR_GameTick         = 0x0056EB1C;  // current game tick

// displayed tile = absolute tile + (DISP_DX, DISP_DY)
static const int DISP_DX = -31, DISP_DY = +1;

static const int VK_SET_MILITARY   = 0xDB;   // '['
static const int VK_SET_COLONY     = 0xDD;   // ']'
static const int START_GRACE_TICKS = 100;    // a new game's own units trickle in: don't rally before this tick
static const int FACTORY_REACH     = 5;      // a new vehicle within this many tiles of a factory came out of it
static const int MARKER_LIFETIME   = 200;    // ticks the marker stays after setting a point
static const int MARKER_LINGER     = 16;     // ticks the markers stay after the factory is deselected
static const int MAX_UNIT_INDEX    = 4096;
static const int FAULT_LIMIT       = 10;     // after this many caught faults the module stops

// ---- calling conventions ------------------------------------------------------------
// The Unit::* methods take ECX = a Unit object, which is just { int unitIndex }.
struct LOCATION   { int x; int y; };
struct UnitHandle { int unitIndex; };
typedef int       (__fastcall *LocalPlayer_t)();
typedef void      (__thiscall *EnumCtor1_t)(void* self, int playerNum);
typedef void      (__thiscall *EnumCtor2_t)(void* self, int playerNum, int mapIdFilter);
typedef int       (__thiscall *EnumNext_t)(void* self, UnitHandle* outUnit);
typedef int       (__thiscall *UnitFn_t)(UnitHandle* self);
typedef LOCATION* (__thiscall *Location_t)(UnitHandle* self, LOCATION* out);
typedef void      (__thiscall *DoMove_t)(UnitHandle* self, LOCATION loc);
typedef void      (__thiscall *ProcGameCycle_t)(void* self);
typedef int       (__fastcall *PlaceMarker_t)(UnitHandle* outUnit, int tileX, int tileY, int markerType);
typedef void      (__thiscall *MsgLogAdd_t)(void* self, int px, int py, const char* text, int sound);

// ---- settings -----------------------------------------------------------------------
struct Point { bool set; LOCATION abs; bool fromGame; };   // a rally point, in absolute tiles
static Point g_iniMil, g_iniCol;        // from the ini (the start of every game)
static Point g_mil, g_col;              // in use
static int   g_marker = 1;              // [Rally] Marker
static bool  g_debug  = false;          // [Rally] Debug

// ---- per-game state -----------------------------------------------------------------
static int   g_present[MAX_UNIT_INDEX]; // cycle number a vehicle was last listed in
static int   g_cycle      = 0;          // cycles since the module started
static bool  g_baseline   = true;       // next RallyTick: take the vehicles as they are, move none
static int   g_lastTick   = -1;         // to notice a new game / loaded save
static bool  g_activeLogged = false;
static int   g_selFactory = -1;         // selected Vehicle/Arachnid Factory, -1 = none
static bool  g_milKeyDown = false, g_colKeyDown = false;
static int   g_milMarker = -1, g_colMarker = -1;       // marker unit numbers, -1 = none
static int   g_milMarkerUntil = 0, g_colMarkerUntil = 0;  // tick the marker is removed at
static bool  g_milMarkerFailed = false, g_colMarkerFailed = false;   // PlaceMarker refused: don't retry every cycle
static int   g_ralliedMil = 0, g_ralliedCol = 0;

struct Track { bool used; RlTrack t; LOCATION destAbs; };
static Track g_tracks[RL_MAX_TRACKS];   // ring of the last vehicles sent
static int   g_trackHead = 0;           // next slot to write

// ---- module state -------------------------------------------------------------------
static HMODULE         g_hSelf     = NULL;
static ProcGameCycle_t g_nextCycle = NULL;   // what the cycle call pointed at before us
static bool            g_installed = false;
static bool            g_stopped   = false;  // too many faults: passes no longer run
static int             g_faults    = 0;
static char            g_gameDir[MAX_PATH];
static HANDLE          g_shmHandle = NULL;
static RlSnapshot*     g_shm       = NULL;
static int             g_publishCountdown = 0;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = NULL;

// ---- game folder, log ---------------------------------------------------------------
// The folder holding the live outpost2.ini. Under OPU the game runs with the current
// directory = the OPU folder (which has outpost2.ini) while Outpost2.exe sits one level up,
// so prefer the current directory when it has the ini, else use the exe's folder.
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

// Append one line to <game dir>\logs\Rally.log, with the time and game tick. Opened and closed
// per line so the log survives a crash.
static void Log(const char* fmt, ...) {
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\logs", GameDir());
    CreateDirectoryA(path, NULL);
    snprintf(path, MAX_PATH, "%s\\logs\\Rally.log", GameDir());
    FILE* f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d tick %d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            *(int*)ADDR_GameTick);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// ---- small helpers ------------------------------------------------------------------
// Call an exported `int Unit::X() const` method on unit `unitIndex`.
static int UnitCall(uintptr_t fn, int unitIndex) {
    UnitHandle h = { unitIndex };
    return ((UnitFn_t)fn)(&h);
}
// Unit::Location - the unit's tile (absolute frame). It returns the LOCATION through a hidden
// pointer argument, hence the out parameter.
static LOCATION UnitLoc(int unitIndex) {
    UnitHandle h = { unitIndex };
    LOCATION l = { 0, 0 };
    ((Location_t)ADDR_Unit_Location)(&h, &l);
    return l;
}
// Is the unit alive? (Unit::IsLive; it zeroes the handle it is given if not, so a copy is used.)
static bool UnitLive(int unitIndex) {
    UnitHandle h = { unitIndex };                 // IsLive zeroes the handle it is given if dead
    return unitIndex > 0 && ((UnitFn_t)ADDR_Unit_IsLive)(&h) != 0;
}
static int  DispX(int absX) { return absX + DISP_DX; }
static int  DispY(int absY) { return absY + DISP_DY; }
static int  Dist(LOCATION a, LOCATION b) {        // tiles, Chebyshev (a diagonal step = 1)
    int dx = a.x - b.x; if (dx < 0) dx = -dx;
    int dy = a.y - b.y; if (dy < 0) dy = -dy;
    return dx > dy ? dx : dy;
}
static bool IsFactory(int type)  { return type == RL_MAP_VEHICLE_FACTORY || type == RL_MAP_ARACHNID_FACTORY; }
static bool IsMilitary(int type) { return type >= 4 && type <= 7; }   // Scorpion, Lynx, Panther, Tiger

// A readable vehicle name for a map_id, for the log.
static const char* VehName(int t) {
    static const char* n[] = { "None", "Cargo Truck", "ConVec", "Spider", "Scorpion", "Lynx", "Panther",
                               "Tiger", "Robo-Surveyor", "Robo-Miner", "GeoCon", "Scout", "Robo-Dozer",
                               "Evac Transport", "Repair Vehicle", "Earthworker", "Small Air Transport" };
    return (t >= 0 && t < (int)(sizeof(n) / sizeof(n[0]))) ? n[t] : "?";
}

// A message in the game's message log. The log keeps the char* it is given, so the text must
// outlive the call: a small ring of static buffers.
static void GameMessage(const char* fmt, ...) {
    static char bufs[8][96];
    static int  next = 0;
    char* b = bufs[next]; next = (next + 1) % 8;
    va_list ap; va_start(ap, fmt); vsnprintf(b, 96, fmt, ap); va_end(ap);
    ((MsgLogAdd_t)ADDR_MsgLogAdd)((void*)ADDR_MessageLog, -1, -1, b, 0x5d);   // no map anchor, sound 0x5d
}

// ---- new game -----------------------------------------------------------------------
// Called when the tick goes back or jumps ahead (a new game, or a save loaded). Unit numbers
// from the old game mean nothing now, so the markers are forgotten (not removed: that unit
// number may be anything in the new game).
static void ResetGame(const char* why) {
    memset(g_present, 0, sizeof(g_present));
    memset(g_tracks, 0, sizeof(g_tracks));
    g_trackHead = 0;
    g_baseline = true;
    g_activeLogged = false;
    g_mil = g_iniMil; g_col = g_iniCol;
    g_milMarker = g_colMarker = -1;
    g_milMarkerUntil = g_colMarkerUntil = 0;
    g_ralliedMil = g_ralliedCol = 0;
    g_selFactory = -1;
    Log("new game (%s): rally points back to the ini values", why);
}

// ---- the rally pass -------------------------------------------------------------------
// Remember a vehicle just sent to a rally point, for the UI feed (a ring of RL_MAX_TRACKS).
static void AddTrack(int unit, int type, int weapon, bool military, int factory, LOCATION destAbs, LOCATION now) {
    Track& tr = g_tracks[g_trackHead];
    g_trackHead = (g_trackHead + 1) % RL_MAX_TRACKS;
    memset(&tr, 0, sizeof(tr));
    tr.used = true;
    tr.destAbs = destAbs;
    tr.t.unitIndex = unit; tr.t.type = type; tr.t.weapon = weapon; tr.t.military = military;
    tr.t.factory = factory; tr.t.tick = *(int*)ADDR_GameTick;
    tr.t.destX = DispX(destAbs.x); tr.t.destY = DispY(destAbs.y);
    tr.t.x = DispX(now.x); tr.t.y = DispY(now.y);
    tr.t.state = RL_TRACK_MOVING;
}

// The local player's Vehicle/Arachnid Factory nearest to `at`, within FACTORY_REACH; -1 if none.
static int FactoryNear(int lp, LOCATION at, int* distOut) {
    char it[16] = {0};
    ((EnumCtor2_t)ADDR_PBE_ctor)(it, lp, 0);
    UnitHandle u;
    int best = -1, bestD = 1 << 30;
    while (((EnumNext_t)ADDR_PBE_GetNext)(it, &u)) {
        if (!IsFactory(UnitCall(ADDR_Unit_GetType, u.unitIndex))) continue;
        int d = Dist(UnitLoc(u.unitIndex), at);
        if (d < bestD) { bestD = d; best = u.unitIndex; }
    }
    *distOut = bestD;
    return bestD <= FACTORY_REACH ? best : -1;
}

// Every cycle: notice a new game, then send each vehicle that just came out of one of the
// local player's Vehicle/Arachnid Factories to its group's rally point (see the file header).
static void RallyTick() {
    int tick = *(int*)ADDR_GameTick;
    if (g_lastTick >= 0 && tick < g_lastTick)            ResetGame("tick went back");
    else if (g_lastTick >= 0 && tick > g_lastTick + 64)  ResetGame("tick jumped ahead");
    g_lastTick = tick;

    g_cycle++;
    bool quiet = g_baseline || tick < START_GRACE_TICKS;   // take what's there, move nothing
    if (!quiet && !g_activeLogged) {
        g_activeLogged = true;
        Log("rally active: new vehicles at a factory now drive to their rally point");
    }

    int lp = ((LocalPlayer_t)ADDR_LocalPlayer)();
    char it[16] = {0};
    ((EnumCtor1_t)ADDR_PVE_ctor)(it, lp);
    UnitHandle u;
    while (((EnumNext_t)ADDR_PVE_GetNext)(it, &u)) {
        int idx = u.unitIndex;
        if (idx <= 0 || idx >= MAX_UNIT_INDEX) continue;
        bool isNew = g_present[idx] != g_cycle - 1;      // not listed in the cycle before
        g_present[idx] = g_cycle;
        if (!isNew || quiet) continue;

        int type   = UnitCall(ADDR_Unit_GetType, idx);
        int weapon = UnitCall(ADDR_Unit_GetWeapon, idx);
        LOCATION at = UnitLoc(idx);
        int dist;
        int factory = FactoryNear(lp, at, &dist);
        if (factory < 0) {
            if (g_debug) Log("new %s #%d at (%d,%d) - not at a factory (nearest %d tiles), left alone",
                             VehName(type), idx, DispX(at.x), DispY(at.y), dist);
            continue;
        }
        bool military = IsMilitary(type);
        const Point& p = military ? g_mil : g_col;
        if (!p.set) {
            if (g_debug) Log("new %s #%d at factory #%d - no %s rally point set", VehName(type), idx, factory,
                             military ? "military" : "colony");
            continue;
        }
        UnitHandle h = { idx };
        ((DoMove_t)ADDR_Unit_DoMove)(&h, p.abs);
        if (military) g_ralliedMil++; else g_ralliedCol++;
        AddTrack(idx, type, weapon, military, factory, p.abs, at);
        Log("rally: %s #%d %s from factory #%d -> (%d,%d)", military ? "MIL" : "COL", idx, VehName(type),
            factory, DispX(p.abs.x), DispY(p.abs.y));
    }
    g_baseline = false;
}

// ---- markers --------------------------------------------------------------------------
// Markers are real (gaia) units, so single player only (see the header). Each rally point has
// at most one; it is removed at its `until` tick, which is pushed on while a factory is selected
// and MARKER_LIFETIME ticks on after the point is set.
static void RemoveMarker(int* markerIdx) {
    if (*markerIdx >= 0) {
        UnitHandle h = { *markerIdx };
        ((UnitFn_t)ADDR_Unit_DoDeath)(&h);
        *markerIdx = -1;
    }
}

// Make sure the marker for one rally point is up, and keep it until at least `until`.
static void ShowMarker(bool military, int until) {
    const Point& p = military ? g_mil : g_col;
    int*  mi     = military ? &g_milMarker       : &g_colMarker;
    int*  mu     = military ? &g_milMarkerUntil  : &g_colMarkerUntil;
    bool* failed = military ? &g_milMarkerFailed : &g_colMarkerFailed;
    if (g_marker <= 0 || !p.set) return;
    if (*mi < 0 && !*failed) {
        UnitHandle mh = { -1 };
        if (((PlaceMarker_t)ADDR_PlaceMarker)(&mh, p.abs.x, p.abs.y, g_marker - 1)) *mi = mh.unitIndex;
        else { *failed = true; Log("marker: could not place one at (%d,%d)", DispX(p.abs.x), DispY(p.abs.y)); }
    }
    if (until > *mu) *mu = until;
}

// ---- selection, hotkeys -----------------------------------------------------------------
// While a Vehicle/Arachnid Factory is selected, both rally points are marked on the map.
static void SelectionTick() {
    int count = *(int*)ADDR_SelCount;
    int sel = -1;
    if (count >= 1 && count <= 32) {
        int idx = *(int*)ADDR_SelArray;                  // the first selected unit
        if (idx > 0 && idx < MAX_UNIT_INDEX && UnitCall(ADDR_Unit_OwnerID, idx) == ((LocalPlayer_t)ADDR_LocalPlayer)() &&
            IsFactory(UnitCall(ADDR_Unit_GetType, idx)))
            sel = idx;
    }
    if (sel != g_selFactory && g_debug) {
        if (sel >= 0) Log("select: factory #%d", sel); else Log("select: no factory");
    }
    if (sel >= 0 && g_selFactory < 0) g_milMarkerFailed = g_colMarkerFailed = false;   // fresh selection: try again
    g_selFactory = sel;
    if (sel >= 0) {
        int until = *(int*)ADDR_GameTick + MARKER_LINGER;
        ShowMarker(true, until);
        ShowMarker(false, until);
    }
}

// [ or ] pressed: set that rally point to the tile under the mouse (only with a factory
// selected and the mouse over the map), confirm with a message and move its marker there.
static void SetRallyFromCursor(bool military) {
    if (g_selFactory < 0) return;                        // only with a factory selected
    const char* name = military ? "Military" : "Colony";
    if (*(int*)ADDR_HoverValid == 0) { Log("set %s ignored: mouse not over the map", name); return; }
    LOCATION a = { *(int*)ADDR_HoverPixelX >> 5, *(int*)ADDR_HoverPixelY >> 5 };
    int tw = *(int*)ADDR_MapTileWidth, th = *(int*)ADDR_MapTileHeight;
    if (a.x < 0 || a.x >= tw || a.y < 0 || a.y >= th) {
        Log("set %s ignored: tile (%d,%d) is off the map", name, DispX(a.x), DispY(a.y));
        return;
    }
    Point& p = military ? g_mil : g_col;
    p.set = true; p.abs = a; p.fromGame = true;
    Log("set %s rally point -> (%d,%d)", name, DispX(a.x), DispY(a.y));
    GameMessage("%s rally point set: %d, %d", name, DispX(a.x), DispY(a.y));

    // move this point's marker to the new tile
    RemoveMarker(military ? &g_milMarker : &g_colMarker);
    (military ? g_milMarkerFailed : g_colMarkerFailed) = false;
    ShowMarker(military, *(int*)ADDR_GameTick + MARKER_LIFETIME);
}

// Every cycle: react to [ / ] going down (edge-detected, so holding a key sets the point once).
static void HotkeyTick() {
    // only while the game window has focus (GetAsyncKeyState sees keys pressed in any window)
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool focus = pid == GetCurrentProcessId();
    bool m = focus && (GetAsyncKeyState(VK_SET_MILITARY) & 0x8000) != 0;
    bool c = focus && (GetAsyncKeyState(VK_SET_COLONY)   & 0x8000) != 0;
    if (m && !g_milKeyDown) SetRallyFromCursor(true);
    if (c && !g_colKeyDown) SetRallyFromCursor(false);
    g_milKeyDown = m; g_colKeyDown = c;
}

// Every cycle: remove a marker once its time is up.
static void MarkerTick() {
    int now = *(int*)ADDR_GameTick;
    if (g_milMarker >= 0 && now >= g_milMarkerUntil) RemoveMarker(&g_milMarker);
    if (g_colMarker >= 0 && now >= g_colMarkerUntil) RemoveMarker(&g_colMarker);
}

// ---- UI feed --------------------------------------------------------------------------
// Copy a rally point into the snapshot, in displayed tiles.
static void PutPoint(RlPoint& o, const Point& p) {
    o.set = p.set; o.fromGame = p.fromGame;
    o.x = p.set ? DispX(p.abs.x) : 0; o.y = p.set ? DispY(p.abs.y) : 0;
}

// Every RL_PUBLISH_EVERY cycles: write a snapshot for Rally-UI (rally points, selected factory,
// counts, factories, recent vehicles). Only reads game state; `seq` is odd while writing
// (see rl_shared.h). Rally-UI currently shows only the two points and the status line.
static void Publish() {
    if (!g_shm) return;
    if (--g_publishCountdown > 0) return;
    g_publishCountdown = RL_PUBLISH_EVERY;

    RlSnapshot* s = g_shm;
    int lp = ((LocalPlayer_t)ADDR_LocalPlayer)();
    InterlockedIncrement((volatile LONG*)&s->seq);       // odd: writing
    s->version     = RL_SHM_VERSION;
    s->gameRunning = 1;
    s->tick        = *(int*)ADDR_GameTick;
    s->localPlayer = lp;
    s->marker      = g_marker;
    PutPoint(s->military, g_mil);
    PutPoint(s->colony,   g_col);
    s->selectedFactory = g_selFactory;
    s->ralliedMilitary = g_ralliedMil;
    s->ralliedColony   = g_ralliedCol;

    int veh = 0;
    char it[16] = {0};
    UnitHandle u;
    ((EnumCtor1_t)ADDR_PVE_ctor)(it, lp);
    while (((EnumNext_t)ADDR_PVE_GetNext)(it, &u)) veh++;
    s->vehicles = veh;

    int n = 0;
    memset(it, 0, sizeof(it));
    ((EnumCtor2_t)ADDR_PBE_ctor)(it, lp, 0);
    while (n < RL_MAX_FACTORIES && ((EnumNext_t)ADDR_PBE_GetNext)(it, &u)) {
        int type = UnitCall(ADDR_Unit_GetType, u.unitIndex);
        if (!IsFactory(type)) continue;
        RlFactory& f = s->factories[n++];
        LOCATION l = UnitLoc(u.unitIndex);
        f.unitIndex = u.unitIndex; f.type = type; f.x = DispX(l.x); f.y = DispY(l.y);
        f.busy = UnitCall(ADDR_Unit_IsBusy, u.unitIndex) != 0;
    }
    s->numFactories = n;

    // the tracked vehicles, newest first, with where they are now
    n = 0;
    for (int k = 1; k <= RL_MAX_TRACKS; k++) {
        Track& tr = g_tracks[(g_trackHead - k + RL_MAX_TRACKS) % RL_MAX_TRACKS];
        if (!tr.used) continue;
        if (tr.t.state != RL_TRACK_GONE) {
            int idx = tr.t.unitIndex;
            if (UnitLive(idx) && UnitCall(ADDR_Unit_GetType, idx) == tr.t.type && UnitCall(ADDR_Unit_OwnerID, idx) == lp) {
                LOCATION l = UnitLoc(idx);
                tr.t.x = DispX(l.x); tr.t.y = DispY(l.y);
                if (Dist(l, tr.destAbs) <= 1) tr.t.state = RL_TRACK_ARRIVED;
            } else tr.t.state = RL_TRACK_GONE;
        }
        s->tracks[n++] = tr.t;
    }
    s->numTracks = n;
    InterlockedIncrement((volatile LONG*)&s->seq);       // even: done
}

// Create the named mapping the UI reads. Failure only disables the UI feed.
static void OpenSharedMemory() {
    g_shmHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(RlSnapshot), RL_SHM_NAME);
    if (!g_shmHandle) { Log("ui feed: CreateFileMapping failed (%lu) - no UI data", GetLastError()); return; }
    g_shm = (RlSnapshot*)MapViewOfFile(g_shmHandle, FILE_MAP_WRITE, 0, 0, sizeof(RlSnapshot));
    if (!g_shm) { Log("ui feed: MapViewOfFile failed (%lu) - no UI data", GetLastError()); CloseHandle(g_shmHandle); g_shmHandle = NULL; return; }
    memset(g_shm, 0, sizeof(RlSnapshot));
    g_shm->version = RL_SHM_VERSION;
    g_shm->gamePid = (int32_t)GetCurrentProcessId();      // lets the UI quit when the game does
    Log("ui feed: shared memory \"%s\" ready (%u bytes)", RL_SHM_NAME, (unsigned)sizeof(RlSnapshot));
}

// ---- fault guard ----------------------------------------------------------------------
// A fault in a pass is logged (which pass, the address as Rally.dll+offset or engine) and
// swallowed, so the game keeps running. After FAULT_LIMIT faults the module stops for the
// session rather than keep faulting.
static int FaultFilter(const char* where, EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    uintptr_t eip = ep->ContextRecord->Eip, base = (uintptr_t)g_hSelf, size = 0x100000;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_hSelf;
    if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE)
        size = ((IMAGE_NT_HEADERS*)(base + dos->e_lfanew))->OptionalHeader.SizeOfImage;
    bool inUs = eip >= base && eip < base + size;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        Log("*** FAULT in %s: access violation (%s 0x%08X) at %s0x%X", where,
            er->ExceptionInformation[0] ? "write" : "read", (unsigned)er->ExceptionInformation[1],
            inUs ? "Rally.dll+" : "", (unsigned)(inUs ? eip - base : eip));
    else
        Log("*** FAULT in %s: code 0x%08X at %s0x%X", where, (unsigned)er->ExceptionCode,
            inUs ? "Rally.dll+" : "", (unsigned)(inUs ? eip - base : eip));
    return EXCEPTION_EXECUTE_HANDLER;
}

// Run one per-cycle pass behind an SEH guard.
static void SafeRun(const char* name, void (*fn)()) {
    if (g_stopped) return;
    __try { fn(); }
    __except (FaultFilter(name, GetExceptionInformation())) {
        if (++g_faults >= FAULT_LIMIT) {
            g_stopped = true;
            if (g_shm) {                                 // leave the snapshot readable, mark the feed stopped
                if (g_shm->seq & 1) InterlockedIncrement((volatile LONG*)&g_shm->seq);
                g_shm->gameRunning = 0;
                g_shm = NULL;
            }
            Log("%d faults - Rally stopped for this session", g_faults);
        }
    }
}

// Faults outside the passes: log them, then hand on to whatever filter was there before
// (OPU's own crash handling), so the game's behaviour doesn't change.
static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    FaultFilter("(unhandled)", ep);
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// ---- the game-cycle hook --------------------------------------------------------------
// Stands in for ProcessGameCycle at its call site. `self` is the ECX the game passed; it goes
// on unchanged to the next hook in the chain (or the engine's function).
static void __fastcall HookedProcessGameCycle(void* self, void* /*edx*/) {
    g_nextCycle(self);
    SafeRun("RallyTick",     RallyTick);
    SafeRun("SelectionTick", SelectionTick);
    SafeRun("HotkeyTick",    HotkeyTick);
    SafeRun("MarkerTick",    MarkerTick);
    SafeRun("Publish",       Publish);
}

// Point the `call ProcessGameCycle` at 0x0049C4BD at our hook, remembering what it called
// before (the engine's function, or another module's chained hook). Checks the CALL byte first.
static bool HookGameCycle() {
    BYTE* site = (BYTE*)CALLSITE_PGC;
    if (site[0] != 0xE8) { Log("hook: no CALL at 0x%08X (byte %02X) - not installed", (unsigned)CALLSITE_PGC, site[0]); return false; }
    uintptr_t cur = CALLSITE_PGC + 5 + *(int*)(site + 1);
    g_nextCycle = (ProcGameCycle_t)cur;
    DWORD op;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &op)) { Log("hook: VirtualProtect failed on the cycle call"); return false; }
    *(int*)(site + 1) = (int)((uintptr_t)&HookedProcessGameCycle - (CALLSITE_PGC + 5));
    VirtualProtect(site + 1, 4, op, &op);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    Log("hook: game cycle call @ 0x%08X -> Rally (chains to 0x%08X%s)",
        (unsigned)CALLSITE_PGC, (unsigned)cur, cur == ADDR_ProcessGameCycle ? ", the engine" : ", another module");
    return true;
}

// ---- settings -------------------------------------------------------------------------
// A displayed-tile pair from the ini; not set if either key is missing or negative.
static Point ReadPoint(const char* sec, const char* kx, const char* ky, const char* ini) {
    Point p = { false, { 0, 0 }, false };
    int x = (int)GetPrivateProfileIntA(sec, kx, -1, ini);
    int y = (int)GetPrivateProfileIntA(sec, ky, -1, ini);
    if (x >= 0 && y >= 0) { p.set = true; p.abs.x = x - DISP_DX; p.abs.y = y - DISP_DY; }
    return p;
}

// ---- op2ext entry points --------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hSelf = (HMODULE)hInst; DisableThreadLibraryCalls(hInst); }
    return TRUE;
}

// op2ext calls this once at load with the module's ini section name ("Rally").
EXPORT void InitMod(char* iniSectionName) {
    if (g_installed) return;
    g_installed = true;

    const char* sec = (iniSectionName && *iniSectionName) ? iniSectionName : "Rally";
    char ini[MAX_PATH];
    snprintf(ini, MAX_PATH, "%s\\outpost2.ini", GameDir());
    int enabled = GetPrivateProfileIntA(sec, "Enabled", 1, ini);
    g_marker = GetPrivateProfileIntA(sec, "Marker", 1, ini);
    if (g_marker < 0 || g_marker > 3) g_marker = 1;
    g_debug  = GetPrivateProfileIntA(sec, "Debug", 0, ini) != 0;
    g_iniMil = ReadPoint(sec, "RallyMilitaryX", "RallyMilitaryY", ini);
    g_iniCol = ReadPoint(sec, "RallyColonyX",   "RallyColonyY",   ini);
    g_mil = g_iniMil; g_col = g_iniCol;

    char m[32], c[32];
    if (g_mil.set) snprintf(m, sizeof(m), "(%d,%d)", DispX(g_mil.abs.x), DispY(g_mil.abs.y)); else strcpy(m, "not set");
    if (g_col.set) snprintf(c, sizeof(c), "(%d,%d)", DispX(g_col.abs.x), DispY(g_col.abs.y)); else strcpy(c, "not set");
    Log("=== Rally start (section [%s], Enabled=%d, Marker=%d, Debug=%d, military %s, colony %s) ===",
        sec, enabled, g_marker, (int)g_debug, m, c);
    if (!enabled) { Log("DISABLED: Enabled=0 - nothing patched"); return; }

    g_prevFilter = SetUnhandledExceptionFilter(UnhandledFilter);
    if (HookGameCycle()) OpenSharedMemory();
}

// The patch is left in place at shutdown: the game is exiting, and another module may have
// chained onto it. The UI is told the game is gone.
EXPORT bool DestroyMod() {
    if (g_shm) {
        InterlockedIncrement((volatile LONG*)&g_shm->seq);
        g_shm->gameRunning = 0;
        InterlockedIncrement((volatile LONG*)&g_shm->seq);
    }
    Log("=== Rally stop ===");
    return true;
}
