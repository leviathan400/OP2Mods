// bq_shared.h  -  the shared-memory snapshot BuildQueue.dll publishes for BuildQueue-UI.exe
// =========================================================================================
// BuildQueue.dll (inside Outpost2.exe) creates a named file mapping BQ_SHM_NAME and, every
// BQ_PUBLISH_EVERY game cycles, writes one BqSnapshot into it. BuildQueue-UI.exe (or any other
// reader) opens the same name read-only and copies the snapshot out.
//
// Consistency (a "seqlock"): the writer makes `seq` odd, writes the rest, then makes it even
// again. A reader copies the whole struct, then checks that `seq` was even and unchanged
// across the copy; if not, the game was mid-write and the reader tries again next frame.
// The game never waits on the reader.
//
// Both sides include this one header, so the layout always matches. Change BQ_SHM_VERSION
// whenever the layout changes; the UI refuses a snapshot with another version.
//
// Plain C layout, 32-bit ints only, so it means the same in the 32-bit DLL and a 32- or 64-bit
// reader.
#pragma once
#include <stdint.h>

#define BQ_SHM_NAME      "Local\\OP2BuildQueue"   // per-session name; no admin rights needed
#define BQ_SHM_VERSION   3                         // 2: added the enabled* flags; 3: gamePid
#define BQ_MAX_FACTORIES 48                        // factory rows in one snapshot
#define BQ_PUBLISH_EVERY 4                         // game cycles between snapshots

// Factory map_ids (Outpost 2's map_id enum).
#define BQ_MAP_ARACHNID_FACTORY  0x2D
#define BQ_MAP_STRUCTURE_FACTORY 0x2F
#define BQ_MAP_VEHICLE_FACTORY   0x30

// One factory of the local player.
typedef struct BqFactory {
    int32_t unitIndex;      // the factory's unit number (what the game logs as "#27")
    int32_t type;           // its map_id (BQ_MAP_*_FACTORY)
    int32_t busy;           // 1 = building something right now (Unit::IsBusy)
    int32_t lastItem;       // map_id of the last unit you ordered here (0 = none seen this game)
    int32_t lastWeapon;     // its weapon map_id (0 = none)
    int32_t batchLeft;      // units the batch still owes after the current one (0 = no batch)
    int32_t batchTotal;     // the batch's size (0 = no batch)
} BqFactory;

typedef struct BqSnapshot {
    volatile int32_t seq;       // seqlock counter: odd while the game is writing
    int32_t version;            // BQ_SHM_VERSION
    int32_t gameRunning;        // 1 while a game is being played (snapshots arrive), 0 after shutdown
    int32_t gamePid;            // process id of Outpost2.exe (set when the mapping is created), so a
                                // reader can notice the game exiting, even after a crash
    int32_t tick;               // game tick of this snapshot
    int32_t localPlayer;        // the local player's number
    int32_t batchCount;         // [BuildQueue] BatchCount
    int32_t enabledVehicle;     // 1 = Shift batches on at Vehicle Factories   ([BuildQueue] VehicleFactory)
    int32_t enabledStructure;   // 1 = ... at Structure Factories              ([BuildQueue] StructureFactory)
    int32_t enabledArachnid;    // 1 = ... at Arachnid Factories               ([BuildQueue] ArachnidFactory)
                                // The viewer lists only the kinds that are enabled.
    int32_t vehicleFactories;   // counts of the local player's factories
    int32_t structureFactories;
    int32_t arachnidFactories;
    int32_t numFactories;       // rows used in factories[] (all three kinds, may be capped)
    BqFactory factories[BQ_MAX_FACTORIES];
} BqSnapshot;
