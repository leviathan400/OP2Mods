// picker.cpp - "our debug menu": a real Win32 dialog that lists every mission DLL the game
// can load and lets you pick one or type ANY filename, then launch it.
//
// Where the missions are: under OPU most mission DLLs are NOT next to Outpost2.exe. OPU keeps
// them in per-map subfolders - OPU\base\maps\<map>\ (campaign, tutorial, colony and the original
// multiplayer maps) and OPU\maps\<map>\ (extra maps, LUA missions, official demos, ...) - and the
// game resolves a bare name like "c_ashes.dll" from those folders itself. So the list is built
// from the exe folder, the OPU folder and both maps trees (all subfolders), keeping only DLLs
// that export DescBlock (every mission DLL does; engine/module DLLs don't), one entry per name.
// That matches the engine's own "PICK A SCRIPT" list.
//
// Filter box (focused when the dialog opens): as you type, the list shows only the missions
// whose name starts with the text (case-insensitive), and its caption shows "N of M". Enter
// launches the name in the "type any DLL name" box, else the list's selection, else the only
// match left.
//
// Mission details: when a list entry stays selected for half a second, the panel under the
// buttons shows what the DLL says about itself - its LevelDesc (the mission's name), the
// DescBlock (mission type, players, max tech level, unit mission), MapName and TechtreeName -
// plus where the file is. All read straight from the file's exports (see PeFile); the DLL is
// never loaded, so none of its code runs. The delay keeps arrowing through the list fast.
//
// It is a normal DialogBox (template IDD_PICKER in Launcher.dll). We bracket it with
// odasl's wplEnable()/wplDisable() - OP2Shell.dll has already loaded + initialised
// odasl in-process, so its CBT hook auto-skins the dialog + controls to the OP2 look
// (navy caption, red-bordered buttons), exactly like the engine's own "PICK A SCRIPT"
// dialog. odasl draws its own caption, so there is no Windows title-bar icon.
//
// Returns the chosen DLL filename, or "" if the user cancelled.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>
#include "resource.h"
#include "engine.h"

static std::string g_result;

// odasl skin toggles (__stdcall, no args).
typedef void(__stdcall* WplFn)(void);

// The folder Outpost2.exe is in, with a trailing backslash. Under OPU this is the install root
// (the original game files: the classic campaign/colony DLLs sit here too).
static std::string ExeDir() {
    char exe[MAX_PATH] = ""; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string s(exe); size_t sl = s.find_last_of("\\/");
    return (sl == std::string::npos) ? "" : s.substr(0, sl + 1);
}

// The OPU runtime folder, with a trailing backslash: OPULauncher starts the game with the
// current directory set to it (the same rule launcher.cpp's ResolveGameDir uses).
static std::string OpuDir() {
    char dir[MAX_PATH] = ""; GetCurrentDirectoryA(MAX_PATH, dir);
    std::string s(dir);
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
    return s;
}

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// ---- reading a DLL's exports from the file -------------------------------------------------
// A read-only file mapping of a 32-bit PE plus bounds-checked lookups. Nothing is loaded and
// no code runs; a malformed or truncated file simply finds nothing.
class PeFile {
public:
    explicit PeFile(const std::string& path) {
        f_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f_ == INVALID_HANDLE_VALUE) return;
        size_ = GetFileSize(f_, nullptr);
        if (size_ < 0x200 || size_ == INVALID_FILE_SIZE) return;
        m_ = CreateFileMappingA(f_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (m_) b_ = (const BYTE*)MapViewOfFile(m_, FILE_MAP_READ, 0, 0, 0);
        if (!b_) return;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)b_;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || !In(dos->e_lfanew, sizeof(IMAGE_NT_HEADERS32))) return;
        nt_ = (const IMAGE_NT_HEADERS32*)(b_ + dos->e_lfanew);
        if (nt_->Signature != IMAGE_NT_SIGNATURE || nt_->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            { nt_ = nullptr; return; }
        sec_ = IMAGE_FIRST_SECTION(nt_);
        nsec_ = nt_->FileHeader.NumberOfSections;
        if (!In((DWORD)((const BYTE*)sec_ - b_), nsec_ * sizeof(IMAGE_SECTION_HEADER))) nt_ = nullptr;
    }
    ~PeFile() {
        if (b_) UnmapViewOfFile(b_);
        if (m_) CloseHandle(m_);
        if (f_ != INVALID_HANDLE_VALUE) CloseHandle(f_);
    }

    // File offset of the data an export points at (e.g. the DescBlock struct or the LevelDesc
    // char array), or 0 when there is no such export or its data isn't in the file (e.g. .bss).
    DWORD ExportData(const char* name) const {
        if (!nt_ || nt_->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return 0;
        DWORD edOff = RvaToOff(nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
        if (!edOff || !In(edOff, sizeof(IMAGE_EXPORT_DIRECTORY))) return 0;
        const IMAGE_EXPORT_DIRECTORY* ed = (const IMAGE_EXPORT_DIRECTORY*)(b_ + edOff);
        DWORD namesOff = RvaToOff(ed->AddressOfNames), ordsOff = RvaToOff(ed->AddressOfNameOrdinals);
        DWORD funcsOff = RvaToOff(ed->AddressOfFunctions);
        if (!namesOff || !ordsOff || !funcsOff || !In(namesOff, ed->NumberOfNames * 4) ||
            !In(ordsOff, ed->NumberOfNames * 2) || !In(funcsOff, ed->NumberOfFunctions * 4)) return 0;
        size_t want = strlen(name) + 1;                       // compare including the NUL
        for (DWORD i = 0; i < ed->NumberOfNames; i++) {
            DWORD noff = RvaToOff(((const DWORD*)(b_ + namesOff))[i]);
            if (!noff || !In(noff, (DWORD)want) || memcmp(b_ + noff, name, want) != 0) continue;
            WORD ord = ((const WORD*)(b_ + ordsOff))[i];      // index into AddressOfFunctions
            if (ord >= ed->NumberOfFunctions) return 0;
            return RvaToOff(((const DWORD*)(b_ + funcsOff))[ord]);
        }
        return 0;
    }
    bool HasExport(const char* name) const {                  // exported AND has file data
        return ExportData(name) != 0;
    }

    // The NUL-terminated string an export points at (exported char arrays such as LevelDesc,
    // MapName, TechtreeName), or "" when missing or not plain printable text.
    std::string ExportString(const char* name, size_t maxLen = 200) const {
        DWORD off = ExportData(name);
        std::string s;
        for (; off && In(off, 1) && s.size() < maxLen; off++) {
            unsigned char c = b_[off];
            if (!c) return s;
            if (c < 32 && c != '\t') return "";               // not text (e.g. a pointer)
            s += (char)c;
        }
        return "";                                            // ran off the file / too long
    }

    // Read `n` bytes of an export's data into `out`; false when missing or truncated.
    bool ExportBytes(const char* name, void* out, DWORD n) const {
        DWORD off = ExportData(name);
        if (!off || !In(off, n)) return false;
        memcpy(out, b_ + off, n);
        return true;
    }

private:
    // true when [off, off+len) lies inside the file (every read is checked with it)
    bool In(DWORD off, DWORD len) const { return off <= size_ && len <= size_ - off; }
    // file offset of an RVA via the section table; 0 when it isn't backed by file data
    DWORD RvaToOff(DWORD rva) const {
        if (!rva) return 0;
        for (WORD i = 0; i < nsec_; i++) {
            DWORD va = sec_[i].VirtualAddress;
            if (rva >= va && rva < va + sec_[i].SizeOfRawData) {
                DWORD off = sec_[i].PointerToRawData + (rva - va);
                return In(off, 1) ? off : 0;
            }
        }
        return 0;
    }

    HANDLE f_ = INVALID_HANDLE_VALUE, m_ = nullptr;
    const BYTE* b_ = nullptr;
    DWORD size_ = 0;
    const IMAGE_NT_HEADERS32* nt_ = nullptr;
    const IMAGE_SECTION_HEADER* sec_ = nullptr;
    WORD nsec_ = 0;
};

// ---- finding the missions -------------------------------------------------------------------

struct Mission { std::string name, path; };                  // display name + full path

// Add every mission DLL in `dir` (and, with `recurse`, all its subfolders) to `out`, keyed by
// lower-case name. A name already present is skipped - the first place it's found wins, as the
// same DLL often sits both next to the exe and in OPU\base\maps\<map>\.
static void AddMissions(const std::string& dir, bool recurse, std::map<std::string, Mission>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;                    // folder missing: nothing to add
    do {
        std::string name(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recurse && name != "." && name != "..") AddMissions(dir + name + "\\", true, out);
            continue;
        }
        if (name.size() < 5 || _stricmp(name.c_str() + name.size() - 4, ".dll") != 0) continue;
        std::string key = Lower(name);
        if (out.count(key)) continue;
        if (!PeFile(dir + name).HasExport("DescBlock")) continue;   // engine/module DLL, not a mission
        out[key] = Mission{ name, dir + name };
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// All mission DLLs the game can load by bare name, sorted case-insensitively (as the engine's
// own list is). Anything not listed can still be typed into the edit box.
static std::vector<Mission> ListMissions() {
    std::map<std::string, Mission> found;                     // keyed by lower-case name
    const std::string exe = ExeDir(), opu = OpuDir();
    AddMissions(exe, false, found);                           // classic DLLs next to Outpost2.exe
    if (_stricmp(opu.c_str(), exe.c_str()) != 0)
        AddMissions(opu, false, found);                       // the OPU folder itself
    AddMissions(opu + "base\\maps\\", true, found);           // OPU's original maps
    AddMissions(opu + "maps\\", true, found);                 // extra / community maps
    std::vector<Mission> out;
    for (auto& kv : found) out.push_back(kv.second);          // std::map: already sorted by lower-case name
    return out;
}

// ---- mission details ----------------------------------------------------------------------

// The four ints every mission DLL exports as DescBlock.
struct DescBlock { int missionType, numPlayers, maxTechLevel, unitMission; };

// DescBlock.missionType: 1..12 = campaign mission number; negative = the game type.
static std::string MissionTypeText(int t) {
    char buf[48];
    if (t > 0) { wsprintfA(buf, "Campaign mission %d", t); return buf; }
    switch (t) {
    case -1: return "Colony game";
    case -2: return "Auto-demo";
    case -3: return "Tutorial";
    case -4: return "Multiplayer - Land Rush";
    case -5: return "Multiplayer - Space Race";
    case -6: return "Multiplayer - Resource Race";
    case -7: return "Multiplayer - Midas";
    case -8: return "Multiplayer - Last One Standing";
    }
    wsprintfA(buf, "Unknown (%d)", t);
    return buf;
}

// The details text for one mission DLL, read from its exports: the mission's name first, then
// each field on two lines - its label, then its value underneath - e.g.
//     Type:
//     Colony game
static std::string MissionDetails(const Mission& m) {
    PeFile pe(m.path);
    std::string level = pe.ExportString("LevelDesc");
    std::string out = level.empty() ? "(no LevelDesc)" : level;
    out += "\r\n";
    auto field = [&out](const char* label, const std::string& value) {
        out += "\r\n"; out += label; out += "\r\n"; out += value;
    };
    DescBlock db;
    if (pe.ExportBytes("DescBlock", &db, sizeof(db))) {
        field("Type:", MissionTypeText(db.missionType));
        field("Players:", std::to_string(db.numPlayers));
        field("Max tech level:", std::to_string(db.maxTechLevel));
        if (db.unitMission) field("Unit mission:", "yes");
    }
    std::string map = pe.ExportString("MapName"), tech = pe.ExportString("TechtreeName");
    if (!map.empty())  field("Map:", map);
    if (!tech.empty()) field("Tech tree:", tech);
    // where the file is, relative to the OPU folder (or the exe folder) when it's inside one
    std::string path = m.path, opu = OpuDir(), exe = ExeDir();
    if (_strnicmp(path.c_str(), opu.c_str(), opu.size()) == 0)      path = "OPU\\" + path.substr(opu.size());
    else if (_strnicmp(path.c_str(), exe.c_str(), exe.size()) == 0) path = path.substr(exe.size()) + "  (game folder)";
    out += "\r\n";
    field("File:", path);
    return out;
}

// ---- the dialog ---------------------------------------------------------------------------

static const UINT_PTR kDetailsTimer = 1;                     // fires 500 ms after the last selection change
static const UINT     kDetailsDelayMs = 500;

static void CenterOnScreen(HWND hDlg) {
    RECT rc; GetWindowRect(hDlg, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(hDlg, HWND_TOP,
                 (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - h) / 2, 0, 0, SWP_NOSIZE);
}

// Accept `name` as the result and close, unless it's too long for GameStartInfo.scriptName
// (a list entry can exceed the limit even though typing can't). Returns true if accepted.
static bool TryAccept(HWND hDlg, const char* name) {
    if (!op2::ScriptNameFits(name)) {
        char msg[160];
        wsprintfA(msg, "\"%s\" is too long.\n\nMission DLL names can be at most %d characters.",
                  name, (int)op2::kMaxScriptNameLen);
        MessageBoxA(hDlg, msg, "Launch a DLL", MB_OK | MB_ICONWARNING);
        return false;
    }
    g_result = name; EndDialog(hDlg, 1);
    return true;
}

// Every mission found when the dialog opened; the list box shows the ones matching the filter.
static std::vector<Mission> g_all;

static const Mission* FindMission(const char* name) {
    for (const auto& m : g_all) if (_stricmp(m.name.c_str(), name) == 0) return &m;
    return nullptr;
}

static void SetInfo(HWND hDlg, const char* text);           // below: clean repaint of the details label

// Refill the list box with the missions whose name STARTS with the Filter box's text
// (case-insensitive; an empty filter shows all), and put the count in the list's caption.
// Redraw is switched off while refilling so the list doesn't flicker on every keystroke.
static void ApplyFilter(HWND hDlg) {
    char filter[64] = "";
    GetDlgItemTextA(hDlg, IDC_FILTER, filter, sizeof(filter));
    size_t flen = strlen(filter);
    HWND list = GetDlgItem(hDlg, IDC_LIST);
    SendMessageA(list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(list, LB_RESETCONTENT, 0, 0);
    int shown = 0;
    for (const auto& m : g_all) {
        if (flen && _strnicmp(m.name.c_str(), filter, flen) != 0) continue;
        SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)m.name.c_str());
        shown++;
    }
    SendMessageA(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
    char label[96];
    if (flen) wsprintfA(label, "Mission / scenario DLLs (%d of %d):", shown, (int)g_all.size());
    else      wsprintfA(label, "Mission / scenario DLLs (%d):", (int)g_all.size());
    SetDlgItemTextA(hDlg, IDC_LISTLABEL, label);
    KillTimer(hDlg, kDetailsTimer);                           // the selection is gone
    SetInfo(hDlg, "");
}

// Show `text` in the details panel, one label per line ("\r\n"-separated; past INFO_ROWS lines
// is dropped).
//
// The labels are DESTROYED AND RE-CREATED every time, never given new text. Changing a skinned
// static's text (WM_SETTEXT) makes the control repaint itself at once with plain Windows drawing -
// a white background - bypassing the skin, and nothing erases the old text first, so the end of
// a longer previous line stayed visible. Destroying a label instead makes the dialog repaint its
// background there, wiping the old text. The new labels are created after odasl skinned the
// dialog, so the skin leaves them alone; they ask the dialog for colours (WM_CTLCOLORSTATIC in
// DlgProc), which answers "transparent, in the skin's label colour" - so they look like the
// skinned labels and show the dialog's picture behind them.
static void SetInfo(HWND hDlg, const char* text) {
    for (int row = 0; row < INFO_ROWS; row++) {               // remove the previous mission's rows
        HWND old = GetDlgItem(hDlg, IDC_INFO0 + row);
        if (old) DestroyWindow(old);
    }
    HFONT font = (HFONT)SendDlgItemMessageA(hDlg, IDC_LISTLABEL, WM_GETFONT, 0, 0);  // the dialog's font
    const char* p = text;
    for (int row = 0; row < INFO_ROWS && p && *p; row++) {
        const char* end = strstr(p, "\r\n");
        std::string line = end ? std::string(p, end) : std::string(p);
        p = end ? end + 2 : nullptr;                          // next line, or none left
        if (line.empty()) continue;                           // blank line: just leave the gap
        RECT rc = { 228, 56 + row * 9, 228 + 184, 56 + row * 9 + 9 };   // dialog units, as in picker.rc
        MapDialogRect(hDlg, &rc);                             // -> pixels
        HWND lbl = CreateWindowExA(0, "STATIC", line.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                   rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                   hDlg, (HMENU)(INT_PTR)(IDC_INFO0 + row), GetModuleHandleA(nullptr), nullptr);
        if (lbl && font) SendMessageA(lbl, WM_SETFONT, (WPARAM)font, FALSE);
    }
}

static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        // cap typed names to what GameStartInfo.scriptName[32] can hold
        SendDlgItemMessageA(hDlg, IDC_EDIT, EM_LIMITTEXT, op2::kMaxScriptNameLen, 0);
        SendDlgItemMessageA(hDlg, IDC_FILTER, EM_LIMITTEXT, op2::kMaxScriptNameLen, 0);
        g_all = ListMissions();
        // Show keyboard cues (underlines, focus rect) from the start. Otherwise the first Alt
        // press switches them on, and every label repaints in plain Windows style - the white
        // patches over the skin's background.
        SendMessageA(hDlg, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEACCEL | UISF_HIDEFOCUS), 0);
        ApplyFilter(hDlg);                                   // empty filter: everything
        CenterOnScreen(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_FILTER));               // start typing straight away
        return FALSE;                                        // FALSE: we set the focus ourselves
    }
    case WM_CTLCOLORSTATIC: {
        // The details rows are created at run time, after odasl skinned the dialog, so the skin
        // doesn't draw them: a plain static asks its parent (us) for colours, and the default
        // answer is Windows' grey/white box. Answer for those rows: transparent background (a
        // hollow brush, so the dialog's picture shows through) and the skin's label colour.
        int id = GetDlgCtrlID((HWND)lParam);
        if (id >= IDC_INFO0 && id < IDC_INFO0 + INFO_ROWS) {
            HDC dc = (HDC)wParam;
            SetTextColor(dc, RGB(181, 210, 152));            // the skin's label text colour
            SetBkMode(dc, TRANSPARENT);
            return (INT_PTR)GetStockObject(HOLLOW_BRUSH);
        }
        return FALSE;                                        // skinned controls: the skin's colours
    }
    case WM_TIMER:
        if (wParam == kDetailsTimer) {
            // The selection has held for 500 ms: show that DLL's details.
            KillTimer(hDlg, kDetailsTimer);
            HWND list = GetDlgItem(hDlg, IDC_LIST);
            int sel = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
            char buf[256] = "";
            if (sel != LB_ERR) SendMessageA(list, LB_GETTEXT, sel, (LPARAM)buf);
            const Mission* m = buf[0] ? FindMission(buf) : nullptr;
            SetInfo(hDlg, m ? MissionDetails(*m).c_str() : "");
            return TRUE;
        }
        return FALSE;
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (id == IDC_FILTER && code == EN_CHANGE) {          // every keystroke in Filter
            ApplyFilter(hDlg);
            // A name picked earlier that no longer matches the filter is cleared, so Enter
            // doesn't launch something that has disappeared from the list.
            char filter[64] = "", name[256] = "";
            GetDlgItemTextA(hDlg, IDC_FILTER, filter, sizeof(filter));
            GetDlgItemTextA(hDlg, IDC_EDIT, name, sizeof(name));
            if (name[0] && _strnicmp(name, filter, strlen(filter)) != 0)
                SetDlgItemTextA(hDlg, IDC_EDIT, "");
            return TRUE;
        }
        if (id == IDC_LIST && (code == LBN_SELCHANGE || code == LBN_DBLCLK)) {
            HWND list = GetDlgItem(hDlg, IDC_LIST);
            int sel = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                char buf[256] = "";
                SendMessageA(list, LB_GETTEXT, sel, (LPARAM)buf);
                SetDlgItemTextA(hDlg, IDC_EDIT, buf);
                if (code == LBN_DBLCLK) { TryAccept(hDlg, buf); return TRUE; }
                // (Re)start the details delay: only a selection that stays put is read.
                SetInfo(hDlg, "");
                SetTimer(hDlg, kDetailsTimer, kDetailsDelayMs, nullptr);
            }
            return TRUE;
        }
        if (id == IDC_LAUNCH) {                               // LAUNCH button, or Enter anywhere
            char buf[256] = ""; GetDlgItemTextA(hDlg, IDC_EDIT, buf, sizeof(buf));
            if (!buf[0]) {
                // Nothing typed or picked: launch the list's selection, or the only match
                // left by the filter (type "t14", press Enter).
                HWND list = GetDlgItem(hDlg, IDC_LIST);
                int sel = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
                if (sel == LB_ERR && SendMessageA(list, LB_GETCOUNT, 0, 0) == 1) sel = 0;
                if (sel != LB_ERR) SendMessageA(list, LB_GETTEXT, sel, (LPARAM)buf);
            }
            if (buf[0]) TryAccept(hDlg, buf);
            else MessageBeep(MB_ICONWARNING);                 // nothing to launch
            return TRUE;
        }
        if (id == IDCANCEL) { g_result.clear(); EndDialog(hDlg, 0); return TRUE; }
        return FALSE;
    }
    case WM_CLOSE: g_result.clear(); EndDialog(hDlg, 0); return TRUE;
    }
    return FALSE;
}

std::string ShowPicker() {
    g_result.clear();

    // Our own module handle (the dialog template lives in Launcher.dll, not the exe).
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ShowPicker, &self);

    HWND parent = FindWindowA("Outpost2Shell", nullptr);

    // Turn OP2's odasl skin on around the dialog (it auto-skins #32770 dialogs).
    WplFn enable = nullptr, disable = nullptr;
    HMODULE odasl = GetModuleHandleA("odasl.dll");
    if (odasl) {
        enable  = (WplFn)GetProcAddress(odasl, "wplEnable");
        disable = (WplFn)GetProcAddress(odasl, "wplDisable");
    }

    if (enable) enable();
    DialogBoxParamA((HINSTANCE)self, MAKEINTRESOURCEA(IDD_PICKER), parent, DlgProc, 0);
    if (disable) disable();

    return g_result;
}
