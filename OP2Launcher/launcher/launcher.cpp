// launcher.cpp - op2ext ".ini module" (Launcher) that boots Outpost 2 straight into
// an arbitrary mission/scenario/colony DLL, or pops a picker to choose one.
//
// op2ext contract (see op2ext ReadMe.txt / IniModule.cpp): op2ext loads Launcher.dll
// and calls InitMod("Launcher") then DestroyMod(). It loads us only when
// [ExternalModules] Launcher=yes. Config keys (all under [Launcher]):
//     Dll=Launcher.dll            ; module path (op2ext's key; relative to the OPU dir)
//     Enabled=1                   ; everyday on/off - 0 = loaded but does nothing
//     Mission=mymission.dll       ; the DLL to launch (when Auto=1)
//     Auto=1                      ; 1 = launch Mission on startup; 0 = show the picker
//     Load=saves\SGAME0.OP2       ; a saved game to load instead (relative to the OPU dir,
//                                 ; or absolute); cleared once used. Takes precedence.
//     OneShot=1                   ; 1 = set Enabled=0 again after an auto launch
//     PlayerName=Player           ; informational (logged only)
//     GameSpeed=8                 ; informational (OnLoadScript uses the [Game] speed)
//
// How it launches (see engine.h): it does NOT poke GameStartInfo itself. Once the
// shell window is up it reuses the engine's OWN debug-menu path - OnLoadScript(name)
// builds the GameStartInfo + starts the game, then ActivateGame() switches to the
// in-game frame so it renders. The launch is posted to the shell window so it runs
// on OP2's main thread in a clean message-loop context.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include "engine.h"

static HINSTANCE g_hInst = nullptr;
static char      g_section[64] = "Launcher";
static char      g_gameDir[MAX_PATH] = "";   // op2ext game dir (the OPU runtime dir), trailing slash
static char      g_iniPath[MAX_PATH] = "";   // <gameDir>outpost2.ini
static char      g_logPath[MAX_PATH] = "";   // <gameDir>logs\launcher.log

// config
static bool        g_enabled = false;   // [Launcher] Enabled=1/0 - the everyday on/off
static std::string g_mission;
static std::string g_load;              // [Launcher] Load= - a saved game to load (wins over Mission)
static bool        g_auto = false;
static bool        g_oneShot = false;   // turn Enabled back to 0 after one auto launch
static std::string g_playerName = "Player";
static int         g_gameSpeed4X = 8;

// trigger state (all touched only on the main thread)
static HHOOK   g_hook = nullptr;
static DWORD   g_mainThreadId = 0;
static DWORD   g_shellFirstSeenTick = 0;
static bool    g_fired = false;
static WNDPROC g_origShellProc = nullptr;
static const DWORD kSettleMs = 750;  // let the shell finish initialising before we start
#define WM_LAUNCHER_GO (WM_APP + 0x4C2)

std::string ShowPicker();  // picker.cpp

// ---------------------------------------------------------------------------
static void Log(const std::string& msg) {
    if (g_logPath[0] == '\0') return;
    FILE* f = nullptr;
    fopen_s(&f, g_logPath, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg.c_str());
    fclose(f);
}

// OPULauncher runs the game with cwd = the OPU runtime dir (where op2ext read
// outpost2.ini and writes Outpost2Log.txt / ipxwrapper.log). op2ext's GetGameDir_s
// instead returns the raw Outpost2.exe folder, which is the dead unpatched GOG copy
// in the root - so we use the current directory, which is the live OPU dir.
static void ResolveGameDir() {
    char dir[MAX_PATH] = "";
    GetCurrentDirectoryA(sizeof(dir), dir);
    size_t n = strlen(dir);
    if (n && dir[n - 1] != '\\' && dir[n - 1] != '/' && n + 1 < sizeof(dir)) {
        dir[n] = '\\'; dir[n + 1] = '\0';
    }

    strncpy_s(g_gameDir, dir, _TRUNCATE);

    std::string ini = std::string(g_gameDir) + "outpost2.ini";
    strncpy_s(g_iniPath, ini.c_str(), _TRUNCATE);

    std::string logDir = std::string(g_gameDir) + "logs";
    CreateDirectoryA(logDir.c_str(), nullptr);           // harmless if it already exists
    std::string log = logDir + "\\launcher.log";
    strncpy_s(g_logPath, log.c_str(), _TRUNCATE);
}

static std::string IniStr(const char* key, const char* def) {
    char buf[512] = "";
    GetPrivateProfileStringA(g_section, key, def, buf, sizeof(buf), g_iniPath);
    return std::string(buf);
}
static int IniInt(const char* key, int def) {
    return (int)GetPrivateProfileIntA(g_section, key, def, g_iniPath);
}

// One-shot: turn the module's own [Launcher] Enabled flag back to 0 so the NEXT
// normal launch is clean (module loads but does nothing). op2launcher.exe sets
// Enabled=1 each time it runs.
static void DisableSelf() {
    WritePrivateProfileStringA(g_section, "Enabled", "0", g_iniPath);
    Log("one-shot: set [" + std::string(g_section) + "] Enabled=0 (off next launch)");
}

static void ReadConfig() {
    g_enabled     = IniInt("Enabled", 0) != 0;
    g_mission     = IniStr("Mission", "");
    g_load        = IniStr("Load", "");
    g_auto        = IniInt("Auto", 0) != 0;
    g_oneShot     = IniInt("OneShot", 0) != 0;
    g_playerName  = IniStr("PlayerName", "Player");
    g_gameSpeed4X = IniInt("GameSpeed", 8);

    Log("config: Enabled=" + std::string(g_enabled ? "1" : "0") +
        " Mission='" + g_mission + "' Load='" + g_load + "' Auto=" + (g_auto ? "1" : "0") +
        " OneShot=" + (g_oneShot ? "1" : "0") +
        " PlayerName='" + g_playerName + "' GameSpeed=" + std::to_string(g_gameSpeed4X));
}

// ---------------------------------------------------------------------------
// Start the chosen mission DLL. Runs on the main thread.
static void DoLaunch(const std::string& missionDll) {
    if (missionDll.empty()) { Log("DoLaunch: empty mission, nothing to do"); return; }
    if (!op2::ScriptNameFits(missionDll.c_str())) {   // backstop for ini Mission= / any caller
        Log("DoLaunch: REJECTED '" + missionDll + "' - " + std::to_string(missionDll.size()) +
            " chars, max is " + std::to_string(op2::kMaxScriptNameLen) + " (GameStartInfo.scriptName[32])");
        return;
    }

    // Use the engine's own debug-menu path: OnLoadScript builds the GameStartInfo
    // from the name (+ ini GameSpeed/seed) and starts the game; ActivateGame then
    // switches to the in-game frame so it renders. (g_playerName/g_gameSpeed4X are
    // informational here - OnLoadScript sources speed from the main [Game] ini.)
    Log("DoLaunch: OnLoadScript + ActivateGame (scriptName='" + missionDll + "')");
    op2::LaunchMission(missionDll.c_str());
    Log("DoLaunch: launch sequence issued");
}

// Load a saved game. Runs on the main thread. A relative path is taken from the OPU
// runtime dir (where OPU keeps saves\SGAMEn.OP2).
static void DoLoad(const std::string& save) {
    bool absolute = save.size() > 1 && (save[1] == ':' || (save[0] == '\\' && save[1] == '\\'));
    std::string path = absolute ? save : std::string(g_gameDir) + save;
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("DoLoad: no such saved game '" + path + "'");
        return;
    }
    Log("DoLoad: OnLoadGame + ActivateGame ('" + path + "')");
    int ok = op2::LoadSavedGame(path.c_str());
    Log(ok ? "DoLoad: loaded" : "DoLoad: not loaded (path too long, or the game showed its own error)");
}

// Find a visible top-level window created on OP2's main thread (the shell).
static HWND FindShellWindow() {
    struct Ctx { HWND found; } ctx{ nullptr };
    EnumThreadWindows(g_mainThreadId, [](HWND h, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;
        if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;  // skip owned popups
        char title[128] = ""; GetWindowTextA(h, title, sizeof(title));
        if (title[0] == '\0') return TRUE;
        reinterpret_cast<Ctx*>(lp)->found = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// Subclass proc on the shell window. WM_LAUNCHER_GO arrives at the top of the
// shell's own message loop (clean stack), which is exactly where a menu "start
// game" command would be handled - the right context for the OnLoadScript +
// ActivateGame launch (see DoLaunch / engine.h LaunchMission).
static LRESULT CALLBACK ShellSubclass(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_LAUNCHER_GO) {
        // Restore the original wndproc first so the nested picker loop (and the
        // game hand-off) never re-enter us.
        SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)g_origShellProc);
        if (!g_load.empty()) {
            // a load is used once: clear it so a later launch doesn't load it again
            WritePrivateProfileStringA(g_section, "Load", nullptr, g_iniPath);
            if (g_oneShot) DisableSelf();
            DoLoad(g_load);
            return 0;
        }
        std::string target = g_auto ? g_mission : ShowPicker();
        // One-shot disable after this session (so the toggle resets even if the launch
        // fails/crashes), for BOTH auto launches and picker sessions. A manual picker
        // setup with OneShot=0 stays enabled for repeated use.
        if (g_oneShot) DisableSelf();
        DoLaunch(target);
        return 0;
    }
    return CallWindowProcA(g_origShellProc, h, msg, wParam, lParam);
}

// Thread-local message hook, installed on the main thread in InitMod. Used only
// to detect when the shell window exists; the actual launch is posted to the
// shell so it runs in a clean context rather than nested inside this hook.
static LRESULT CALLBACK GetMsgHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && !g_fired) {
        HWND shell = FindShellWindow();
        if (shell) {
            if (g_shellFirstSeenTick == 0) {
                g_shellFirstSeenTick = GetTickCount();
                char cls[64] = ""; GetClassNameA(shell, cls, sizeof(cls));
                char ttl[128] = ""; GetWindowTextA(shell, ttl, sizeof(ttl));
                Log(std::string("shell window seen: class='") + cls + "' title='" + ttl + "'");
            } else if (GetTickCount() - g_shellFirstSeenTick >= kSettleMs) {
                g_fired = true;
                g_origShellProc = (WNDPROC)SetWindowLongPtrA(shell, GWLP_WNDPROC, (LONG_PTR)ShellSubclass);
                PostMessageA(shell, WM_LAUNCHER_GO, 0, 0);
                if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
                Log("subclassed shell, posted launch trigger");
            }
        }
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void InitMod(const char* iniSectionName) {
    if (iniSectionName && *iniSectionName)
        strncpy_s(g_section, iniSectionName, _TRUNCATE);

    ResolveGameDir();
    Log("=== Launcher InitMod (section '" + std::string(g_section) + "') ===");
    Log("gameDir='" + std::string(g_gameDir) + "' ini='" + std::string(g_iniPath) + "'");

    void* exeBase = GetModuleHandleA("Outpost2.exe");
    if (reinterpret_cast<uintptr_t>(exeBase) != op2::kExpectedExeBase) {
        char b[96]; sprintf_s(b, "WARNING: Outpost2.exe base is %p, expected 0x%08X - addresses may be wrong",
                              exeBase, (unsigned)op2::kExpectedExeBase);
        Log(b);
    }

    ReadConfig();

    // The everyday on/off. op2ext loaded us (because [ExternalModules] Launcher=yes),
    // but if the user has [Launcher] Enabled=0 we do nothing - clean normal play.
    if (!g_enabled) {
        Log("Enabled=0 - Launcher inactive, doing nothing.");
        return;
    }

    if (!g_auto && g_mission.empty())
        Log("Auto off and no Mission set - picker will be shown when the shell appears.");

    g_mainThreadId = GetCurrentThreadId();
    g_hook = SetWindowsHookExA(WH_GETMESSAGE, GetMsgHook, nullptr, g_mainThreadId);
    Log(g_hook ? "trigger hook installed" : "ERROR: failed to install trigger hook");
}

extern "C" __declspec(dllexport) void DestroyMod() {
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    Log("=== Launcher DestroyMod ===");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_hInst = (HINSTANCE)hModule;
    }
    return TRUE;
}
