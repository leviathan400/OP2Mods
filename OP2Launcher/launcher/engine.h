// engine.h - Outpost 2 engine functions/structs we touch (OPU 1.4.1 / retail 1.3.6
// share these addresses). Minimal standalone: we declare exactly what we call rather
// than pulling in the whole OP2Internal library. Outpost2.exe is fixed-base
// 0x00400000 (the old exe is not ASLR-relocated), so a raw pointer to a hard address
// is valid in-process.
//
// The module's ACTIVE launch path is LaunchMission() = OnLoadScript() + ActivateGame()
// (the engine's own debug-menu sequence). The GameStartInfo struct, the StartupFlag
// enum, and StartGame()/StartSingleGame() below are kept as REFERENCE: they document
// the underlying mechanism (OnLoadScript builds a GameStartInfo internally, then calls
// StartGame) but are not on the active code path.
//
// Sources (cross-checked): OP2Internal (OP2Internal.asm exports + GameStartInfo.h) and
// the code of Outpost2.exe and op2shell.dll themselves.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace op2 {

// ---- GameStartInfo ---------------------------------------------------------
// Base struct is 0xBA bytes; StartGame can index into the larger in-memory
// "MissionResults" view (up to ~0x3DD), so callers must back this with a
// zeroed buffer of at least kStartInfoBufSize bytes to avoid a stray read.
#pragma pack(push, 1)
struct GameStartInfo {
    int  startupFlags;      // 0x00  StartupFlags bitfield (see below)
    int  version;           // 0x04  e.g. 0x01040001 = 1.4.0.1
    int  b1;                // 0x08
    int  gameFlags;         // 0x0C  GameFlags: numPlayers:3, bReplay:1
    char scriptName[32];    // 0x10  mission DLL filename - THE field that picks the DLL
    int  playerFlags[6];    // 0x30
    int  playerNetID[6];    // 0x48
    char playerName[13 * 6];// 0x60  playerName[0] is the local player
    int  gameSpeed4X;       // 0xAE  game speed (valid range ~4..40)
    int  randomNumberSeed;  // 0xB2
    int  b2;                // 0xB6
};                          // size 0xBA
#pragma pack(pop)

static const size_t kStartInfoBufSize = 0x400;  // >= MissionResults (0x3DD), zeroed

// scriptName is a fixed char[32] (incl. the NUL), and OnLoadScript copies the name into it
// unchecked - so a mission DLL name may be at most 31 characters. Longer names are rejected
// (not truncated: a truncated name would load a different DLL or none).
static const size_t kMaxScriptNameLen = sizeof(((GameStartInfo*)0)->scriptName) - 1;  // 31

inline bool ScriptNameFits(const char* name) {
    return name && strlen(name) <= kMaxScriptNameLen;
}

// startupFlags bits (from OP2Internal StartFlags.h)
enum StartupFlag {
    kDisastersOn      = 0x00000001,
    kDayNightOn       = 0x00000002,
    kMoraleOn         = 0x00000004,
    kCampaign         = 0x00000008,  // MUST be 0, else StartGame rebuilds name as E/P%02d.DLL
    kMultiplayer      = 0x00000010,  // MUST be 0 for our single-player launch path
    kCheatsOn         = 0x00000020,
    kMaxPlayersShift  = 6,           // maxPlayers : 3 bits  (mask 0x000001C0)
    kMissionTypeShift = 9,           // missionType: 5 bits  (mask 0x00003E00)
    kInitVehiclesShift= 17,          // numInitialVehicles : 4 bits (mask 0x001E0000)
};

// ---- Game-start entry points -----------------------------------------------
// TApp::StartSingleGame is the front-end shell's own "start a single game" entry:
// it calls TethysGame::StartGame, sets app.bGameInitialized (+0x16C), and signals
// the shell loop to hand off to the game loop. This is the correct call - using
// raw StartGame leaves the shell loop running and the engine crashes once the sim
// spins up. Must be called on the main thread, from a clean top-of-message-loop
// context (not nested inside a hook).
typedef int(__thiscall* StartSingleGameFn)(void* app, GameStartInfo* info);
typedef int(__thiscall* StartGameFn)(void* thisGame, GameStartInfo* info);

static const uintptr_t kStartSingleGameAddr = 0x00487F80;
static const uintptr_t kAppInstance         = 0x0056E868;  // global TApp "this"
static const uintptr_t kStartGameAddr       = 0x004893B0;
static const uintptr_t kTethysGameInstance  = 0x0056EA98;  // global TethysGame "this"
static const uintptr_t kExpectedExeBase     = 0x00400000;

// Returns nonzero on a successful start.
inline int StartSingleGame(GameStartInfo* info) {
    StartSingleGameFn fn = reinterpret_cast<StartSingleGameFn>(kStartSingleGameAddr);
    return fn(reinterpret_cast<void*>(kAppInstance), info);
}

// ---- The debug-menu launch path (the one that actually works) --------------
// TApp::OnLoadScript(char* scriptName): builds a GameStartInfo from the name +
// ini GameSpeed/seed and calls StartGame (a non-NULL name is used verbatim, with
// the campaign bit clear). This is exactly what debug-menu command 0x9CB7 does.
//   void __thiscall TApp::OnLoadScript(char*)                  @ 0x00487590
// TApp::ActivateGame(): switches TApp.activeFrame (+0x44) to the in-game frame
// (Dans_RULE_UIFrame @ 0x005756C0) so the game renders + runs - the step a bare
// StartGame/StartSingleGame call omits. Self-guards if already active. (cmd 0x9C6C)
//   int  __thiscall TApp::ActivateGame(void)                   @ 0x00486F40
typedef void(__thiscall* OnLoadScriptFn)(void* app, const char* scriptName);
typedef int (__thiscall* ActivateGameFn)(void* app);

static const uintptr_t kOnLoadScriptAddr = 0x00487590;
static const uintptr_t kActivateGameAddr = 0x00486F40;

// Launch an arbitrary mission DLL the way the debug menu does. Main thread only.
inline void LaunchMission(const char* scriptName) {
    OnLoadScriptFn load     = reinterpret_cast<OnLoadScriptFn>(kOnLoadScriptAddr);
    ActivateGameFn activate = reinterpret_cast<ActivateGameFn>(kActivateGameAddr);
    load(reinterpret_cast<void*>(kAppInstance), scriptName);   // start the game
    activate(reinterpret_cast<void*>(kAppInstance));           // switch to game frame
}

// ---- Loading a saved game --------------------------------------------------
// The shell's own Load button (OP2Shell.dll 0x130082B3) runs TApp::GetLoadName (its
// dialog copies the chosen file to TApp +0x5C, 0x104 bytes), then TApp::OnLoadGame,
// then TApp::ActivateGame when OnLoadGame returned nonzero. OnLoadGame (0x004874F0)
// first loads the sprite sheets (0x00445180), then TApp::LoadGame(TApp +0x5C)
// (0x004878F0) behind a "Loading..." box; LoadGame shows its own error box for a bad
// file. Calling TApp::LoadGame alone from the shell skips the sheets, and the game then
// crashes drawing its first frame (Viewport::MarkBackgroundRectToRedraw, Outpost2.exe
// +0x3DC4) - as the command line's /noshell /map <save> does under OPU 1.4.1.
typedef int(__thiscall* OnLoadGameFn)(void* app);

static const uintptr_t kOnLoadGameAddr  = 0x004874F0;
static const uintptr_t kLoadNameOffset  = 0x5C;    // TApp's saved-game file name
static const size_t    kLoadNameSize    = 0x104;

// Load a saved game the way the shell's Load button does, after its dialog. Main thread
// only. Returns nonzero when the save loaded (the game is then running).
inline int LoadSavedGame(const char* path) {
    if (!path || strlen(path) >= kLoadNameSize) return 0;
    char* name = reinterpret_cast<char*>(kAppInstance + kLoadNameOffset);
    strncpy_s(name, kLoadNameSize, path, _TRUNCATE);
    OnLoadGameFn load       = reinterpret_cast<OnLoadGameFn>(kOnLoadGameAddr);
    ActivateGameFn activate = reinterpret_cast<ActivateGameFn>(kActivateGameAddr);
    if (!load(reinterpret_cast<void*>(kAppInstance))) return 0;
    activate(reinterpret_cast<void*>(kAppInstance));
    return 1;
}

// Low-level start (does NOT signal the shell hand-off). Kept for reference.
inline int StartGame(GameStartInfo* info) {
    StartGameFn fn = reinterpret_cast<StartGameFn>(kStartGameAddr);
    return fn(reinterpret_cast<void*>(kTethysGameInstance), info);
}

}  // namespace op2
