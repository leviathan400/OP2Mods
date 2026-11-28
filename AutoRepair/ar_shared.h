// ar_shared.h  -  the shared-memory snapshot AutoRepair.dll publishes for AutoRepair-UI.exe
// =========================================================================================
// AutoRepair.dll (inside Outpost2.exe) creates a named file mapping AR_SHM_NAME and, every
// AR_PUBLISH_EVERY game cycles, writes one ArSnapshot into it. AutoRepair-UI.exe (or any other
// reader) opens the same name read-only and copies the snapshot out.
//
// Consistency (a "seqlock"): the writer makes `seq` odd, writes the rest, then makes it even
// again. A reader copies the whole struct, then checks that `seq` was even and unchanged
// across the copy; if not, the game was mid-write and the reader tries again next frame.
// The game never waits on the reader.
//
// Both sides include this one header, so the layout always matches. Change AR_SHM_VERSION
// whenever the layout changes; the UI refuses a snapshot with another version.
//
// Plain C layout, 32-bit ints only, so it means the same in the 32-bit DLL and a 64-bit reader.
#pragma once
#include <stdint.h>

#define AR_SHM_NAME       "Local\\OP2AutoRepair"   // per-session name; no admin rights needed
#define AR_SHM_VERSION    4                        // 2: retryIn; 3: vehicles + Repair Vehicles, ConVec/RepairVehicle switches; 4: Spiders, repairerType
#define AR_MAX_DAMAGED    32                       // damaged-unit rows in one snapshot
#define AR_PUBLISH_EVERY  4                        // game cycles between snapshots

// One of the local player's damaged units: a building (repaired by ConVecs) or a vehicle
// (repaired by Repair Vehicles and Spiders - only listed below 50% health, the most they restore).
// Only kinds whose switch is on are listed.
typedef struct ArDamaged {
    int32_t unitIndex;      // the unit's number
    int32_t type;           // its map_id (vehicles are 1..16, buildings 21..58)
    int32_t damage;         // damage taken (hit points = maxHP - damage)
    int32_t maxHP;          // its full hit points (0 = unknown)
    int32_t x, y;           // its tile, as the status bar shows it
    int32_t repairer;       // unit number of the ConVec / Repair Vehicle / Spider repairing it, -1 = none yet
    int32_t repairerType;   // that worker's map_id (2 ConVec, 3 Spider, 14 Repair Vehicle), 0 = none
    int32_t retryIn;        // >0: the last repair didn't lower its damage; ticks until AutoRepair tries again
} ArDamaged;

typedef struct ArSnapshot {
    volatile int32_t seq;       // seqlock counter: odd while the game is writing
    int32_t version;            // AR_SHM_VERSION
    int32_t gameRunning;        // 1 while a game is being played (snapshots arrive), 0 after shutdown
    int32_t gamePid;            // process id of Outpost2.exe, so a reader can notice the game exiting
    int32_t tick;               // game tick of this snapshot (a mark is 100 ticks)
    int32_t localPlayer;        // the local player's number
    int32_t interval;           // [AutoRepair] Interval: game cycles between repair passes
    int32_t minDamage;          // [AutoRepair] MinDamage
    int32_t nextPassIn;         // cycles until the next repair pass
    int32_t useConVec;          // [AutoRepair] ConVec: 1 = ConVecs repair damaged buildings
    int32_t useRepairVehicle;   // [AutoRepair] RepairVehicle: 1 = Repair Vehicles repair vehicles below 50%
    int32_t buildings;          // the local player's buildings
    int32_t convecs;            // ... ConVecs (the switch only stops AutoRepair using them)
    int32_t convecsIdle;        //     idle and empty (free to send)
    int32_t convecsRepairing;   //     on a repair job
    int32_t repairVehicles;     // ... Repair Vehicles
    int32_t repairVehiclesIdle;      //     idle (free to send)
    int32_t repairVehiclesRepairing; //     on a repair job
    int32_t useSpider;          // [AutoRepair] Spider: 1 = Spiders repair vehicles below 50%
    int32_t spiders;            // ... Spiders
    int32_t spidersIdle;        //     idle (free to send)
    int32_t spidersRepairing;   //     on a repair job
    int32_t repairsSent;        // repair orders AutoRepair has sent this game
    int32_t numDamaged;         // damaged units listed (all of them, may exceed AR_MAX_DAMAGED)
    ArDamaged damaged[AR_MAX_DAMAGED];    // the most damaged (share of hit points) first
} ArSnapshot;
