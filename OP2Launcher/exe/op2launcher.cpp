// op2launcher.cpp - source for op2launcher.exe ("OP2Launcher"), the OPU launch tool.
//
// It installs the Launcher op2ext module into outpost2.ini if missing, then boots OPU
// straight into a mission DLL (or into the in-game picker). It can also uninstall.
//
// Cross-platform: Windows (native) and Linux (where OPU runs under Wine).
//
// PLACEMENT: drop op2launcher.exe (+ optional op2launcher.ini) in the OPU folder, next
// to OPULauncher.exe and outpost2.ini. It self-locates them via its own dir.
//
// USAGE:
//   op2launcher <mission.dll>     e.g.  op2launcher e04.dll
//   op2launcher                   (uses [Launcher] Dll= from op2launcher.ini)
//   op2launcher --picker          (boot into the in-game "Launch a DLL" menu; Auto=0)
//   op2launcher --load <save>     (boot into a saved game, e.g. saves\SGAME0.OP2; relative
//                                  to the OPU folder, or absolute)
//   op2launcher --uninstall       (remove Launcher from outpost2.ini; keeps Launcher.dll)
//   op2launcher --help            (usage; also -h, -?, /?)
// An unknown option (anything starting with '-', or a short /x) or an extra argument prints
// an error + the usage and exits with code 1 WITHOUT changing the ini or starting the game.
//
// WHAT IT DOES, in outpost2.ini:
//   0. INSTALL (only if missing): adds [ExternalModules] Launcher=yes and a default
//      [Launcher] section, so op2ext will load the module.
//   1. [Launcher] Enabled=1             - turns the module on (the everyday switch).
//      [Launcher] Mission=<dll>, Auto=1, OneShot=1 - boot into <dll>, one-shot.
//   2. Starts OPULauncher.exe (via `wine` on Linux). The Launcher module then boots
//      OPU straight into <dll>, and (OneShot) sets Enabled=0 again so the next normal
//      launch is clean.

#include <string>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <fstream>
  #include <vector>
  #include <cctype>
  #include <unistd.h>
  #include <limits.h>
  #include <sys/types.h>
  #include <sys/stat.h>
#endif

// ---- platform helpers ------------------------------------------------------

// Directory of this executable, with trailing separator.
static std::string SelfDir() {
#ifdef _WIN32
    char p[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p);
    size_t sl = s.find_last_of("\\/");
    return (sl == std::string::npos) ? "" : s.substr(0, sl + 1);
#else
    char p[PATH_MAX] = "";
    ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
    if (n <= 0) return "./";
    p[n] = '\0';
    std::string s(p);
    size_t sl = s.find_last_of('/');
    return (sl == std::string::npos) ? "./" : s.substr(0, sl + 1);
#endif
}

static bool FileExists(const std::string& path) {
#ifdef _WIN32
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

#ifndef _WIN32
static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}
static bool IEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}
// Portable section/key=value setter (creates section/key if missing, preserves the rest).
static bool SetIniPortable(const std::string& path, const std::string& section,
                           const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    { std::ifstream in(path); std::string l; while (std::getline(in, l)) lines.push_back(l); }

    const std::string hdr = "[" + section + "]";
    int secStart = -1, secEnd = (int)lines.size();
    for (int i = 0; i < (int)lines.size(); ++i) {
        std::string t = Trim(lines[i]);
        if (!t.empty() && t[0] == '[') {
            if (secStart < 0 && IEq(t, hdr)) secStart = i;
            else if (secStart >= 0) { secEnd = i; break; }
        }
    }
    const std::string kv = key + "=" + value;
    if (secStart < 0) {
        if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
        lines.push_back(hdr);
        lines.push_back(kv);
    } else {
        bool replaced = false;
        for (int i = secStart + 1; i < secEnd; ++i) {
            std::string t = Trim(lines[i]);
            size_t eq = t.find('=');
            if (eq != std::string::npos && IEq(Trim(t.substr(0, eq)), key)) {
                lines[i] = kv; replaced = true; break;
            }
        }
        if (!replaced) lines.insert(lines.begin() + secEnd, kv);
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    for (const auto& l : lines) out << l << "\n";
    return true;
}
// Remove a single key from a section (no-op if absent).
static bool RemoveIniKeyPortable(const std::string& path, const std::string& section,
                                 const std::string& key) {
    std::vector<std::string> in, out;
    { std::ifstream f(path); std::string l; while (std::getline(f, l)) in.push_back(l); }
    bool inSec = false;
    const std::string hdr = "[" + section + "]";
    for (const auto& raw : in) {
        std::string t = Trim(raw);
        if (!t.empty() && t[0] == '[') { inSec = IEq(t, hdr); out.push_back(raw); continue; }
        if (inSec) {
            size_t eq = t.find('=');
            if (eq != std::string::npos && IEq(Trim(t.substr(0, eq)), key)) continue;  // drop
        }
        out.push_back(raw);
    }
    std::ofstream o(path, std::ios::trunc); if (!o) return false;
    for (const auto& l : out) o << l << "\n"; return true;
}
// Remove a whole section (its header + body, up to the next section or EOF).
static bool RemoveIniSectionPortable(const std::string& path, const std::string& section) {
    std::vector<std::string> in, out;
    { std::ifstream f(path); std::string l; while (std::getline(f, l)) in.push_back(l); }
    bool inTarget = false;
    const std::string hdr = "[" + section + "]";
    for (const auto& raw : in) {
        std::string t = Trim(raw);
        if (!t.empty() && t[0] == '[') { inTarget = IEq(t, hdr); if (inTarget) continue; }
        if (inTarget) continue;   // drop the target section's body
        out.push_back(raw);
    }
    std::ofstream o(path, std::ios::trunc); if (!o) return false;
    for (const auto& l : out) o << l << "\n"; return true;
}
#endif

// Read section/key from an ini, returning def if missing.
static std::string ReadIniValue(const std::string& path, const std::string& sec,
                                const std::string& key, const std::string& def) {
#ifdef _WIN32
    char b[256] = "";
    GetPrivateProfileStringA(sec.c_str(), key.c_str(), def.c_str(), b, sizeof(b), path.c_str());
    return b;
#else
    std::ifstream in(path);
    std::string line; bool inSec = false;
    const std::string hdr = "[" + sec + "]";
    while (std::getline(in, line)) {
        std::string t = Trim(line);
        if (!t.empty() && t[0] == '[') { inSec = IEq(t, hdr); continue; }
        if (inSec) {
            size_t eq = t.find('=');
            if (eq != std::string::npos && IEq(Trim(t.substr(0, eq)), key))
                return Trim(t.substr(eq + 1));
        }
    }
    return def;
#endif
}

static bool SetIni(const std::string& path, const char* sec, const char* key, const char* val) {
#ifdef _WIN32
    return WritePrivateProfileStringA(sec, key, val, path.c_str()) != 0;
#else
    return SetIniPortable(path, sec, key, val);
#endif
}

// Remove a key (WritePrivateProfileString with a NULL value deletes the key on Windows).
static bool RemoveIniKey(const std::string& path, const char* sec, const char* key) {
#ifdef _WIN32
    return WritePrivateProfileStringA(sec, key, nullptr, path.c_str()) != 0;
#else
    return RemoveIniKeyPortable(path, sec, key);
#endif
}
// Remove a whole section (NULL key + NULL value deletes the section on Windows).
static bool RemoveIniSection(const std::string& path, const char* sec) {
#ifdef _WIN32
    return WritePrivateProfileStringA(sec, nullptr, nullptr, path.c_str()) != 0;
#else
    return RemoveIniSectionPortable(path, sec);
#endif
}

// Unregister the module: drop the [ExternalModules] Launcher flag and the whole
// [Launcher] section. Leaves Launcher.dll on disk - this only edits the ini.
static void Uninstall(const std::string& ini) {
    RemoveIniKey(ini, "ExternalModules", "Launcher");
    RemoveIniSection(ini, "Launcher");
    printf("Uninstalled Launcher from outpost2.ini. (Launcher.dll left in place.)\n");
}

// One-time install: register the module + write a default [Launcher] section, but
// only if it isn't already there (we never clobber an existing setup).
static void EnsureInstalled(const std::string& ini) {
    if (!ReadIniValue(ini, "ExternalModules", "Launcher", "").empty()) {
        // printf("Launcher already registered in outpost2.ini.\n");  // (debug-only, quieted)
        return;
    }
    printf("Installing Launcher into outpost2.ini (first-time setup)...\n");
    SetIni(ini, "ExternalModules", "Launcher",   "yes");   // op2ext load flag
    SetIni(ini, "Launcher", "Dll",        "Launcher.dll"); // module DLL (in the OPU folder)
    SetIni(ini, "Launcher", "Enabled",    "0");            // everyday on/off (off until launched)
    SetIni(ini, "Launcher", "Mission",    "");
    SetIni(ini, "Launcher", "Auto",       "0");
    SetIni(ini, "Launcher", "OneShot",    "0");
    SetIni(ini, "Launcher", "PlayerName", "Player");
    SetIni(ini, "Launcher", "GameSpeed",  "8");
}

// Read [Launcher] Dll= from op2launcher.ini (the launcher's own config).
static std::string ReadDefaultDll(const std::string& selfIni) {
    return ReadIniValue(selfIni, "Launcher", "Dll", "");
}

// Start OPULauncher.exe (via `wine` on Linux), cwd = the OPU folder.
static bool LaunchOpu(const std::string& opu, const std::string& dir) {
#ifdef _WIN32
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::string cmd = "\"" + opu + "\"";
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr,
                        dir.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (chdir(dir.c_str()) != 0) _exit(126);
        execlp("wine", "wine", opu.c_str(), (char*)nullptr);
        _exit(127);   // exec failed (wine not installed?)
    }
    return true;      // parent: OPU runs under wine, we return
#endif
}

// ---- command line ------------------------------------------------------------

static void PrintUsage() {
    printf("Usage: op2launcher <mission.dll>   e.g. op2launcher e04.dll\n");
    printf("   or set [Launcher] Dll=<mission.dll> in op2launcher.ini and run op2launcher\n");
    printf("   op2launcher --picker             boot into the in-game \"Launch a DLL\" menu\n");
    printf("   op2launcher --load <save>        boot into a saved game, e.g. saves\\SGAME0.OP2\n");
    printf("   op2launcher --uninstall          remove Launcher from outpost2.ini (keeps the DLL)\n");
    printf("   op2launcher --help               show this help\n");
}

static bool Is(const char* a, const char* longName, const char* shortName) {
    return strcmp(a, longName) == 0 || strcmp(a, shortName) == 0;
}

// An argument that looks like an option (-x, --xyz, or /x on Windows) rather than a mission
// name. Mission names never start with these, so anything like this that isn't a known option
// is a typo - and must NOT be written to the ini as Mission= and launched.
static bool LooksLikeOption(const char* a) {
    return a[0] == '-' || (a[0] == '/' && a[1] != '\0' && strchr(a + 1, '/') == nullptr
                                       && strchr(a + 1, '\\') == nullptr && strlen(a) <= 3);
}

// ---- main ------------------------------------------------------------------

int main(int argc, char** argv) {
    std::string dir     = SelfDir();
    std::string ini     = dir + "outpost2.ini";
    std::string opu     = dir + "OPULauncher.exe";
    std::string selfIni = dir + "op2launcher.ini";

    // Check the command line BEFORE touching the ini or starting anything: a wrong argument
    // must only print an error, never launch the game.
    //   no args                 -> mission from op2launcher.ini
    //   <mission.dll>           -> exactly one non-option argument
    //   --picker | --uninstall  -> no further arguments
    //   --load <save>           -> exactly one argument after it
    //   --help / -h / -? / /?   -> usage, exit 0
    if (argc > 1) {
        const char* a = argv[1];
        if (Is(a, "--help", "-h") || strcmp(a, "-?") == 0 || strcmp(a, "/?") == 0) {
            PrintUsage();
            return 0;
        }
        bool known = Is(a, "--picker", "-p") || Is(a, "--uninstall", "-u") || Is(a, "--load", "-l");
        if (LooksLikeOption(a) && !known) {
            printf("Unknown option: %s\n\n", a);
            PrintUsage();
            return 1;
        }
        int maxArgs = Is(a, "--load", "-l") ? 3 : 2;      // program + option/mission (+ save)
        if (argc > maxArgs) {
            printf("Unexpected argument: %s\n\n", argv[maxArgs]);
            PrintUsage();
            return 1;
        }
    }

    // --uninstall: remove the module from the ini (DLL stays on disk), then exit.
    if (argc > 1 && Is(argv[1], "--uninstall", "-u")) {
        Uninstall(ini);
        return 0;
    }

    // --picker / -p: boot into the in-game "Launch a DLL" picker (Auto=0) instead of
    // auto-launching a specific mission.
    bool picker = (argc > 1 && (strcmp(argv[1], "--picker") == 0 || strcmp(argv[1], "-p") == 0));

    // --load / -l <save>: boot into a saved game instead of a mission.
    std::string load;
    if (argc > 1 && (strcmp(argv[1], "--load") == 0 || strcmp(argv[1], "-l") == 0)) {
        if (argc < 3 || argv[2][0] == '\0') {
            printf("Usage: op2launcher --load <save>   e.g. op2launcher --load saves\\SGAME0.OP2\n");
            return 1;
        }
        load = argv[2];
        std::string full = (load.size() > 1 && (load[1] == ':' || load[0] == '\\' || load[0] == '/')) ? load : dir + load;
        if (!FileExists(full)) {
            printf("Saved game not found:\n  %s\n", full.c_str());
            return 1;
        }
    }

    std::string dll;
    if (!picker && load.empty()) {
        dll = (argc > 1) ? std::string(argv[1]) : ReadDefaultDll(selfIni);
        if (dll.empty()) {
            PrintUsage();
            return 1;
        }
    }
    // The engine stores the mission name in GameStartInfo.scriptName[32] (incl. NUL) with no
    // bounds check, so reject anything longer than 31 chars up front (same limit as Launcher.dll).
    const size_t kMaxScriptNameLen = 31;
    if (!picker && load.empty() && dll.size() > kMaxScriptNameLen) {
        printf("Mission name too long (%u chars): %s\n", (unsigned)dll.size(), dll.c_str());
        printf("Outpost 2 allows at most %u characters for a mission DLL name.\n", (unsigned)kMaxScriptNameLen);
        return 1;
    }
    if (!FileExists(opu)) {
        printf("OPULauncher.exe not found next to op2launcher:\n  %s\n", opu.c_str());
        printf("Place op2launcher.exe in the OPU folder (next to OPULauncher.exe + outpost2.ini).\n");
        return 1;
    }

    // Register the module on first run (no-op if already set up).
    EnsureInstalled(ini);

    // Common: activate the module for this run, one-shot (resets to off afterward).
    SetIni(ini, "ExternalModules", "Launcher", "yes");
    SetIni(ini, "Launcher", "Dll",     "Launcher.dll");
    SetIni(ini, "Launcher", "Enabled", "1");
    SetIni(ini, "Launcher", "OneShot", "1");

    // a Load= left from a run that never reached the shell would win over this run's choice
    RemoveIniKey(ini, "Launcher", "Load");

    if (!load.empty()) {
        SetIni(ini, "Launcher", "Load", load.c_str());   // the module loads it, then clears it
        printf("Launcher enabled -> Load=%s. Starting Outpost 2 (OPULauncher.exe)...\n", load.c_str());
    } else if (picker) {
        SetIni(ini, "Launcher", "Auto", "0");   // 0 = show the picker menu in-game
        printf("Launcher enabled (picker menu). Starting Outpost 2 (OPULauncher.exe)...\n");
    } else {
        SetIni(ini, "Launcher", "Mission", dll.c_str());
        SetIni(ini, "Launcher", "Auto", "1");   // 1 = auto-launch Mission
        printf("Launcher enabled -> Mission=%s. Starting Outpost 2 (OPULauncher.exe)...\n", dll.c_str());
    }

    if (!LaunchOpu(opu, dir)) {
        printf("Failed to start OPULauncher.\n");
        return 1;
    }
    return 0;
}
