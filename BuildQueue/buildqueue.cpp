// buildqueue.cpp  -  BuildQueue: Shift + BUILD IT builds a batch of units at a factory
// =====================================================================================
// An op2ext module for Outpost 2 OPU 1.4.1 (Outpost2.exe, fixed image base 0x00400000).
// Every address used below was checked against the OPU 1.4.1 exe; the same code is
// byte-identical in retail 1.3.6, but only 1.4.1 is supported.
//
// WHAT THE MOD DOES
// -----------------
// A factory's BUILD IT still builds one unit, as in the unmodded game. Hold Shift while
// you click BUILD IT and the factory builds BatchCount of that unit (default 5), one after
// another, with no more clicks. The game's UI takes no new build order while a factory is
// building, and the next order of a batch goes in the cycle the factory turns idle, so a
// batch normally runs to the end. (A build order that reaches a factory with a batch
// running, e.g. from code, replaces that batch.)
//
// HOW IT WORKS (capture and replay)
// ---------------------------------
// The game has no player build queue: a factory takes one ctMoProduce command at a time,
// and clicking BUILD IT on a busy factory does nothing. So the module:
//
//   1. Detours Player::ProcessCommandPacket (FUN_0040e300). Every command the player issues
//      (and the AI, through the exported Unit::Do* methods) goes through it. When the LOCAL
//      player issues a ctMoProduce (command type 9) while Shift is held, the module copies
//      the raw command packet - a ready-made "build this unit at this factory" - and arms a
//      counter of BatchCount - 1 more builds for that factory.
//
//   2. Hooks TethysGame::ProcessGameCycle (once per game cycle). After the game's own cycle,
//      for each armed factory that is idle (Unit::IsBusy == 0) it sends the saved packet
//      again through the original ProcessCommandPacket - the same path the UI uses, so the
//      command goes through the game's normal command queue - and counts one down. A batch
//      whose factory never starts building (no ore, no space, factory destroyed) is dropped
//      after AWAIT_LIMIT cycles instead of retrying forever.
//
// Replaying the game's own packet means the module never builds a command by hand, so the
// command is always one the game itself produced.
//
// Which factory kinds batch is set in the ini (VehicleFactory / StructureFactory /
// ArachnidFactory; by default Vehicle and Arachnid Factories only). The mechanism is the same
// for all three: a build order is the same ctMoProduce command everywhere.
//
// UI FEED (for ui\BuildQueue-UI.exe)
// -----------------------------------
// Every BQ_PUBLISH_EVERY cycles, right after the batch pacing, Publish() writes a snapshot of
// the local player's factories (which kinds are enabled, counts, busy/idle, the last unit ordered at each, batch
// progress) into the named shared memory "Local\OP2BuildQueue". The layout and the seq
// protocol that keeps reads consistent are in bq_shared.h. It only reads game state, so it
// never affects the simulation; if the mapping can't be created the module works as before.
//
// LIMITS
// ------
// Single player / skirmish only. Multiplayer is not supported: the Shift state is read on
// one machine and the batch is paced by that machine's view of the factory.
//
// SHARING THE GAME-CYCLE HOOK
// ---------------------------
// The game calls ProcessGameCycle from one place (0x0049C4BD), and other op2ext modules may
// patch the same call. This module CHAINS: it reads whatever that call currently points at (the
// engine's function, or another module's hook that chains the same way) and calls that,
// rather than assuming the original. Modules that chain the same way all run together, in any
// load order.
//
// INSTALL (OPU 1.4.1, in OPU\outpost2.ini, with BuildQueue.dll in the OPU folder)
// --------------------------------------------------------------------------------
//     [ExternalModules]  BuildQueue=yes     ; OPU's loader: must be yes/no, so leave it yes
//     [BuildQueue]       Dll=BuildQueue.dll
//                        Enabled=1          ; 1 = on, 0 = DLL loads but patches nothing
//                        BatchCount=5       ; units built by Shift + BUILD IT (2..99)
//                        VehicleFactory=1   ; 1 = Shift batches at Vehicle Factories   (default 1)
//                        ArachnidFactory=1  ; 1 = ... at Arachnid Factories            (default 1)
//                        StructureFactory=0 ; 1 = ... at Structure Factories (kits)    (default 0)
// Settings are read once at startup; restart the game after changing them.
// Log: <game dir>\logs\BuildQueue.log (under OPU: OPU\logs\BuildQueue.log).
//
// Build: build.bat (32-bit DLL).

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "bq_shared.h"   // the snapshot layout shared with BuildQueue-UI.exe

#define EXPORT extern "C" __declspec(dllexport)

// ---- engine addresses (OPU 1.4.1 Outpost2.exe) --------------------------------------
static const uintptr_t ADDR_ProcessCommand  = 0x0040E300;  // Player::ProcessCommandPacket(CommandPacket*), __thiscall
static const uintptr_t ADDR_ProcessGameCycle= 0x00489EA0;  // TethysGame::ProcessGameCycle(), __thiscall
static const uintptr_t CALLSITE_PGC         = 0x0049C4BD;  // the one `call ProcessGameCycle` (E8 rel32)
static const uintptr_t ADDR_Unit_IsBusy     = 0x00475E60;  // int Unit::IsBusy() const        (exported)
static const uintptr_t ADDR_Unit_OwnerID    = 0x00475EC0;  // int Unit::OwnerID() const       (exported)
static const uintptr_t ADDR_Unit_GetType    = 0x00475EE0;  // map_id Unit::GetType() const   (exported)
static const uintptr_t ADDR_LocalPlayer     = 0x00477F10;  // static int TethysGame::LocalPlayer() (exported)
static const uintptr_t ADDR_PBE_ctor        = 0x0047A4E0;  // PlayerBuildingEnum(int player, map_id filter) (exported)
static const uintptr_t ADDR_PBE_GetNext     = 0x0047A510;  // int PlayerBuildingEnum::GetNext(Unit&)       (exported)
static const uintptr_t ADDR_GameTick        = 0x0056EB1C;  // current game tick (what TethysGame::Tick returns)
static const uintptr_t PLAYER_ARRAY         = 0x0056EF1C;  // Player objects, one per player slot
static const uintptr_t PLAYER_STRIDE        = 0x0C24;      //   size of one Player object
static const int       MAX_PLAYERS          = 7;

// The first 6 bytes of ProcessCommandPacket, which the detour JMP overwrites:
//   81 EC 18 01 00 00    sub esp, 0x118        (one whole instruction, so it can be moved)
// They are copied into the trampoline, which then jumps back to ADDR_ProcessCommand + 6.
static const BYTE E300_PROLOGUE[6] = { 0x81, 0xEC, 0x18, 0x01, 0x00, 0x00 };
static const int  E300_STEAL       = 6;

// ---- calling conventions ------------------------------------------------------------
// The Unit::* methods take ECX = a Unit object, which is just { int unitIndex }.
// ProcessCommandPacket is __thiscall (ECX = the Player object, the packet on the stack, the
// callee pops it). A __fastcall function with a dummy EDX argument has exactly that shape,
// so it is used to both call it and stand in for it.
// ProcessGameCycle is __thiscall with no stack arguments; its stand-in is __fastcall too, so
// that ECX (the game object) arrives as `self` and can be passed on unchanged.
struct UnitHandle { int unitIndex; };
typedef int  (__fastcall *ProcCmd_t)(void* player, int edxDummy, void* packet);
typedef int  (__thiscall *UnitFn_t)(UnitHandle* self);
typedef int  (__fastcall *LocalPlayer_t)();
typedef void (__thiscall *ProcGameCycle_t)(void* self);
// PlayerBuildingEnum: a small iterator object (16 bytes is plenty) built in place by its
// constructor, then GetNext fills a Unit with each of the player's buildings in turn.
typedef void (__thiscall *PBECtor_t)(void* self, int playerNum, int mapIdFilter);
typedef int  (__thiscall *PBEGetNext_t)(void* self, UnitHandle* outUnit);

// ---- CommandPacket layout -----------------------------------------------------------
//   +0x00 int   messageType     (ctMoProduce = 9)
//   +0x04 short dataLength      (bytes of data[])
//   +0x06 int   timeStamp       (game tick it was issued on)
//   +0x0A int   (unknown)
//   +0x0E       data[]          ctMoProduce: u16 factory, u16 item, u16 weapon, s16 (unused)
#define CP_HEADER      0x0E
#define CP_TYPE(p)     (*(int*)     ((BYTE*)(p) + 0x00))
#define CP_DATALEN(p)  (*(short*)   ((BYTE*)(p) + 0x04))
#define CP_TIME(p)     (*(int*)     ((BYTE*)(p) + 0x06))
#define CP_FACTORY(p)  (*(uint16_t*)((BYTE*)(p) + 0x0E))
#define CP_ITEM(p)     (*(uint16_t*)((BYTE*)(p) + 0x10))
#define CP_WEAPON(p)   (*(uint16_t*)((BYTE*)(p) + 0x12))

static const int CT_PRODUCE  = 9;
static const int AWAIT_LIMIT = 600;    // cycles to wait for a factory to start a build before dropping the batch
static const int MAX_BATCHES = 64;     // factories that can have a batch running at once

// ---- one running batch -------------------------------------------------------------
struct Batch {
    bool  used;
    int   factory;          // unit index of the factory
    int   factoryType;      // its map_id when the batch started, to notice a destroyed/reused unit
    int   owner;            // its owner (player number)
    void* player;           // the Player object (ECX) that issued the order, for replaying it
    BYTE  packet[64];       // the captured ctMoProduce packet
    int   packetLen;
    int   remaining;        // builds still to issue after the one in progress
    bool  awaitingStart;    // an order went in; waiting for the factory to become busy
    int   awaitTicks;       // cycles spent waiting
};
static Batch g_batch[MAX_BATCHES];

// The last unit the local player ordered at each factory (any BUILD IT, batch or not), for
// the UI. Indexed by unit number; cleared with the batches on a new game.
static const int MAX_UNIT_INDEX = 4096;
static uint16_t g_lastItem[MAX_UNIT_INDEX];
static uint16_t g_lastWeapon[MAX_UNIT_INDEX];

// The shared-memory snapshot for BuildQueue-UI.exe (see bq_shared.h). NULL if the mapping
// could not be created; the module then works as before, just without the UI feed.
static HANDLE      g_shmHandle = NULL;
static BqSnapshot* g_shm       = NULL;
static int         g_publishCountdown = 0;

// ---- settings and state -------------------------------------------------------------
static int             g_batchCount  = 5;      // [BuildQueue] BatchCount
static bool            g_onVehicle   = true;   // [BuildQueue] VehicleFactory   (default 1)
static bool            g_onStructure = false;  // [BuildQueue] StructureFactory (default 0)
static bool            g_onArachnid  = true;   // [BuildQueue] ArachnidFactory  (default 1)

// Are Shift batches switched on for this factory map_id?
static bool KindEnabled(int type) {
    switch (type) {
    case BQ_MAP_VEHICLE_FACTORY:   return g_onVehicle;
    case BQ_MAP_STRUCTURE_FACTORY: return g_onStructure;
    case BQ_MAP_ARACHNID_FACTORY:  return g_onArachnid;
    }
    return false;   // anything else that takes build orders (e.g. a Spaceport): not batched
}
static HMODULE         g_hSelf       = NULL;
static ProcCmd_t       g_e300Tramp   = NULL;   // trampoline = the original ProcessCommandPacket
static ProcGameCycle_t g_nextCycle   = NULL;   // what the cycle call pointed at before us
static int             g_lastTick    = -1;     // to notice a new game (the tick goes back)
static bool            g_installed   = false;
static char            g_gameDir[MAX_PATH];

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

// Append one timestamped line to <game dir>\logs\BuildQueue.log. Opened and closed per
// line so the log survives a crash.
static void Log(const char* fmt, ...) {
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\logs", GameDir());
    CreateDirectoryA(path, NULL);
    snprintf(path, MAX_PATH, "%s\\logs\\BuildQueue.log", GameDir());
    FILE* f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// ---- small helpers ------------------------------------------------------------------
// Call an exported `int Unit::X() const` method on unit `unitIndex`.
static int UnitCall(uintptr_t fn, int unitIndex) {
    UnitHandle h = { unitIndex };
    return ((UnitFn_t)fn)(&h);
}

// Which player slot a Player object is, or -1 if the pointer isn't one of them.
static int PlayerIndex(void* player) {
    uintptr_t p = (uintptr_t)player;
    if (p < PLAYER_ARRAY) return -1;
    uintptr_t ofs = p - PLAYER_ARRAY;
    if (ofs % PLAYER_STRIDE) return -1;
    int n = (int)(ofs / PLAYER_STRIDE);
    return n < MAX_PLAYERS ? n : -1;
}

// The batch running at this factory, or NULL.
static Batch* FindBatch(int factory) {
    for (int i = 0; i < MAX_BATCHES; i++)
        if (g_batch[i].used && g_batch[i].factory == factory) return &g_batch[i];
    return NULL;
}

// An unused batch slot, or NULL if all MAX_BATCHES are in use.
static Batch* FreeSlot() {
    for (int i = 0; i < MAX_BATCHES; i++)
        if (!g_batch[i].used) return &g_batch[i];
    return NULL;
}

// Forget every batch and the "last ordered" memory (new game, loaded save, or a fault).
static void ClearAll() {
    memset(g_batch, 0, sizeof(g_batch));
    memset(g_lastItem, 0, sizeof(g_lastItem));
    memset(g_lastWeapon, 0, sizeof(g_lastWeapon));
}

// Send a batch's saved packet again, through the ORIGINAL ProcessCommandPacket (the
// trampoline), so it isn't captured a second time.
static void Replay(Batch& b) {
    CP_TIME(b.packet) = *(int*)ADDR_GameTick;    // stamp it with the current tick, like a fresh order
    g_e300Tramp(b.player, 0, b.packet);
}

// ---- capture: runs for every command, before the game handles it ---------------------
// Look at one command before the game handles it: if it is the local player's build order
// (ctMoProduce), remember what was ordered (for the UI) and, with Shift held at a factory kind
// that is switched on, start a batch by keeping a copy of the packet.
static void Capture(void* player, void* packet) {
    if (CP_TYPE(packet) != CT_PRODUCE) return;
    int dataLen = CP_DATALEN(packet);
    if (dataLen < 8 || CP_HEADER + dataLen > (int)sizeof(g_batch[0].packet)) return;

    // Only the local player's own orders. The AI issues its build orders through the same
    // function, and must not be batched because the player is holding Shift.
    int pn = PlayerIndex(player);
    if (pn < 0 || pn != ((LocalPlayer_t)ADDR_LocalPlayer)()) return;

    int factory = CP_FACTORY(packet);
    if (factory >= 0 && factory < MAX_UNIT_INDEX) {          // remember it for the UI
        g_lastItem[factory]   = CP_ITEM(packet);
        g_lastWeapon[factory] = CP_WEAPON(packet);
    }

    // Any new order at a factory replaces the batch running there.
    Batch* old = FindBatch(factory);
    if (old) {
        if (old->remaining > 0) Log("cancel: factory #%d, %d unbuilt (new build order)", factory, old->remaining);
        old->used = false;
    }

    // Shift held at the moment of the BUILD IT click -> start a batch.
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!shift || g_batchCount < 2) return;

    // Only at the factory kinds switched on in the ini (VehicleFactory / StructureFactory /
    // ArachnidFactory). Elsewhere Shift + BUILD IT builds one, as in the unmodded game.
    int ftype = UnitCall(ADDR_Unit_GetType, factory);
    if (!KindEnabled(ftype)) {
        Log("skip: factory #%d (type %d) - batches are off for this kind in the ini", factory, ftype);
        return;
    }

    Batch* b = FreeSlot();
    if (!b) { Log("batch table full, factory #%d builds one", factory); return; }
    memset(b, 0, sizeof(*b));
    b->used          = true;
    b->factory       = factory;
    b->factoryType   = UnitCall(ADDR_Unit_GetType, factory);
    b->owner         = UnitCall(ADDR_Unit_OwnerID, factory);
    b->player        = player;
    b->packetLen     = CP_HEADER + dataLen;
    memcpy(b->packet, packet, b->packetLen);
    b->remaining     = g_batchCount - 1;    // the order passing through now is the first one
    b->awaitingStart = true;                // it will make the factory busy shortly
    Log("batch: factory #%d (type %d) item=%u weapon=%u x%d",
        factory, b->factoryType, CP_ITEM(packet), CP_WEAPON(packet), g_batchCount);
}

// The detour target: the JMP at ProcessCommandPacket's entry lands here with the stack and
// registers exactly as the caller left them (ECX = Player, packet on the stack).
static int __fastcall HookProcessCommand(void* player, int edxDummy, void* packet) {
    Capture(player, packet);
    return g_e300Tramp(player, edxDummy, packet);   // then run the game's own code
}

// ---- pacing: runs once per game cycle, after the game's own cycle --------------------
// Every cycle: for each batch, drop it if its factory is gone or never starts, and send the
// saved order again the cycle the factory turns idle.
static void QueueTick() {
    int tick = *(int*)ADDR_GameTick;
    if (tick < g_lastTick) ClearAll();   // tick went back: a new game or a loaded save
    g_lastTick = tick;

    for (int i = 0; i < MAX_BATCHES; i++) {
        Batch& b = g_batch[i];
        if (!b.used) continue;

        // The factory died (or its unit slot now holds something else): drop the batch.
        if (UnitCall(ADDR_Unit_GetType, b.factory) != b.factoryType ||
            UnitCall(ADDR_Unit_OwnerID, b.factory) != b.owner) {
            Log("drop: factory #%d is gone, %d unbuilt", b.factory, b.remaining);
            b.used = false; continue;
        }

        bool busy = UnitCall(ADDR_Unit_IsBusy, b.factory) != 0;
        if (b.awaitingStart) {
            if (busy) { b.awaitingStart = false; b.awaitTicks = 0; }
            else if (++b.awaitTicks > AWAIT_LIMIT) {
                Log("drop: factory #%d never started building, %d unbuilt", b.factory, b.remaining);
                b.used = false;
            }
        } else if (!busy) {
            if (b.remaining > 0) {
                Replay(b);
                b.remaining--;
                b.awaitingStart = true;
                b.awaitTicks = 0;
                Log("build: factory #%d, %d left", b.factory, b.remaining);
            } else {
                b.used = false;   // batch finished
            }
        }
    }
}

// ---- UI feed: write a snapshot of the local player's factories to shared memory -------
// Runs every BQ_PUBLISH_EVERY cycles, on the game thread, right after QueueTick. Only reads
// game state (through the exported Unit / PlayerBuildingEnum methods), so it never affects
// the simulation. See bq_shared.h for the layout and the seq protocol.
static void Publish() {
    if (!g_shm) return;
    if (--g_publishCountdown > 0) return;
    g_publishCountdown = BQ_PUBLISH_EVERY;

    BqSnapshot* s = g_shm;
    InterlockedIncrement((volatile LONG*)&s->seq);           // odd: writing
    s->version     = BQ_SHM_VERSION;
    s->gameRunning = 1;
    s->tick        = *(int*)ADDR_GameTick;
    s->localPlayer = ((LocalPlayer_t)ADDR_LocalPlayer)();
    s->batchCount  = g_batchCount;
    s->enabledVehicle   = g_onVehicle;
    s->enabledStructure = g_onStructure;
    s->enabledArachnid  = g_onArachnid;
    s->vehicleFactories = s->structureFactories = s->arachnidFactories = 0;

    int n = 0;
    char it[16] = {0};                                        // PlayerBuildingEnum object
    ((PBECtor_t)ADDR_PBE_ctor)(it, s->localPlayer, 0);        // filter 0 = all buildings
    UnitHandle u;
    while (((PBEGetNext_t)ADDR_PBE_GetNext)(it, &u)) {
        int type = UnitCall(ADDR_Unit_GetType, u.unitIndex);
        if      (type == BQ_MAP_VEHICLE_FACTORY)   s->vehicleFactories++;
        else if (type == BQ_MAP_STRUCTURE_FACTORY) s->structureFactories++;
        else if (type == BQ_MAP_ARACHNID_FACTORY)  s->arachnidFactories++;
        else continue;
        if (n >= BQ_MAX_FACTORIES) continue;                  // keep counting, stop listing

        BqFactory& f = s->factories[n++];
        memset(&f, 0, sizeof(f));
        f.unitIndex = u.unitIndex;
        f.type      = type;
        f.busy      = UnitCall(ADDR_Unit_IsBusy, u.unitIndex) != 0;
        if (u.unitIndex >= 0 && u.unitIndex < MAX_UNIT_INDEX) {
            f.lastItem   = g_lastItem[u.unitIndex];
            f.lastWeapon = g_lastWeapon[u.unitIndex];
        }
        Batch* b = FindBatch(u.unitIndex);
        if (b) { f.batchLeft = b->remaining; f.batchTotal = g_batchCount; }
    }
    s->numFactories = n;
    InterlockedIncrement((volatile LONG*)&s->seq);           // even: done
}

// Create the named mapping the UI reads. Failure only disables the UI feed.
static void OpenSharedMemory() {
    g_shmHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                     sizeof(BqSnapshot), BQ_SHM_NAME);
    if (!g_shmHandle) { Log("ui feed: CreateFileMapping failed (%lu) - no UI data", GetLastError()); return; }
    g_shm = (BqSnapshot*)MapViewOfFile(g_shmHandle, FILE_MAP_WRITE, 0, 0, sizeof(BqSnapshot));
    if (!g_shm) { Log("ui feed: MapViewOfFile failed (%lu) - no UI data", GetLastError()); CloseHandle(g_shmHandle); g_shmHandle = NULL; return; }
    memset(g_shm, 0, sizeof(BqSnapshot));
    g_shm->version = BQ_SHM_VERSION;
    g_shm->gamePid = (int32_t)GetCurrentProcessId();   // lets the UI quit when the game does
    Log("ui feed: shared memory \"%s\" ready (%u bytes)", BQ_SHM_NAME, (unsigned)sizeof(BqSnapshot));
}

// QueueTick behind an SEH guard: a fault in the mod is logged and the mod stops, instead of
// taking the game down.
static bool g_tickFaulted = false;   // a fault happened: batches and the feed are off for the session
static void SafeQueueTick() {
    if (g_tickFaulted) return;
    __try { QueueTick(); Publish(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tickFaulted = true;
        ClearAll();
        if (g_shm) {                                   // leave the snapshot readable: seq even,
            g_shm->gameRunning = 0;                    // and tell the UI the feed has stopped
            if (g_shm->seq & 1) InterlockedIncrement((volatile LONG*)&g_shm->seq);
            g_shm = NULL;
        }
        Log("FAULT in QueueTick (code 0x%08X) - batches stopped for this session", GetExceptionCode());
    }
}

// Stands in for ProcessGameCycle at its call site. `self` is the ECX the game passed;
// it goes on unchanged to the next hook in the chain (or the engine's function).
static void __fastcall HookedProcessGameCycle(void* self, void* /*edx*/) {
    g_nextCycle(self);
    SafeQueueTick();
}

// ---- installing the hooks ------------------------------------------------------------
// Point the `call ProcessGameCycle` at 0x0049C4BD at our hook, remembering what it called
// before (the engine's function, or another module's chained hook).
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
    Log("hook: game cycle call @ 0x%08X -> BuildQueue (chains to 0x%08X%s)",
        (unsigned)CALLSITE_PGC, (unsigned)cur, cur == ADDR_ProcessGameCycle ? ", the engine" : ", another module");
    return true;
}

// Detour ProcessCommandPacket: copy its first instruction into a trampoline that jumps back
// to the rest of the function, then overwrite the entry with a JMP to HookProcessCommand.
static bool HookProcessCommandPacket() {
    if (memcmp((void*)ADDR_ProcessCommand, E300_PROLOGUE, E300_STEAL) != 0) {
        const BYTE* p = (const BYTE*)ADDR_ProcessCommand;
        Log("hook: ProcessCommandPacket entry is %02X %02X %02X %02X %02X %02X, not the expected "
            "81 EC 18 01 00 00 (another module patched it?) - not installed", p[0], p[1], p[2], p[3], p[4], p[5]);
        return false;
    }
    BYTE* tr = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { Log("hook: VirtualAlloc for the trampoline failed"); return false; }
    memcpy(tr, (void*)ADDR_ProcessCommand, E300_STEAL);                       // sub esp, 0x118
    tr[E300_STEAL] = 0xE9;                                                   // jmp ProcessCommand+6
    *(int*)(tr + E300_STEAL + 1) = (int)((ADDR_ProcessCommand + E300_STEAL) - (uintptr_t)(tr + E300_STEAL + 5));
    FlushInstructionCache(GetCurrentProcess(), tr, 32);
    g_e300Tramp = (ProcCmd_t)tr;

    BYTE patch[E300_STEAL];
    patch[0] = 0xE9;                                                         // jmp HookProcessCommand
    *(int*)(patch + 1) = (int)((uintptr_t)&HookProcessCommand - (ADDR_ProcessCommand + 5));
    patch[5] = 0x90;                                                         // nop (pads the 6th byte)
    DWORD op;
    if (!VirtualProtect((void*)ADDR_ProcessCommand, E300_STEAL, PAGE_EXECUTE_READWRITE, &op)) {
        Log("hook: VirtualProtect failed on ProcessCommandPacket"); return false;
    }
    memcpy((void*)ADDR_ProcessCommand, patch, E300_STEAL);
    VirtualProtect((void*)ADDR_ProcessCommand, E300_STEAL, op, &op);
    FlushInstructionCache(GetCurrentProcess(), (void*)ADDR_ProcessCommand, E300_STEAL);
    Log("hook: ProcessCommandPacket @ 0x%08X detoured (trampoline 0x%p)", (unsigned)ADDR_ProcessCommand, tr);
    return true;
}

// ---- op2ext entry points -------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hSelf = (HMODULE)hInst; DisableThreadLibraryCalls(hInst); }
    return TRUE;
}

// op2ext calls this once at load with the module's ini section name ("BuildQueue").
EXPORT void InitMod(char* iniSectionName) {
    if (g_installed) return;
    g_installed = true;

    const char* sec = (iniSectionName && *iniSectionName) ? iniSectionName : "BuildQueue";
    char ini[MAX_PATH];
    snprintf(ini, MAX_PATH, "%s\\outpost2.ini", GameDir());
    int enabled = GetPrivateProfileIntA(sec, "Enabled", 1, ini);
    int bc      = GetPrivateProfileIntA(sec, "BatchCount", 5, ini);
    if (bc < 1) bc = 1;
    if (bc > 99) bc = 99;
    g_batchCount = bc;
    g_onVehicle   = GetPrivateProfileIntA(sec, "VehicleFactory",   1, ini) != 0;
    g_onStructure = GetPrivateProfileIntA(sec, "StructureFactory", 0, ini) != 0;
    g_onArachnid  = GetPrivateProfileIntA(sec, "ArachnidFactory",  1, ini) != 0;

    Log("=== BuildQueue start (section [%s], Enabled=%d, BatchCount=%d, VehicleFactory=%d, "
        "StructureFactory=%d, ArachnidFactory=%d) ===", sec, enabled, g_batchCount,
        (int)g_onVehicle, (int)g_onStructure, (int)g_onArachnid);
    if (!enabled) { Log("DISABLED: Enabled=0 - nothing patched"); return; }

    ClearAll();
    // The batches need both hooks, so the cycle hook goes in only if the capture detour did.
    // The UI feed rides on the cycle hook.
    if (HookProcessCommandPacket()) {
        OpenSharedMemory();
        HookGameCycle();
    }
}

// op2ext calls this at shutdown. The patches are left in place: the game is exiting, and
// another module may have chained its cycle hook onto ours. The UI is told the game is gone.
EXPORT bool DestroyMod() {
    if (g_shm) {
        InterlockedIncrement((volatile LONG*)&g_shm->seq);
        g_shm->gameRunning = 0;
        InterlockedIncrement((volatile LONG*)&g_shm->seq);
    }
    Log("=== BuildQueue stop ===");
    return true;
}
