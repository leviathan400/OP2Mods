// rl_shared.h  -  the shared-memory snapshot Rally.dll publishes for Rally-UI.exe
// ==============================================================================
// Rally.dll (inside Outpost2.exe) creates a named file mapping RL_SHM_NAME and, every
// RL_PUBLISH_EVERY game cycles, writes one RlSnapshot into it. Rally-UI.exe (or any other
// reader) opens the same name read-only and copies the snapshot out.
//
// Consistency (a "seqlock"): the writer makes `seq` odd, writes the rest, then makes it even
// again. A reader copies the whole struct, then checks that `seq` was even and unchanged
// across the copy; if not, the game was mid-write and the reader tries again next frame.
// The game never waits on the reader.
//
// Both sides include this one header, so the layout always matches. Change RL_SHM_VERSION
// whenever the layout changes; the UI refuses a snapshot with another version.
//
// Coordinates: every x/y here is in the DISPLAYED frame - the tile numbers Outpost 2's status
// bar shows - so the viewer can print them as they are.
//
// Plain C layout, 32-bit ints only, so it means the same in the 32-bit DLL and a 32- or 64-bit
// reader.
#pragma once
#include <stdint.h>

#define RL_SHM_NAME       "Local\\OP2Rally"   // per-session name; no admin rights needed
#define RL_SHM_VERSION    1
#define RL_MAX_FACTORIES  32                  // factory rows in one snapshot
#define RL_MAX_TRACKS     16                  // recently rallied vehicles in one snapshot
#define RL_PUBLISH_EVERY  4                   // game cycles between snapshots

// map_ids (Outpost 2's map_id enum).
#define RL_MAP_ARACHNID_FACTORY  0x2D
#define RL_MAP_VEHICLE_FACTORY   0x30

// One rally point.
typedef struct RlPoint {
    int32_t set;            // 1 = set (in the ini or in game), 0 = not set: that group isn't moved
    int32_t x, y;           // the tile
    int32_t fromGame;       // 1 = set in game with [ / ], 0 = from the ini
} RlPoint;

// One of the local player's Vehicle / Arachnid Factories.
typedef struct RlFactory {
    int32_t unitIndex;      // the factory's unit number
    int32_t type;           // RL_MAP_*_FACTORY
    int32_t x, y;           // its tile
    int32_t busy;           // 1 = building something right now
} RlFactory;

// A vehicle Rally sent to a rally point (newest first in the snapshot).
typedef struct RlTrack {
    int32_t unitIndex;      // the vehicle's unit number
    int32_t type;           // its map_id
    int32_t weapon;         // its weapon map_id (0 = none)
    int32_t military;       // 1 = sent to the military point, 0 = colony
    int32_t factory;        // unit number of the factory it came out of
    int32_t tick;           // game tick it was sent
    int32_t destX, destY;   // the rally tile it was sent to
    int32_t x, y;           // where it is now (last place seen, if gone)
    int32_t state;          // RL_TRACK_*
} RlTrack;

#define RL_TRACK_MOVING   0   // alive, not at the rally tile yet
#define RL_TRACK_ARRIVED  1   // got within 1 tile of the rally tile at some point
#define RL_TRACK_GONE     2   // destroyed (or its unit number now holds something else)

typedef struct RlSnapshot {
    volatile int32_t seq;       // seqlock counter: odd while the game is writing
    int32_t version;            // RL_SHM_VERSION
    int32_t gameRunning;        // 1 while a game is being played (snapshots arrive), 0 after shutdown
    int32_t gamePid;            // process id of Outpost2.exe, so a reader can notice the game exiting
    int32_t tick;               // game tick of this snapshot
    int32_t localPlayer;        // the local player's number
    int32_t marker;             // [Rally] Marker: 0 = none, 1 = circle, 2 = DNA, 3 = beaker
    RlPoint military;           // where armed vehicles go
    RlPoint colony;             // where everything else goes
    int32_t selectedFactory;    // unit number of the selected Vehicle/Arachnid Factory, -1 = none
                                // (the [ / ] hotkeys work only while this is set)
    int32_t ralliedMilitary;    // vehicles sent this game, per group
    int32_t ralliedColony;
    int32_t vehicles;           // the local player's vehicles right now
    int32_t numFactories;       // rows used in factories[]
    RlFactory factories[RL_MAX_FACTORIES];
    int32_t numTracks;          // rows used in tracks[]
    RlTrack tracks[RL_MAX_TRACKS];
} RlSnapshot;
