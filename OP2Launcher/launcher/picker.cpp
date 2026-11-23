// picker.cpp - "our debug menu": a real Win32 dialog that lists every DLL in the OP2
// folder and lets you pick one or type ANY filename, then launch it.
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
#include <algorithm>
#include <cctype>
#include "resource.h"
#include "engine.h"

static std::string g_result;

// odasl skin toggles (__stdcall, no args).
typedef void(__stdcall* WplFn)(void);

// Engine/runtime DLLs that are never missions - hidden from the list (the user can
// still type them if they really want).
static bool IsEngineDll(const std::string& lower) {
    static const char* skip[] = {
        "op2ext.dll", "launcher.dll", "op2shres.dll", "out2res.dll", "op2shell.dll",
        "winmm.dll", "wsock32.dll", "dpwsockx.dll", "helphook.dll", "opupatch.dll",
        "nethelper.dll", "capstone.dll", "odasl.dll"
    };
    for (const char* s : skip) if (lower == s) return true;
    return false;
}

static std::string GameDir() {
    char exe[MAX_PATH] = ""; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string s(exe); size_t sl = s.find_last_of("\\/");
    return (sl == std::string::npos) ? "" : s.substr(0, sl + 1);
}

static std::vector<std::string> ListDlls() {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((GameDir() + "*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string name(fd.cFileName), lower(name);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (!IsEngineDll(lower)) out.push_back(name);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(),
              [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

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

static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        // cap typed names to what GameStartInfo.scriptName[32] can hold
        SendDlgItemMessageA(hDlg, IDC_EDIT, EM_LIMITTEXT, op2::kMaxScriptNameLen, 0);
        HWND list = GetDlgItem(hDlg, IDC_LIST);
        for (const auto& dll : ListDlls())
            SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)dll.c_str());
        CenterOnScreen(hDlg);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (id == IDC_LIST && (code == LBN_SELCHANGE || code == LBN_DBLCLK)) {
            HWND list = GetDlgItem(hDlg, IDC_LIST);
            int sel = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                char buf[256] = "";
                SendMessageA(list, LB_GETTEXT, sel, (LPARAM)buf);
                SetDlgItemTextA(hDlg, IDC_EDIT, buf);
                if (code == LBN_DBLCLK) TryAccept(hDlg, buf);
            }
            return TRUE;
        }
        if (id == IDC_LAUNCH) {
            char buf[256] = ""; GetDlgItemTextA(hDlg, IDC_EDIT, buf, sizeof(buf));
            TryAccept(hDlg, buf); return TRUE;
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
