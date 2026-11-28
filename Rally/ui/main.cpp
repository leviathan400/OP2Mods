// Rally-UI  -  a live view of the Rally module's two rally points
// ===============================================================
// A small Dear ImGui (Win32 + Direct3D 11) desktop app that runs NEXT TO Outpost 2.
//
// WHERE THE DATA COMES FROM
// -------------------------
// Rally.dll, running inside Outpost2.exe, writes a snapshot into the named shared memory
// "Local\OP2Rally" every few game cycles (layout and protocol: ..\rl_shared.h). This app
// opens that mapping read-only and copies the snapshot ~10 times a second. Nothing goes the
// other way: the app never touches the game, so it can be started and closed at any time.
//
// A copy is used only if the snapshot's `seq` was even and unchanged across the copy (the
// game was not mid-write); otherwise the previous good copy stays on screen.
//
// QUITS WITH THE GAME: the snapshot carries Outpost2.exe's process id. Once the app has seen
// it, it holds a SYNCHRONIZE handle to that process and closes itself when the process ends
// (a normal quit or a crash), or when Rally reports the game shutting down. Started before
// the game, it waits for one; it only quits after it has seen a game.
//
// WHAT IT SHOWS
// -------------
//   - connection state: waiting for the game / in game (with the mark, the game's clock) / paused
//   - where the military and colony rally points are (set in game or from the ini)
//
// Build: build.bat (vswhere + MSVC, 32-bit like the module; Dear ImGui 1.91.5 sources in imgui\, MIT licence;
// the icon, Tech_Vehicle.ico, comes in through app.rc).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include "../rl_shared.h"

// ---- Direct3D 11 (the standard Dear ImGui DX11 setup) ------------------------------------
static ID3D11Device*           g_dev     = nullptr;
static ID3D11DeviceContext*    g_ctx     = nullptr;
static IDXGISwapChain*         g_swap    = nullptr;
static ID3D11RenderTargetView* g_rtv     = nullptr;
static UINT                    g_resizeW = 0, g_resizeH = 0;   // pending resize from WM_SIZE

// The swap chain's back buffer as a render target (redone after every resize).
static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}
static void CleanupRenderTarget() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

// Create the D3D11 device, context and a 2-buffer swap chain for the window.
static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)   // no GPU driver: fall back to the WARP software renderer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr != S_OK) return false;
    CreateRenderTarget();
    return true;
}
// Release everything CreateDeviceD3D made.
static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx)  { g_ctx->Release();  g_ctx  = nullptr; }
    if (g_dev)  { g_dev->Release();  g_dev  = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
// Window messages: ImGui gets them first; a resize is applied in the main loop (g_resizeW/H).
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) { g_resizeW = LOWORD(lParam); g_resizeH = HIWORD(lParam); }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;   // no Alt menu beep
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---- reading the snapshot -----------------------------------------------------------------
enum Link { LINK_WAITING, LINK_MENU, LINK_VERSION, LINK_LIVE, LINK_PAUSED, LINK_STOPPED, LINK_EXITED };

static HANDLE            g_map    = nullptr;
static const RlSnapshot* g_view   = nullptr;   // the game's mapping (read-only)
static RlSnapshot        g_snap   = {};        // last consistent copy
static bool              g_have   = false;
static int               g_lastTick = -1;      // to notice a paused game
static ULONGLONG         g_tickChangedAt = 0;
static HANDLE            g_gameProc = nullptr;  // SYNCHRONIZE handle to Outpost2.exe, once seen
static DWORD             g_gamePid  = 0;

// Open the mapping if the game has created it. Retried each poll until it exists.
static void TryOpen() {
    if (g_view) return;
    g_map = OpenFileMappingA(FILE_MAP_READ, FALSE, RL_SHM_NAME);
    if (!g_map) return;
    g_view = (const RlSnapshot*)MapViewOfFile(g_map, FILE_MAP_READ, 0, 0, sizeof(RlSnapshot));
    if (!g_view) { CloseHandle(g_map); g_map = nullptr; }
}
// Drop the mapping (the game stopped), so a new game's mapping is picked up fresh.
static void Close() {
    if (g_view) UnmapViewOfFile(g_view);
    if (g_map)  CloseHandle(g_map);
    g_view = nullptr; g_map = nullptr; g_have = false; g_lastTick = -1;
}

// Copy a consistent snapshot (seqlock read) and work out the link state.
static Link Poll() {
    TryOpen();
    if (!g_view) return LINK_WAITING;

    for (int attempt = 0; attempt < 4; attempt++) {
        LONG before = g_view->seq;
        if (before & 1) continue;                     // being written right now
        MemoryBarrier();
        RlSnapshot tmp;
        memcpy(&tmp, (const void*)g_view, sizeof(tmp));
        MemoryBarrier();
        if (g_view->seq != before) continue;          // changed under us
        g_snap = tmp; g_have = true;
        break;
    }
    if (!g_have) return LINK_WAITING;
    if (g_snap.version != RL_SHM_VERSION) return LINK_VERSION;

    // Watch the game's process; when it has ended, the app quits (see the file header).
    if (g_snap.gamePid && (DWORD)g_snap.gamePid != g_gamePid) {
        if (g_gameProc) CloseHandle(g_gameProc);
        g_gamePid  = (DWORD)g_snap.gamePid;
        g_gameProc = OpenProcess(SYNCHRONIZE, FALSE, g_gamePid);
    }
    if (g_gameProc && WaitForSingleObject(g_gameProc, 0) == WAIT_OBJECT_0) return LINK_EXITED;

    if (!g_snap.gameRunning) {
        if (g_snap.seq == 0) return LINK_MENU;       // game running, but no game started yet
        Close();
        return LINK_STOPPED;                         // Rally wrote "stopped": the game is exiting
    }
    ULONGLONG now = GetTickCount64();
    if (g_snap.tick != g_lastTick) { g_lastTick = g_snap.tick; g_tickChangedAt = now; }
    return (now - g_tickChangedAt > 1500) ? LINK_PAUSED : LINK_LIVE;
}

// ---- colours ------------------------------------------------------------------------------
static const ImVec4 C_GREEN(0.45f, 0.85f, 0.45f, 1), C_AMBER(0.95f, 0.75f, 0.30f, 1),
                    C_GREY(0.6f, 0.6f, 0.6f, 1),     C_RED(1, 0.4f, 0.4f, 1),
                    C_MIL(1.0f, 0.45f, 0.35f, 1),    C_COL(0.40f, 0.80f, 1.0f, 1);

// One line: a rally point's tile and where it came from, or "not set".
static void PointLine(const char* label, const ImVec4& col, const RlPoint& p) {
    ImGui::TextColored(col, "%s", label);
    ImGui::SameLine(90);
    if (p.set) {
        ImGui::Text("%d, %d", p.x, p.y);
        ImGui::SameLine();
        ImGui::TextDisabled(p.fromGame ? "(set in game)" : "(from the ini)");
    } else ImGui::TextDisabled("not set - those vehicles stay at the factory");
}

// ---- the window's contents ----------------------------------------------------------------
static void DrawUI(Link link, bool& onTop, HWND hwnd) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Rally", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    switch (link) {
    case LINK_WAITING: ImGui::TextColored(C_GREY,  "Waiting for Outpost 2 with Rally.dll..."); break;
    case LINK_MENU:    ImGui::TextColored(C_AMBER, "Outpost 2 is running - start a game"); break;
    case LINK_STOPPED: ImGui::TextColored(C_GREY,  "Game closed"); break;
    case LINK_EXITED:  ImGui::TextColored(C_GREY,  "Game closed"); break;
    case LINK_VERSION: ImGui::TextColored(C_RED,   "Rally.dll and this app don't match (data v%d, app v%d) - rebuild both.",
                                          g_snap.version, RL_SHM_VERSION); break;
    case LINK_LIVE:    ImGui::TextColored(C_GREEN, "In game"); break;
    case LINK_PAUSED:  ImGui::TextColored(C_AMBER, "Paused / not in a game"); break;
    }
    if (link == LINK_LIVE || link == LINK_PAUSED) {
        ImGui::SameLine();
        ImGui::TextDisabled("  mark %d  |  player %d", g_snap.tick / 100, g_snap.localPlayer);   // 1 mark = 100 ticks, the game's clock
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 105);
    if (ImGui::Checkbox("On top", &onTop))
        SetWindowPos(hwnd, onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ImGui::Separator();

    if (link != LINK_LIVE && link != LINK_PAUSED) { ImGui::End(); return; }

    PointLine("Military", C_MIL, g_snap.military);
    PointLine("Colony",   C_COL, g_snap.colony);
    ImGui::End();
}

// ---- entry point --------------------------------------------------------------------------
// Create the window (DPI-aware, icon from app.rc) and D3D11, then loop: poll the snapshot about
// 10 times a second, draw at the monitor's refresh rate, quit when the game has exited.
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, LoadIconW(hInst, MAKEINTRESOURCEW(1)),
                       LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"RallyUI", (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), 0) };   // icon: app.rc
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Rally", WS_OVERLAPPEDWINDOW, 140, 140,
                              (int)(460 * scale), (int)(150 * scale), nullptr, nullptr, hInst, nullptr);
    if (!CreateDeviceD3D(hwnd)) { CleanupDeviceD3D(); UnregisterClassW(wc.lpszClassName, hInst); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                       // nothing to save; the window fills the client area
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    io.FontGlobalScale = scale;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    bool onTop = false;
    Link link = LINK_WAITING;
    ULONGLONG nextPoll = 0;
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resizeW && g_resizeH) {               // apply a window resize to the swap chain
            CleanupRenderTarget();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        ULONGLONG now = GetTickCount64();
        if (now >= nextPoll) { link = Poll(); nextPoll = now + 100; }   // ~10 reads a second
        if (link == LINK_EXITED || link == LINK_STOPPED) break;         // the game quit: so do we

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUI(link, onTop, hwnd);
        ImGui::Render();
        const float clear[4] = { 0.06f, 0.06f, 0.06f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);                      // vsync: low CPU use
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    Close();
    if (g_gameProc) CloseHandle(g_gameProc);
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
