// autorepair.cpp  -  AutoRepair: idle ConVecs repair your buildings, Repair Vehicles and Spiders your vehicles
// ================================================================================================
// An op2ext module for Outpost 2 OPU 1.4.1 (Outpost2.exe, fixed image base 0x00400000).
// Every address below was checked against the OPU 1.4.1 exe.
//
// WHAT THE MOD DOES
// -----------------
// Every Interval game cycles (default 100) it looks at the local player's units:
//   - ConVec=1:        each building with at least MinDamage damage that nobody is repairing gets
//                      the NEAREST idle, empty ConVec.
//   - RepairVehicle=1: each vehicle BELOW 50% health (a Repair Vehicle only repairs up to half a
//                      vehicle's hit points) that nobody is repairing gets the NEAREST idle
//                      Repair Vehicle (never itself).
//   - Spider=1:        Spiders repair vehicles too, the same way and under the same 50% rule; they
//                      share one pool with the Repair Vehicles, so the nearest of either is sent.
// One worker per damaged unit. It drives there and repairs it, exactly as if you had ordered it.
//
// HOW IT WORKS
// ------------
//   - Buildings: PlayerBuildingEnum(player, 0 = all). Damage is the short at Unit+0x1E
//     (hit points = max HP - damage); max HP comes from the unit type table (see MaxHP()).
//   - Vehicles: PlayerVehicleEnum - the damaged ones, and the workers. "Idle" = Unit::IsBusy() == 0
//     (for a ConVec also no cargo, so one carrying a structure kit is never taken). A ConVec or
//     Repair Vehicle or Spider whose current command is ctMoRepairObj (Unit+0x21 == 0x1C) is repairing the
//     unit at Unit+0x48; that unit gets no second worker.
//   - The order is a ctMoRepairObj command packet sent through Player::ProcessCommandPacket
//     (0x0040E300), the same path the game's own orders take (layout from the mission SDK's
//     HFL UnitEx::DoRepair; see IssueRepair()).
//   - A repair that ends without lowering the unit's damage, where the worker GOT THERE: the game
//     refused it (a building with only a little damage, or not enough ore - e.g. a Tiger needs rare
//     ore), so the unit is left alone for a while - RETRY_TICKS (10 marks) for a building,
//     VEHICLE_RETRY_TICKS (1 mark) for a vehicle, which may just have been cut short by a fight.
//     If the worker never got there (you moved the unit) one is sent again at the next check.
//   - Which worker: the damaged units are taken in the order the game lists them, and each gets
//     the nearest worker still free in that pass (see Dispatch()).
//   - A new game or a loaded save is noticed by the game tick going back or jumping ahead;
//     the per-game counters and bookkeeping start again.
//
// UI FEED (for ui\AutoRepair-UI.exe)
// ------------------------------------------------
// Every AR_PUBLISH_EVERY cycles Publish() writes a snapshot (the three switches, ConVec, Repair
// Vehicle and Spider counts, the damaged units with their health and the worker on each, time to the next
// pass) into the named shared memory "Local\OP2AutoRepair" (layout and protocol: ar_shared.h).
// It only reads game state; if the mapping can't be created the module works as before, without
// the feed.
//
// MULTIPLAYER
// -----------
// The repair orders are ordinary commands for the player's own ConVecs, Repair Vehicles and Spiders, so
// they should stay in sync (not yet tested in a real multiplayer game). DebugDamage writes a
// building's damage directly and is single-player only.
//
// SHARING THE GAME-CYCLE HOOK
// ---------------------------
// Other op2ext modules (BuildQueue, for one) patch the same call (0x0049C4BD). They all CHAIN:
// each reads whatever the call points at (the engine's function, or another module's hook) and
// calls that, so they run together in any load order.
//
// INSTALL (OPU 1.4.1, in OPU\outpost2.ini, with AutoRepair.dll in the OPU folder)
// ------------------------------------------------------------------------------
//     [ExternalModules]  AutoRepair=yes     ; OPU's loader: must be yes/no
//     [AutoRepair]       Dll=AutoRepair.dll
//                        Enabled=1          ; 1 = on, 0 = the DLL loads but patches nothing
//                        ConVec=1           ; 1 = ConVecs repair damaged buildings
//                        RepairVehicle=1    ; 1 = Repair Vehicles repair vehicles below 50% health
//                        Spider=1           ; 1 = Spiders repair vehicles below 50% health too
//                        Interval=100       ; game cycles between repair passes (1 mark = 100)
//                        MinDamage=50       ; repair a unit once it has this much damage (Tokamaks
//                                           ;   decay a little all the time - this ignores that)
//                        DebugDamage=0      ; test only: >0 damages one Tokamak by this much once per game
// Settings are read once at startup. Log: <game dir>\logs\AutoRepair.log (OPU\logs\AutoRepair.log).
//
// Build: build.bat (32-bit DLL).

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "ar_shared.h"   // the snapshot layout shared with AutoRepair-UI.exe

#define EXPORT extern "C" __declspec(dllexport)

// ---- engine addresses (OPU 1.4.1 Outpost2.exe) --------------------------------------
static const uintptr_t ADDR_ProcessGameCycle = 0x00489EA0;  // TethysGame::ProcessGameCycle(), __thiscall
static const uintptr_t CALLSITE_PGC          = 0x0049C4BD;  // the one `call ProcessGameCycle` (E8 rel32)
static const uintptr_t ADDR_ProcessCommand   = 0x0040E300;  // Player::ProcessCommandPacket(CommandPacket*), __thiscall
static const uintptr_t ADDR_LocalPlayer      = 0x00477F10;  // static int TethysGame::LocalPlayer()     (exported)
static const uintptr_t ADDR_PVE_ctor         = 0x0047A480;  // PlayerVehicleEnum(int player)           (exported)
static const uintptr_t ADDR_PVE_GetNext      = 0x0047A4B0;  // int PlayerVehicleEnum::GetNext(Unit&)   (exported)
static const uintptr_t ADDR_PBE_ctor         = 0x0047A4E0;  // PlayerBuildingEnum(int player, map_id)  (exported)
static const uintptr_t ADDR_PBE_GetNext      = 0x0047A510;  // int PlayerBuildingEnum::GetNext(Unit&)  (exported)
static const uintptr_t ADDR_Unit_IsBusy      = 0x00475E60;  // int Unit::IsBusy() const
static const uintptr_t ADDR_Unit_GetType     = 0x00475EE0;  // map_id Unit::GetType() const
static const uintptr_t ADDR_Unit_GetCargo    = 0x00475F00;  // map_id Unit::GetCargo() const
static const uintptr_t ADDR_Unit_SetDamage   = 0x00476200;  // void Unit::SetDamage(int)   (DebugDamage only)
static const uintptr_t ADDR_GameTick         = 0x0056EB1C;  // current game tick
static const uintptr_t UNIT_POOL_PTR         = 0x0054F848;  // holds a POINTER to the unit pool (stride 0x78)
static const uintptr_t UNIT_STRIDE           = 0x78;
static const uintptr_t TYPE_TABLE            = 0x004E1348;  // MapObjectType* per map_id (static table)
static const int       NUM_TYPES             = 115;         //   entries (0x4E1348 .. 0x4E1514)
static const uintptr_t PLAYER_ARRAY          = 0x0056EF1C;  // Player objects, stride 0xC24
static const uintptr_t PLAYER_STRIDE         = 0x0C24;

// Unit object fields (offsets into a unit's 0x78-byte record), read directly where no exported
// Unit method gives them:
//   +0x14 / +0x18  pixel X / Y (tile = pixel >> 5)
//   +0x1D          low 4 bits owner, high 4 bits creator (whose unit stats it uses)
//   +0x1E          short damage
//   +0x21          current command (0x1C = ctMoRepairObj)
//   +0x48          repair target's unit number (while repairing)
// MapObjectType: +0x08 per-player stats [7], 0x44 bytes each, hit points first.

static const int MAP_CONVEC         = 2;      // map_ids (vehicles are 1..16, buildings 21..58)
static const int MAP_SPIDER         = 3;
static const int MAP_REPAIR_VEHICLE = 14;
static const int MAP_TOKAMAK        = 58;
static const int CT_REPAIR      = 0x1C;         // ctMoRepairObj: the "repair this unit" command
static const int DISP_DX = -31, DISP_DY = +1;   // displayed tile = absolute tile + (DISP_DX, DISP_DY)
static const int MAX_LIST       = 256;          // damaged units / workers handled per pass (each list)
static const int MAX_UNIT_INDEX = 4096;         // size of the per-unit bookkeeping arrays
static const int RETRY_TICKS    = 1000;         // a repair that reached a building but didn't help: leave it 10 marks
static const int VEHICLE_RETRY_TICKS = 100;     //   ... reached a vehicle but didn't help: leave it 1 mark
static const int REACHED_PIXELS = 3 * 32;       // a worker within 3 tiles of its target got there
static const int FAULT_LIMIT    = 10;           // after this many caught faults the module stops

// ---- calling conventions ------------------------------------------------------------
// The exported Unit::* methods take ECX = a Unit object, which is just { int unitIndex }; the
// enumerators are small objects built in place by their constructor (16 bytes is plenty).
struct UnitHandle { int unitIndex; };
typedef int  (__fastcall *LocalPlayer_t)();
typedef void (__thiscall *EnumCtor1_t)(void* self, int playerNum);
typedef void (__thiscall *EnumCtor2_t)(void* self, int playerNum, int mapIdFilter);
typedef int  (__thiscall *EnumNext_t)(void* self, UnitHandle* outUnit);
typedef int  (__thiscall *UnitFn_t)(UnitHandle* self);
typedef void (__thiscall *SetDamage_t)(UnitHandle* self, int dmg);
// ProcessCommandPacket is __thiscall (ECX = the Player object). A __fastcall function with a
// dummy EDX argument has the same shape. (Calling it as __stdcall leaves garbage in ECX and
// crashes.)
typedef int  (__fastcall *ProcCmd_t)(void* player, int edxDummy, void* packet);
typedef void (__thiscall *ProcGameCycle_t)(void* self);

// ---- settings and state -------------------------------------------------------------
static int  g_interval    = 100;   // [AutoRepair] Interval
static int  g_minDamage   = 50;    // [AutoRepair] MinDamage
static int  g_debugDamage = 0;     // [AutoRepair] DebugDamage
static bool g_useConVec        = true;   // [AutoRepair] ConVec: ConVecs repair damaged buildings
static bool g_useRepairVehicle = true;   // [AutoRepair] RepairVehicle: Repair Vehicles repair damaged vehicles
static bool g_useSpider        = true;   // [AutoRepair] Spider: Spiders repair damaged vehicles

static int  g_countdown   = 0;     // cycles to the next pass
static int  g_lastTick    = -1;    // to notice a new game / loaded save
static bool g_listedOnce  = false; // the full building list is logged once per game
static bool g_dbgApplied  = false; // DebugDamage done for this game
static int  g_repairsSent = 0;     // repair orders sent this game (for the UI)

// Per damaged unit (by unit number): the last repair sent - the worker and the damage then. When
// that repair is over (nobody is on the unit) and its damage didn't go down, Dispatch() decides:
//   - the worker got to it: the game refused the repair (a building with only a little damage, or
//     not enough ore - each refusal shows "Repairs incomplete"), so leave it alone until
//     g_retryAt instead of sending workers to it over and over (10 marks for a building, 1 mark
//     for a vehicle);
//   - the worker never got there (you moved the unit or gave the worker other orders): send one
//     again at the next check.
static int  g_sentTick[MAX_UNIT_INDEX];     // tick + 1 of the last repair sent, 0 = none outstanding
static int  g_sentDamage[MAX_UNIT_INDEX];   // its damage when that repair was sent
static int  g_sentWorker[MAX_UNIT_INDEX];   // the ConVec / Repair Vehicle / Spider that was sent
static int  g_retryAt[MAX_UNIT_INDEX];      // tick before which the unit is left alone (0 = not)

static HMODULE         g_hSelf     = NULL;   // this DLL (for fault addresses)
static ProcGameCycle_t g_nextCycle = NULL;   // what the cycle call pointed at before us
static bool            g_installed = false;  // InitMod ran
static bool            g_stopped   = false;  // too many faults: the passes no longer run
static int             g_faults    = 0;
static char            g_gameDir[MAX_PATH];  // see GameDir()
static HANDLE          g_shmHandle = NULL;   // the UI feed's file mapping
static ArSnapshot*     g_shm       = NULL;   //   and its view (NULL = no feed)
static int             g_publishCountdown = 0;

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

// Append one line to <game dir>\logs\AutoRepair.log, with the time and game tick. Opened and
// closed per line so the log survives a crash.
static void Log(const char* fmt, ...) {
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\logs", GameDir());
    CreateDirectoryA(path, NULL);
    snprintf(path, MAX_PATH, "%s\\logs\\AutoRepair.log", GameDir());
    FILE* f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d tick %d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            *(int*)ADDR_GameTick);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// ---- unit helpers -------------------------------------------------------------------
// Call an exported `int Unit::X() const` method on unit `unitIndex`.
static int UnitCall(uintptr_t fn, int unitIndex) {
    UnitHandle h = { unitIndex };
    return ((UnitFn_t)fn)(&h);
}
// The unit's record in the pool. 0x0054F848 holds a pointer to the pool, so it is dereferenced.
static uintptr_t UnitObj(int index) { return *(uintptr_t*)UNIT_POOL_PTR + (uintptr_t)index * UNIT_STRIDE; }
static int  Damage(int idx)  { return *(short*)(UnitObj(idx) + 0x1E); }
static int  PixX(int idx)    { return *(int*)(UnitObj(idx) + 0x14); }
static int  PixY(int idx)    { return *(int*)(UnitObj(idx) + 0x18); }
static int  Command(int idx) { return *(unsigned char*)(UnitObj(idx) + 0x21); }
static int  RepairTarget(int idx) { return (short)*(unsigned*)(UnitObj(idx) + 0x48); }   // valid while Command == CT_REPAIR

// Full hit points: the unit type's stats for the player who built it (the creator - a captured
// unit keeps its builder's stats). 0 if unknown.
static int MaxHP(int idx, int type) {
    if (type < 0 || type >= NUM_TYPES) return 0;
    uintptr_t t = ((uintptr_t*)TYPE_TABLE)[type];
    if (!t) return 0;
    int creator = (*(unsigned char*)(UnitObj(idx) + 0x1D) >> 4) & 0xF;
    if (creator > 6) return 0;
    return *(int*)(t + 0x08 + creator * 0x44);
}

// Can a Repair Vehicle (or Spider) still help this vehicle? They only bring a vehicle back up to
// half its hit points, so only a vehicle below 50% health (and with at least MinDamage) counts.
static bool VehicleNeedsRepair(int idx) {
    int dmg = Damage(idx);
    if (dmg < g_minDamage) return false;
    int max = MaxHP(idx, UnitCall(ADDR_Unit_GetType, idx));
    return max <= 0 || (max - dmg) * 2 < max;
}

// A readable name for a map_id, for the log.
static const char* UnitName(int t) {
    static const char* v[] = { "None", "Cargo Truck", "ConVec", "Spider", "Scorpion", "Lynx", "Panther", "Tiger",
                               "Robo-Surveyor", "Robo-Miner", "GeoCon", "Scout", "Robo-Dozer", "Evac Transport",
                               "Repair Vehicle", "Earthworker", "Small Air Transport" };
    if (t >= 0 && t <= 16) return v[t];
    static const char* n[] = {
        "Common Ore Mine", "Rare Ore Mine", "Guard Post", "Light Tower", "Common Storage",      // 21-25
        "Rare Storage", "Forum", "Command Center", "MHD Generator", "Residence",                 // 26-30
        "Robot Command", "Trade Center", "Basic Lab", "Medical Center", "Nursery",               // 31-35
        "Solar Power Array", "Recreation Facility", "University", "Agridome", "DIRT",            // 36-40
        "Garage", "Magma Well", "Meteor Defense", "Geothermal Plant", "Arachnid Factory",        // 41-45
        "Consumer Factory", "Structure Factory", "Vehicle Factory", "Standard Lab",              // 46-49
        "Advanced Lab", "Observatory", "Reinforced Residence", "Advanced Residence",             // 50-53
        "Common Ore Smelter", "Spaceport", "Rare Ore Smelter", "GORF", "Tokamak" };              // 54-58
    return (t >= 21 && t <= 58) ? n[t - 21] : "?";
}

// ---- the repair order -----------------------------------------------------------------
// A ctMoRepairObj command packet: worker `convec` (a ConVec, Repair Vehicle or Spider) repairs unit
// `target`. Sent through Player::ProcessCommandPacket for the worker's owner, the same path the
// game's own orders take (layout from HFL UnitEx::DoRepair):
//   CommandPacket { int type@0; short dataLength@4; int timeStamp@6; int unknown@0xA; data@0xE }
//   cmdRepair     { char numUnits=1; short unitId; short unknown1=0; short unitToRepair; short unknown2=-1 }
static void IssueRepair(int convec, int target) {
    uintptr_t obj = UnitObj(convec);
    if (*(int*)(obj + 4) == -1) return;                  // dead (not in a list)
    if (*(unsigned*)(obj + 0x44) & 0x20000) return;      // dying
    int owner = *(unsigned char*)(obj + 0x1D) & 0xF;

    unsigned char packet[40] = {0};
    *(int*)(packet + 0)   = CT_REPAIR;
    *(short*)(packet + 4) = 9;                           // dataLength = sizeof(cmdRepair)
    unsigned char* d = packet + 0x0E;
    d[0] = 1;                                            // numUnits
    *(short*)(d + 1) = (short)convec;
    *(short*)(d + 3) = 0;
    *(short*)(d + 5) = (short)target;
    *(short*)(d + 7) = (short)-1;
    ((ProcCmd_t)ADDR_ProcessCommand)((void*)(PLAYER_ARRAY + (uintptr_t)owner * PLAYER_STRIDE), 0, packet);
}

// ---- the repair pass ------------------------------------------------------------------
// A unit that no longer needs repair (repaired, or never damaged): clear its bookkeeping.
static void ForgetRepairs(int idx) {
    if (idx > 0 && idx < MAX_UNIT_INDEX) g_sentTick[idx] = g_retryAt[idx] = 0;
}

struct PassStats { int sent, already, waiting; };   // for the per-pass log line

// Each damaged unit in `targets` (in that order) that nobody is repairing - and that isn't being
// left alone after a repair that didn't help - gets the nearest still-unused worker from `idle`
// (used workers are set to -1). A worker never repairs itself. `busyTarget` lists the units
// some worker is already repairing. The log names each worker by its own type.
static void Dispatch(const int* targets, int nTargets, int* idle, int nIdle, const int* busyTarget, int nBusy,
                     PassStats* st) {
    int tick = *(int*)ADDR_GameTick;
    for (int i = 0; i < nTargets; i++) {
        int b = targets[i];
        if (b <= 0 || b >= MAX_UNIT_INDEX) continue;
        bool taken = false;
        for (int k = 0; k < nBusy; k++) if (busyTarget[k] == b) { taken = true; break; }
        if (taken) { st->already++; continue; }
        if (tick < g_retryAt[b]) { st->waiting++; continue; }
        if (g_sentTick[b] && Damage(b) >= g_sentDamage[b]) {
            // the last repair is over (nobody is on it) and the damage didn't go down
            g_sentTick[b] = 0;
            int w = g_sentWorker[b];
            long long dx = PixX(b) - PixX(w), dy = PixY(b) - PixY(w);
            bool reached = dx * dx + dy * dy <= (long long)REACHED_PIXELS * REACHED_PIXELS;
            // The worker got there, so the game refused the repair (too little damage on a
            // building, or not enough ore). Wait before trying again: 10 marks for a building, only
            // 1 mark for a vehicle (its repair may just have been cut short by a fight) - this also
            // stops a "Repairs incomplete" message every check while you have no ore.
            bool building = UnitCall(ADDR_Unit_GetType, b) >= 21;
            if (reached) {
                int wait = building ? RETRY_TICKS : VEHICLE_RETRY_TICKS;
                g_retryAt[b] = tick + wait;
                st->waiting++;
                Log("skip: %s #%d - #%d reached it but it is still at damage %d; trying again in %d mark%s",
                    UnitName(UnitCall(ADDR_Unit_GetType, b)), b, w, Damage(b), wait / 100, wait == 100 ? "" : "s");
                continue;
            }
            Log("resend: %s #%d - the repair by #%d ended without lowering its damage (never got there)",
                UnitName(UnitCall(ADDR_Unit_GetType, b)), b, w);
        }
        int best = -1; long long bestD2 = 0;
        for (int j = 0; j < nIdle; j++) {
            if (idle[j] < 0 || idle[j] == b) continue;
            long long dx = PixX(b) - PixX(idle[j]), dy = PixY(b) - PixY(idle[j]);
            long long d2 = dx * dx + dy * dy;
            if (best < 0 || d2 < bestD2) { best = j; bestD2 = d2; }
        }
        if (best < 0) continue;                          // no idle worker left (or only the unit itself)
        int w = idle[best];
        idle[best] = -1;
        IssueRepair(w, b);
        st->sent++; g_repairsSent++;
        g_sentTick[b] = tick + 1; g_sentDamage[b] = Damage(b); g_sentWorker[b] = w;
        int t = UnitCall(ADDR_Unit_GetType, b);
        int tiles = 0; while ((long long)(tiles + 1) * (tiles + 1) * 1024 <= bestD2) tiles++;   // sqrt / 32
        Log("repair: %s #%d -> %s #%d (damage %d / %d, ~%d tiles away)", UnitName(UnitCall(ADDR_Unit_GetType, w)), w, UnitName(t), b,
            Damage(b), MaxHP(b, t), tiles);
    }
}

// One check, every Interval cycles: list the damaged units and the free workers, then dispatch
// ConVecs to buildings, and Repair Vehicles and Spiders (one pool) to vehicles - each only if
// switched on.
static void RepairPass() {
    int lp = ((LocalPlayer_t)ADDR_LocalPlayer)();

    // DebugDamage: once per game, damage a Tokamak so detection + repair can be watched
    // without a fight. Single player only (a direct write).
    if (g_debugDamage > 0 && !g_dbgApplied) {
        g_dbgApplied = true;
        char it[16] = {0};
        ((EnumCtor2_t)ADDR_PBE_ctor)(it, lp, 0);
        UnitHandle u;
        while (((EnumNext_t)ADDR_PBE_GetNext)(it, &u)) {
            if (UnitCall(ADDR_Unit_GetType, u.unitIndex) != MAP_TOKAMAK) continue;
            ((SetDamage_t)ADDR_Unit_SetDamage)(&u, g_debugDamage);
            Log("DebugDamage: Tokamak #%d damage set to %d (reads back %d)", u.unitIndex, g_debugDamage, Damage(u.unitIndex));
            break;
        }
    }

    // damaged buildings (for ConVecs) and damaged vehicles (for Repair Vehicles / Spiders)
    static int dmgB[MAX_LIST], dmgV[MAX_LIST];
    int nB = 0, nV = 0, nBuildings = 0;
    bool full = !g_listedOnce;
    if (full) Log("--- buildings (player %d) ---", lp);
    char it[16] = {0};
    UnitHandle u;
    ((EnumCtor2_t)ADDR_PBE_ctor)(it, lp, 0);
    while (((EnumNext_t)ADDR_PBE_GetNext)(it, &u)) {
        nBuildings++;
        int dmg = Damage(u.unitIndex);
        if (full) {
            int t = UnitCall(ADDR_Unit_GetType, u.unitIndex);
            Log("  %-20s #%-4d damage %d / %d", UnitName(t), u.unitIndex, dmg, MaxHP(u.unitIndex, t));
        }
        if (dmg >= g_minDamage) { if (g_useConVec && nB < MAX_LIST) dmgB[nB++] = u.unitIndex; }
        else ForgetRepairs(u.unitIndex);
    }
    g_listedOnce = true;

    // vehicles: damaged ones, idle ConVecs / Repair Vehicles / Spiders, and what is already being repaired
    static int idleC[MAX_LIST], idleR[MAX_LIST], busyTarget[MAX_LIST];
    int nIdleC = 0, nIdleR = 0, nIdleS = 0, nBusy = 0;   // idleR holds Repair Vehicles and Spiders
    memset(it, 0, sizeof(it));
    ((EnumCtor1_t)ADDR_PVE_ctor)(it, lp);
    while (((EnumNext_t)ADDR_PVE_GetNext)(it, &u)) {
        int idx = u.unitIndex;
        int type = UnitCall(ADDR_Unit_GetType, idx);
        if (VehicleNeedsRepair(idx)) { if ((g_useRepairVehicle || g_useSpider) && nV < MAX_LIST) dmgV[nV++] = idx; }
        else ForgetRepairs(idx);
        if (type != MAP_CONVEC && type != MAP_REPAIR_VEHICLE && type != MAP_SPIDER) continue;
        if (Command(idx) == CT_REPAIR) { if (nBusy < MAX_LIST) busyTarget[nBusy++] = RepairTarget(idx); continue; }
        if (UnitCall(ADDR_Unit_IsBusy, idx)) continue;
        if (type == MAP_CONVEC) { if (g_useConVec && UnitCall(ADDR_Unit_GetCargo, idx) == 0 && nIdleC < MAX_LIST) idleC[nIdleC++] = idx; }
        else if (type == MAP_REPAIR_VEHICLE) { if (g_useRepairVehicle && nIdleR < MAX_LIST) idleR[nIdleR++] = idx; }
        else { if (g_useSpider && nIdleR < MAX_LIST) { idleR[nIdleR++] = idx; nIdleS++; } }   // Spider
    }

    PassStats st = {};
    Dispatch(dmgB, nB, idleC, nIdleC, busyTarget, nBusy, &st);
    Dispatch(dmgV, nV, idleR, nIdleR, busyTarget, nBusy, &st);
    if (nB || nV || st.sent)
        Log("pass: %d damaged buildings, %d damaged vehicles (%d being repaired, %d left alone for now), "
            "%d idle ConVecs, %d idle Repair Vehicles, %d idle Spiders, %d sent", nB, nV, st.already, st.waiting,
            nIdleC, nIdleR - nIdleS, nIdleS, st.sent);
}

// ---- UI feed --------------------------------------------------------------------------
// Every AR_PUBLISH_EVERY cycles: write a snapshot for AutoRepair-UI. Only reads game state. The
// seq counter is odd while writing (see ar_shared.h), so the UI never keeps a half-written copy.
static void Publish() {
    if (!g_shm) return;
    if (--g_publishCountdown > 0) return;
    g_publishCountdown = AR_PUBLISH_EVERY;

    ArSnapshot* s = g_shm;
    int lp = ((LocalPlayer_t)ADDR_LocalPlayer)();
    InterlockedIncrement((volatile LONG*)&s->seq);       // odd: writing
    s->version     = AR_SHM_VERSION;
    s->gameRunning = 1;
    s->tick        = *(int*)ADDR_GameTick;
    s->localPlayer = lp;
    s->interval    = g_interval;
    s->minDamage   = g_minDamage;
    s->nextPassIn  = g_countdown;
    s->repairsSent = g_repairsSent;

    s->useConVec        = g_useConVec;
    s->useRepairVehicle = g_useRepairVehicle;
    s->useSpider        = g_useSpider;

    // ConVecs / Repair Vehicles / Spiders (and who is repairing what)
    static int repairer[MAX_LIST], repTarget[MAX_LIST];
    int nRep = 0, convecs = 0, cIdle = 0, cRep = 0, rvs = 0, rIdle = 0, rRep = 0, sps = 0, sIdle = 0, sRep = 0;
    char it[16] = {0};
    UnitHandle u;
    ((EnumCtor1_t)ADDR_PVE_ctor)(it, lp);
    while (((EnumNext_t)ADDR_PVE_GetNext)(it, &u)) {
        int type = UnitCall(ADDR_Unit_GetType, u.unitIndex);
        if (type != MAP_CONVEC && type != MAP_REPAIR_VEHICLE && type != MAP_SPIDER) continue;
        bool cv = type == MAP_CONVEC, sp = type == MAP_SPIDER;
        if (cv) convecs++; else if (sp) sps++; else rvs++;
        if (Command(u.unitIndex) == CT_REPAIR) {
            if (cv) cRep++; else if (sp) sRep++; else rRep++;
            if (nRep < MAX_LIST) { repairer[nRep] = u.unitIndex; repTarget[nRep] = RepairTarget(u.unitIndex); nRep++; }
        } else if (!UnitCall(ADDR_Unit_IsBusy, u.unitIndex) && (!cv || UnitCall(ADDR_Unit_GetCargo, u.unitIndex) == 0)) {
            if (cv) cIdle++; else if (sp) sIdle++; else rIdle++;
        }
    }
    s->convecs = convecs; s->convecsIdle = cIdle; s->convecsRepairing = cRep;
    s->repairVehicles = rvs; s->repairVehiclesIdle = rIdle; s->repairVehiclesRepairing = rRep;
    s->spiders = sps; s->spidersIdle = sIdle; s->spidersRepairing = sRep;

    // damaged units of the switched-on kinds, most damaged (share of hit points) first
    int nb = 0, nd = 0;
    auto Add = [&](int idx, bool vehicle) {
        int dmg = Damage(idx);
        if (dmg < g_minDamage) return;
        if (vehicle && !VehicleNeedsRepair(idx)) return;   // at or above 50%: a Repair Vehicle / Spider can't help
        nd++;
        ArDamaged b;
        b.unitIndex = idx;
        b.type   = UnitCall(ADDR_Unit_GetType, idx);
        b.damage = dmg;
        b.maxHP  = MaxHP(idx, b.type);
        b.x = (PixX(idx) >> 5) + DISP_DX;
        b.y = (PixY(idx) >> 5) + DISP_DY;
        b.repairer = -1;
        b.repairerType = 0;
        for (int k = 0; k < nRep; k++)
            if (repTarget[k] == idx) { b.repairer = repairer[k]; b.repairerType = UnitCall(ADDR_Unit_GetType, repairer[k]); break; }
        b.retryIn = 0;
        if (idx > 0 && idx < MAX_UNIT_INDEX && g_retryAt[idx] > s->tick) b.retryIn = g_retryAt[idx] - s->tick;
        // insert sorted by damage share (damage * 1000 / maxHP), worst first; drop the least damaged past the cap
        long long key = b.maxHP > 0 ? (long long)dmg * 1000 / b.maxHP : dmg;
        int count = nd - 1 < AR_MAX_DAMAGED ? nd - 1 : AR_MAX_DAMAGED;
        int pos = count;
        while (pos > 0) {
            const ArDamaged& q = s->damaged[pos - 1];
            long long qk = q.maxHP > 0 ? (long long)q.damage * 1000 / q.maxHP : q.damage;
            if (qk >= key) break;
            pos--;
        }
        if (pos >= AR_MAX_DAMAGED) return;
        int last = count < AR_MAX_DAMAGED ? count : AR_MAX_DAMAGED - 1;
        for (int k = last; k > pos; k--) s->damaged[k] = s->damaged[k - 1];
        s->damaged[pos] = b;
    };
    memset(it, 0, sizeof(it));
    ((EnumCtor2_t)ADDR_PBE_ctor)(it, lp, 0);
    while (((EnumNext_t)ADDR_PBE_GetNext)(it, &u)) { nb++; if (g_useConVec) Add(u.unitIndex, false); }
    if (g_useRepairVehicle || g_useSpider) {
        memset(it, 0, sizeof(it));
        ((EnumCtor1_t)ADDR_PVE_ctor)(it, lp);
        while (((EnumNext_t)ADDR_PVE_GetNext)(it, &u)) Add(u.unitIndex, true);
    }
    s->buildings  = nb;
    s->numDamaged = nd;
    InterlockedIncrement((volatile LONG*)&s->seq);       // even: done
}

// Create the named mapping the UI reads. Failure only disables the UI feed.
static void OpenSharedMemory() {
    g_shmHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(ArSnapshot), AR_SHM_NAME);
    if (!g_shmHandle) { Log("ui feed: CreateFileMapping failed (%lu) - no UI data", GetLastError()); return; }
    g_shm = (ArSnapshot*)MapViewOfFile(g_shmHandle, FILE_MAP_WRITE, 0, 0, sizeof(ArSnapshot));
    if (!g_shm) { Log("ui feed: MapViewOfFile failed (%lu) - no UI data", GetLastError()); CloseHandle(g_shmHandle); g_shmHandle = NULL; return; }
    memset(g_shm, 0, sizeof(ArSnapshot));
    g_shm->version = AR_SHM_VERSION;
    g_shm->gamePid = (int32_t)GetCurrentProcessId();      // lets the UI quit when the game does
    Log("ui feed: shared memory \"%s\" ready (%u bytes)", AR_SHM_NAME, (unsigned)sizeof(ArSnapshot));
}

// ---- per cycle --------------------------------------------------------------------------
// Runs after every game cycle: notice a new game / loaded save, and run a check every Interval
// cycles.
static void Tick() {
    int tick = *(int*)ADDR_GameTick;
    if (g_lastTick >= 0 && (tick < g_lastTick || tick > g_lastTick + 64)) {
        g_listedOnce = false; g_dbgApplied = false; g_repairsSent = 0; g_countdown = 0;
        memset(g_sentTick, 0, sizeof(g_sentTick)); memset(g_retryAt, 0, sizeof(g_retryAt));
        Log("new game (tick %s)", tick < g_lastTick ? "went back" : "jumped ahead");
    }
    g_lastTick = tick;
    if (--g_countdown <= 0) { g_countdown = g_interval; RepairPass(); }
}

// A fault is logged (where, as AutoRepair.dll+offset or an engine address) and caught, so the
// game keeps running; after FAULT_LIMIT the module stops for the session.
static int FaultFilter(const char* where, EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    uintptr_t eip = ep->ContextRecord->Eip, base = (uintptr_t)g_hSelf, size = 0x100000;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_hSelf;
    if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE)
        size = ((IMAGE_NT_HEADERS*)(base + dos->e_lfanew))->OptionalHeader.SizeOfImage;
    bool inUs = eip >= base && eip < base + size;
    Log("*** FAULT in %s: code 0x%08X at %s0x%X", where, (unsigned)er->ExceptionCode,
        inUs ? "AutoRepair.dll+" : "", (unsigned)(inUs ? eip - base : eip));
    return EXCEPTION_EXECUTE_HANDLER;
}

// Run one per-cycle pass behind an SEH guard.
static void SafeRun(const char* name, void (*fn)()) {
    if (g_stopped) return;
    __try { fn(); }
    __except (FaultFilter(name, GetExceptionInformation())) {
        if (++g_faults >= FAULT_LIMIT) {
            g_stopped = true;
            if (g_shm) {
                if (g_shm->seq & 1) InterlockedIncrement((volatile LONG*)&g_shm->seq);
                g_shm->gameRunning = 0;
                g_shm = NULL;
            }
            Log("%d faults - AutoRepair stopped for this session", g_faults);
        }
    }
}

// Stands in for ProcessGameCycle at its call site. `self` is the ECX the game passed; it goes
// on unchanged to the next hook in the chain (or the engine's function).
static void __fastcall HookedProcessGameCycle(void* self, void* /*edx*/) {
    g_nextCycle(self);
    SafeRun("Tick",    Tick);
    SafeRun("Publish", Publish);
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
    Log("hook: game cycle call @ 0x%08X -> AutoRepair (chains to 0x%08X%s)",
        (unsigned)CALLSITE_PGC, (unsigned)cur, cur == ADDR_ProcessGameCycle ? ", the engine" : ", another module");
    return true;
}

// ---- op2ext entry points --------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hSelf = (HMODULE)hInst; DisableThreadLibraryCalls(hInst); }
    return TRUE;
}

// op2ext calls this once at load with the module's ini section name ("AutoRepair").
EXPORT void InitMod(char* iniSectionName) {
    if (g_installed) return;
    g_installed = true;

    const char* sec = (iniSectionName && *iniSectionName) ? iniSectionName : "AutoRepair";
    char ini[MAX_PATH];
    snprintf(ini, MAX_PATH, "%s\\outpost2.ini", GameDir());
    int enabled   = GetPrivateProfileIntA(sec, "Enabled", 1, ini);
    int iv        = GetPrivateProfileIntA(sec, "Interval", 100, ini);
    int md        = GetPrivateProfileIntA(sec, "MinDamage", 50, ini);
    g_debugDamage = GetPrivateProfileIntA(sec, "DebugDamage", 0, ini);
    g_useConVec        = GetPrivateProfileIntA(sec, "ConVec",        1, ini) != 0;
    g_useRepairVehicle = GetPrivateProfileIntA(sec, "RepairVehicle", 1, ini) != 0;
    g_useSpider        = GetPrivateProfileIntA(sec, "Spider",        1, ini) != 0;
    g_interval  = iv > 0 ? iv : 100;
    g_minDamage = md > 0 ? md : 50;

    Log("=== AutoRepair start (section [%s], Enabled=%d, ConVec=%d, RepairVehicle=%d, Spider=%d, Interval=%d, "
        "MinDamage=%d, DebugDamage=%d) ===", sec, enabled, (int)g_useConVec, (int)g_useRepairVehicle, (int)g_useSpider,
        g_interval, g_minDamage, g_debugDamage);
    if (!enabled) { Log("DISABLED: Enabled=0 - nothing patched"); return; }
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
    Log("=== AutoRepair stop ===");
    return true;
}
