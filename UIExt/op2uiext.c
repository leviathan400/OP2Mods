/* op2uiext.c  -  UIExt ("UI Extension"): double-tap a control group to centre the camera,
 *                and double-click a vehicle to select all of its type on screen
 * =====================================================================================
 * The double-click select (DoubleClickSelect=1) is our own addition, not in TH300's
 * original; see "DOUBLE-CLICK A VEHICLE" further down. Everything else in this header
 * describes the double-tap feature.
 *
 * A clean reimplementation of TH300's community mod UIExt.dll (original: 5.6 KB, built
 * 2022-09-05), reverse-engineered from that DLL.
 * Target: Outpost 2 OPU 1.4.1. The patched code is byte-identical in retail 1.3.6.
 *
 * WHAT THE MOD DOES
 * -----------------
 * Press a control-group key (1-9) once: the group is selected, as normal.
 * Press the same key again while that group is still selected (a "double tap"): the
 * camera also jumps to the group. Outpost 2 shipped without this.
 *
 * HOW THE GAME RECALLS A GROUP
 * ----------------------------
 * "Recall control group N" is FUN_0047bd50 @ 0x0047BD50, a __thiscall:
 *   ECX       = the control-group array ("container")
 *   [esp+4]   = the group index N
 * Each group is a 0x41-byte record at container + N*0x41:
 *   +0x0C     unit count (byte)
 *   +0x0D     unit ids (u16 each)
 * The function rebuilds the selection from the record. Afterwards it centres the view
 * on the group ONLY if the dword at container+0x298 is non-zero: FUN_00408140 averages
 * the units' positions and FUN_00408210 scrolls the viewport there. The game itself
 * never sets that flag, so in vanilla a recall never moves the camera.
 *
 * WHAT THIS MODULE CHANGES
 * ------------------------
 * One 5-byte JMP at the entry of FUN_0047bd50 into UIExt_HookEntry. Before the recall
 * runs, UIExt_Check compares the CURRENT selection (the game's live selection buffer at
 * 0x00547210) with group N:
 *   - every selected unit is in group N  -> you pressed the key for a group you already
 *     have selected -> set container+0x298 = 1, so this recall centres the camera;
 *   - otherwise                          -> set it to 0, so a first press just selects.
 * Then the game's own recall runs unchanged.
 *
 * The flag only affects the camera, never simulation state, so this is safe in
 * multiplayer (no desync).
 *
 * DIFFERENCE FROM TH300'S ORIGINAL
 * --------------------------------
 * The original makes 5 in-memory byte patches: a NOP+CALL that routes the recall
 * through its checker, plus companion patches that reset the +0x298 flag at the right
 * times. Here the one entry hook both sets and clears the flag on every recall, so no
 * companion patches are needed. The behaviour is the same.
 *
 * STATUS
 * ------
 * Tested in game on OPU 1.4.1 (2026-11-25): a double tap centres the camera, and with
 * the module off it does not. Double-click select tested 2026-11-27: Lynx, Tiger and
 * ConVec double-clicks selected every one of that type on screen.
 *
 * INSTALL (OPU 1.4.1, in OPU\outpost2.ini):
 *     [ExternalModules]  UIExt=yes        ; OPU's loader: must be yes/no, so leave it yes
 *     [UIExt]            Dll=UIExt.dll
 *                        Enabled=1        ; 1 = on, 0 = off (nothing at all is patched)
 *                        DoubleClickSelect=1 ; 1 = double-click a vehicle selects that type on screen
 * With Enabled=0 the DLL still loads but patches nothing, so the game behaves as vanilla.
 * Enabled is read once at startup (missing = 1); restart the game after changing it.
 * Log: <game dir>\logs\UIExt.log (under OPU: OPU\logs\UIExt.log).
 */
#include <windows.h>
#include <stdio.h>

/* ---- engine addresses and layouts (Outpost2.exe, fixed image base 0x00400000) ---- */
#define RECALL_GROUP_VA   0x0047BD50u   /* FUN_0047bd50: recall control group N                 */
#define RESUME_VA         0x0047BD55u   /* RECALL_GROUP_VA + 5: first instruction after the 5   */
                                        /*   bytes we overwrite; the hook jumps back here       */
#define SEL_BUFFER_VA     0x00547210u   /* live selection: int[0] = count, int[1..] = unit ids  */
#define GROUP_STRIDE      0x41          /* size of one control-group record                     */
#define OFS_COUNT         0x0C          /*   record +0x0C: number of units in the group (byte)  */
#define OFS_IDS           0x0D          /*   record +0x0D: the group's unit ids (u16 each)       */
#define OFS_CENTERFLAG    0x298         /* container +0x298: "centre the view on recall" (dword) */

/* The first 5 bytes of FUN_0047bd50, which the JMP replaces:
 *   8B 44 24 04   mov eax, [esp+4]    ; eax = group index
 *   53            push ebx
 * Install() checks they are exactly this before patching, and UIExt_HookEntry runs these
 * two instructions itself before jumping back to RESUME_VA. */
static const unsigned char EXPECT5[5] = { 0x8B, 0x44, 0x24, 0x04, 0x53 };

static HINSTANCE     g_hInst;           /* this DLL's module handle                            */
static volatile LONG g_done = 0;        /* set once Install() has run (op2ext may call several  */
                                        /*   of the init exports; only the first does work)     */
static unsigned char g_saved[5];        /* the original 5 bytes, put back by Uninstall()        */
static int           g_installed = 0;   /* 1 while the JMP is in place                          */

/* The game's runtime folder: where the live outpost2.ini is and logs\ is written.
 * Under OPU, OPULauncher starts the game with the current directory = the OPU folder, so
 * prefer the current directory when it holds an outpost2.ini; else use the exe's folder.
 * Resolved once, without a trailing backslash. */
static char g_gameDir[MAX_PATH];
static const char *GameDir(void)
{
    if (!g_gameDir[0]) {
        char dir[MAX_PATH], ini[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(MAX_PATH, dir);
        if (n > 0 && n < MAX_PATH - 16) {
            if (dir[n - 1] == '\\') dir[n - 1] = '\0';               /* drop a trailing '\' (drive root) */
            wsprintfA(ini, "%s\\outpost2.ini", dir);
            if (GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES) { lstrcpyA(g_gameDir, dir); return g_gameDir; }
        }
        /* fallback: the folder Outpost2.exe is in */
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        { char *bs = strrchr(dir, '\\'); if (bs) *bs = '\0'; else dir[0] = '\0'; }
        lstrcpyA(g_gameDir, dir[0] ? dir : ".");
    }
    return g_gameDir;
}

/* Append one printf-style line to "<game dir>\logs\UIExt.log", creating the logs folder if
 * needed. The file is opened and closed per line, so the log survives a game crash.
 * Uses wvsprintfA (no %f; output capped at 1024 chars, here 511 by the buffer). */
static void LogLine(const char *fmt, ...)
{
    char buf[512], path[MAX_PATH]; va_list ap; va_start(ap, fmt); wvsprintfA(buf, fmt, ap); va_end(ap);
    { char logdir[MAX_PATH]; wsprintfA(logdir, "%s\\logs", GameDir()); CreateDirectoryA(logdir, NULL); }
    wsprintfA(path, "%s\\logs\\UIExt.log", GameDir());
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { SetFilePointer(h,0,NULL,FILE_END);
        char line[600]; DWORD w; int n=wsprintfA(line,"%s\r\n",buf); WriteFile(h,line,n,&w,NULL); CloseHandle(h); }
}

/* ---- the double-tap test (the same test as TH300's original) ----
 * Called by UIExt_HookEntry on every recall, BEFORE the game's recall code runs.
 *   container = the control-group array (ECX at FUN_0047bd50's entry)
 *   index     = the group being recalled ([esp+4] at entry)
 * Sets container+0x298 = 1 when the current selection is non-empty and every selected
 * unit is in group[index] (the key was pressed for a group already selected), else 0.
 * The recall that follows then centres the camera only in the first case.
 * Runs on the game's own thread (inside its key handling), so the selection buffer and
 * the group record cannot change under us.
 */
void __cdecl UIExt_Check(unsigned char *container, int index)
{
    unsigned char  *rec    = container + (unsigned)index * GROUP_STRIDE;  /* group[index]           */
    int             gcount = rec[OFS_COUNT];                             /* units in the group      */
    unsigned short *gids   = (unsigned short *)(rec + OFS_IDS);          /* the group's unit ids    */
    int            *sel    = (int *)SEL_BUFFER_VA;                       /* the live selection      */
    int             selN   = sel[0];                                     /* units selected now      */
    int             allIn  = 0;

    /* Is every selected unit in the group? (An empty selection or empty group never counts.) */
    if (selN > 0 && gcount > 0) {
        allIn = 1;
        for (int i = 0; i < selN; i++) {
            unsigned int sid = (unsigned int)sel[1 + i];   /* a selected unit's id */
            int found = 0;
            for (int j = 0; j < gcount; j++) {
                if ((unsigned int)gids[j] == sid) { found = 1; break; }
            }
            if (!found) { allIn = 0; break; }              /* this unit is not in the group: a first press */
        }
    }
    *(int *)(container + OFS_CENTERFLAG) = allIn ? 1 : 0;  /* the game reads this after rebuilding the selection */
}

/* ---- the hook: FUN_0047bd50's first 5 bytes are replaced with "jmp UIExt_HookEntry" ----
 * On arrival the stack is exactly as at the function's entry ([esp] = the caller's return
 * address, [esp+4] = group index) and ECX = container.
 * 1. Save every register and the flags, call UIExt_Check(container, index), restore them,
 *    so the game's code sees the same state it would have without the hook.
 * 2. Run the two instructions the JMP overwrote (mov eax,[esp+4]; push ebx).
 * 3. Continue the game's function at 0x0047BD55 with push+ret, which leaves every
 *    register (including EAX, just loaded) untouched.
 * naked: no compiler prologue/epilogue, the stack layout above is exactly what we get.
 */
__declspec(naked) void UIExt_HookEntry(void)
{
    __asm {
        pushad                    ; +0x20: EAX ECX EDX EBX ESP EBP ESI EDI (EDI lowest)
        pushfd                    ; +0x04: EFLAGS
        ; Stack now: [esp]=EFLAGS, [esp+4]=EDI ... [esp+0x1C]=ECX, [esp+0x20]=EAX,
        ;            [esp+0x24]=return address, [esp+0x28]=group index.
        mov  eax, [esp+0x1C]      ; ECX as saved by pushad = the control-group array
        mov  edx, [esp+0x28]      ; the function's first argument = group index
        push edx                  ; UIExt_Check(container, index): __cdecl, args right to left
        push eax
        call UIExt_Check
        add  esp, 8               ; __cdecl: the caller removes the arguments
        popfd
        popad                     ; every register is back to its value at the entry
        ; --- the 5 bytes the JMP replaced ---
        mov  eax, [esp+4]         ; eax = group index
        push ebx
        ; --- carry on in the game's function, after those 5 bytes ---
        push RESUME_VA
        ret
    }
}

/* =====================================================================================
 * DOUBLE-CLICK A VEHICLE: SELECT ALL OF YOUR VEHICLES OF THAT TYPE ON SCREEN
 * ([UIExt] DoubleClickSelect=1)
 * =====================================================================================
 * The game never sees double-clicks (its window classes have no CS_DBLCLKS, so a second
 * click is just another WM_LBUTTONDOWN), so the timing is done here.
 *
 * Map clicks go through MouseCommandFilter::OnUIEvent (0x0041ABC0), a __thiscall
 * (this = the filter object at 0x005471B0; args wnd, msg, wParam, lParam, userData;
 * ret 0x14), reached through slot 0 of the filter's vtable at 0x004CFC60. We replace
 * that one vtable entry with ClickFilterHook, which calls the original first and then:
 *   - a plain left click (no Shift/Ctrl) over the map that left exactly ONE of your
 *     vehicles selected, with the mouse on that vehicle, is a "click on vehicle V";
 *   - a click on the same vehicle V within the Windows double-click time of the last one
 *     is a double-click: the selection becomes every one of your vehicles of V's type
 *     (same map_id) inside the visible map area, nearest to V first, up to the game's
 *     limit of 32 - built the way the game's own selection-history restore does it
 *     (Clear, then AddUnit per unit), stored in the selection history, with the game's
 *     group-select sound.
 * Selection is local UI state only (no command packet is sent), so this is safe in
 * multiplayer. Buildings are left alone (the game never selects several buildings).
 */
#define CLICK_VTBL_SLOT_VA  0x004CFC60u  /* MouseCommandFilter vtable, slot 0 = OnUIEvent         */
#define CLICK_FILTER_VA     0x0041ABC0u  /* MouseCommandFilter::OnUIEvent (expected slot value)   */
#define SEL_CLEAR_VA        0x0041A9C0u  /* UnitGroup::Clear(int userData)   - thiscall, no history */
#define SEL_ADD_VA          0x0041A830u  /* UnitGroup::AddUnit(int id, int userData) - thiscall    */
#define SEL_HISTORY_OBJ     0x0056CA00u  /* UnitSelectionHistory object                            */
#define SEL_HISTORY_PUSH_VA 0x0047BFD0u  /* UnitSelectionHistory::CopySelectionToHistory() - thiscall */
#define SOUND_MGR_OBJ       0x0056D250u  /* the sound manager                                      */
#define SOUND_ADD_VA        0x0047EFD0u  /* SoundManager::AddGameSound(int sound, int players)     */
#define SOUND_GROUP_SELECT  0x55         /*   the sound the band select plays for several units    */
#define LOCAL_PLAYER_VA     0x00477F10u  /* static int TethysGame::LocalPlayer() (exported)        */
#define PVE_CTOR_VA         0x0047A480u  /* PlayerVehicleEnum(int player)            (exported)    */
#define PVE_NEXT_VA         0x0047A4B0u  /* int PlayerVehicleEnum::GetNext(Unit&)    (exported)    */
#define UNIT_GETTYPE_VA     0x00475EE0u  /* map_id Unit::GetType() const             (exported)    */
#define UNIT_POOL_PTR_VA    0x0054F848u  /* holds a POINTER to the unit records (0x78 bytes each)  */
#define UNIT_STRIDE         0x78
#define MAP_XMASK_VA        0x0054F7FCu  /* map pixel-x mask (wraps x on round-the-world maps)     */
#define VIEW_SURFACE_VA     0x005758C8u  /* the detail pane's view surface pointer                 */
#define DETAIL_HWND_VA      0x005758BCu  /* the detail pane's HWND (pane object 0x5758B4 + 8)      */
#define MOUSE_ON_MAP_VA     0x0057B7ACu  /* non-zero while the mouse is over the map               */
#define MOUSE_PX_VA         0x0057B7B4u  /* mouse position on the map, pixels (x, then y at +4)    */
#define INPUT_BLOCKED_VA    0x0056E9E0u  /* non-zero while the game ignores input                  */
#define SEL_MAX             32           /* the game's selection limit                             */
/* Unit record fields used: +0x14/+0x18 pixel x/y, +0x22 action (10 = dying),
 * +0x44 flags (2 = vehicle, 0x20000 = being destroyed). View surface: +0x940/+0x944 =
 * view left/top in map pixels. */

typedef int  (__fastcall *ClickFilter_t)(void *self, int edx, void *wnd, UINT msg, WPARAM wp, LPARAM lp, int user);
typedef void (__fastcall *SelClear_t)(void *sel, int edx, int user);
typedef int  (__fastcall *SelAdd_t)(void *sel, int edx, int id, int user);
typedef void (__fastcall *Thiscall0_t)(void *self, int edx);
typedef void (__fastcall *AddSound_t)(void *self, int edx, int sound, int players);
typedef int  (__fastcall *LocalPlayer_t)(void);
typedef void (__fastcall *EnumCtor_t)(void *self, int edx, int player);
typedef int  (__fastcall *EnumNext_t)(void *self, int edx, int *unit);
typedef int  (__fastcall *UnitFn_t)(int *unit, int edx);
/* (The engine's __thiscall methods take `this` in ECX; a __fastcall function with a
 * dummy second argument puts it in ECX the same way and leaves EDX unused.) */

static ClickFilter_t g_origClick   = NULL;  /* the vtable entry we replaced (the game's handler) */
static int           g_clickHooked = 0;     /* 1 while the vtable entry points at our hook       */
static int           g_lastClickId = -1;    /* vehicle hit by the last single click, -1 = none   */
static DWORD         g_lastClickMs = 0;     /*   and when (GetTickCount)                         */

static unsigned char *UnitRec(int id) { return *(unsigned char **)UNIT_POOL_PTR_VA + (unsigned)id * UNIT_STRIDE; }
static int UnitType(int id) { int h = id; return ((UnitFn_t)UNIT_GETTYPE_VA)(&h, 0); }

/* A usable vehicle: a vehicle (flag 2), not being destroyed, not in the dying action. */
static int LiveVehicle(int id)
{
    unsigned char *u = UnitRec(id);
    unsigned flags = *(unsigned *)(u + 0x44);
    return (flags & 2) && !(flags & 0x20000) && u[0x22] != 10;
}

/* Is the mouse on vehicle `id`? Within 24 pixels (3/4 of a tile) of its centre, measuring
 * x through the map's wrap mask so round-the-world maps work too. */
static int MouseOnUnit(int id)
{
    unsigned char *u = UnitRec(id);
    unsigned mask = *(unsigned *)MAP_XMASK_VA;
    int mx = *(int *)MOUSE_PX_VA, my = *(int *)(MOUSE_PX_VA + 4);
    int dx = (int)(((unsigned)(mx - *(int *)(u + 0x14) + 24)) & mask);
    int dy = my - *(int *)(u + 0x18);
    return dx < 48 && dy > -24 && dy < 24;
}

/* Replace the selection with all of the local player's live vehicles of `type` inside
 * the visible map area, nearest to `clicked` first, up to SEL_MAX. Returns how many. */
static int SelectSameTypeOnScreen(int clicked, int type)
{
    unsigned char *surf = *(unsigned char **)VIEW_SURFACE_VA;
    HWND  hwnd = *(HWND *)DETAIL_HWND_VA;
    RECT  rc;
    if (!surf || !hwnd || !GetClientRect(hwnd, &rc)) return 0;
    unsigned mask = *(unsigned *)MAP_XMASK_VA;
    int left = *(int *)(surf + 0x940) & (int)mask, top = *(int *)(surf + 0x944);
    int w = rc.right, h = rc.bottom;                        /* view size in pixels (client area) */

    /* collect matching on-screen vehicles with their squared distance to the clicked one */
    static int ids[512]; static long long d2[512];
    int n = 0;
    int cx = *(int *)(UnitRec(clicked) + 0x14), cy = *(int *)(UnitRec(clicked) + 0x18);
    char it[32] = {0}; int unit;
    ((EnumCtor_t)PVE_CTOR_VA)(it, 0, ((LocalPlayer_t)LOCAL_PLAYER_VA)());
    while (n < 512 && ((EnumNext_t)PVE_NEXT_VA)(it, 0, &unit)) {
        if (!LiveVehicle(unit) || UnitType(unit) != type) continue;
        unsigned char *u = UnitRec(unit);
        int ux = *(int *)(u + 0x14), uy = *(int *)(u + 0x18);
        int vx = (int)(((unsigned)(ux - left)) & mask);     /* x from the view's left edge, wrapped */
        if (vx >= w || uy < top || uy >= top + h) continue; /* off screen */
        long long ddx = (int)(((unsigned)(ux - cx + (int)(mask / 2))) & mask) - (int)(mask / 2);
        long long ddy = uy - cy;
        ids[n] = unit; d2[n] = ddx * ddx + ddy * ddy; n++;
    }
    if (n <= 1) return n;                                   /* only the clicked one: nothing to add */

    /* nearest first (a simple selection sort; n is small) */
    for (int i = 0; i < n - 1 && i < SEL_MAX; i++) {
        int b = i;
        for (int j = i + 1; j < n; j++) if (d2[j] < d2[b]) b = j;
        int ti = ids[i]; ids[i] = ids[b]; ids[b] = ti;
        long long td = d2[i]; d2[i] = d2[b]; d2[b] = td;
    }
    if (n > SEL_MAX) n = SEL_MAX;

    /* rebuild the selection the way the game's history restore does, then record it */
    ((SelClear_t)SEL_CLEAR_VA)((void *)SEL_BUFFER_VA, 0, 0);
    for (int i = 0; i < n; i++) ((SelAdd_t)SEL_ADD_VA)((void *)SEL_BUFFER_VA, 0, ids[i], 0);
    ((Thiscall0_t)SEL_HISTORY_PUSH_VA)((void *)SEL_HISTORY_OBJ, 0);
    ((AddSound_t)SOUND_ADD_VA)((void *)SOUND_MGR_OBJ, 0, SOUND_GROUP_SELECT, -1);
    return n;
}

/* Stands in for MouseCommandFilter::OnUIEvent (vtable slot 0). The game's handler always
 * runs first and its result is returned unchanged; only a plain left click is looked at. */
static int __fastcall ClickFilterHook(void *self, int edx, void *wnd, UINT msg, WPARAM wp, LPARAM lp, int user)
{
    int r = g_origClick(self, edx, wnd, msg, wp, lp, user);
    if (msg != WM_LBUTTONDOWN) return r;
    __try {
        int *sel = (int *)SEL_BUFFER_VA;
        int  id  = sel[0] == 1 ? sel[1] : -1;
        if ((wp & (MK_SHIFT | MK_CONTROL)) || *(int *)INPUT_BLOCKED_VA || !*(int *)MOUSE_ON_MAP_VA ||
            id < 0 || !LiveVehicle(id) || !MouseOnUnit(id)) {
            g_lastClickId = -1;                              /* not a click on one vehicle */
            return r;
        }
        DWORD now = GetTickCount();
        if (id == g_lastClickId && now - g_lastClickMs <= GetDoubleClickTime()) {
            int type = UnitType(id);
            int n = SelectSameTypeOnScreen(id, type);
            LogLine("double-click: vehicle #%d (type %d) -> %d selected on screen", id, type, n);
            g_lastClickId = -1;                              /* a third click starts over */
        } else {
            g_lastClickId = id; g_lastClickMs = now;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lastClickId = -1;
        LogLine("double-click: fault 0x%08X caught", GetExceptionCode());
    }
    return r;
}

/* Point vtable slot 0 at ClickFilterHook, after checking it still holds the game's
 * handler (so another mod's hook or a different exe is never overwritten). */
static void InstallDoubleClick(void)
{
    void **slot = (void **)CLICK_VTBL_SLOT_VA;
    if ((unsigned)*slot != CLICK_FILTER_VA) {
        LogLine("double-click: VERIFY FAILED, vtable slot 0x%08X holds %p (want 0x%08X) - not hooked",
                CLICK_VTBL_SLOT_VA, *slot, CLICK_FILTER_VA);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) { LogLine("double-click: VirtualProtect failed err=%lu", GetLastError()); return; }
    g_origClick = (ClickFilter_t)*slot;
    *slot = (void *)ClickFilterHook;
    VirtualProtect(slot, 4, old, &old);
    g_clickHooked = 1;
    LogLine("INSTALLED: double-click a vehicle -> select that type on screen. hook=%p", ClickFilterHook);
}

static void UninstallDoubleClick(void)
{
    if (!g_clickHooked) return;
    void **slot = (void **)CLICK_VTBL_SLOT_VA;
    DWORD old = 0;
    if (VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
        *slot = (void *)g_origClick;
        VirtualProtect(slot, 4, old, &old);
    }
    g_clickHooked = 0;
}

/* Patch the JMP into FUN_0047bd50. Runs once; logs a startup line and the result.
 * Refuses to patch (and only logs) if the 5 bytes are not the expected ones, so a
 * different or already-patched exe is never modified. */
static void Install(void)
{
    if (InterlockedExchange(&g_done, 1) != 0) return;     /* already ran */
    SYSTEMTIME st; GetLocalTime(&st);
    LogLine("==== op2uiext (recreated UIExt) startup %04d-%02d-%02d %02d:%02d:%02d ====",
            st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);

    /* 0. [UIExt] Enabled in the live outpost2.ini: 1 (or missing) = patch, 0 = leave the
     *    game untouched. Read here, once, at startup. */
    {
        char ini[MAX_PATH];
        wsprintfA(ini, "%s\\outpost2.ini", GameDir());
        if (GetPrivateProfileIntA("UIExt", "Enabled", 1, ini) == 0) {
            LogLine("DISABLED: [UIExt] Enabled=0 in %s - not patching (double tap does nothing).", ini);
            return;
        }
        /* [UIExt] DoubleClickSelect: 1 (or missing) = double-click a vehicle to select all of
         * that type on screen, 0 = off. Independent of the double-tap patch below. */
        if (GetPrivateProfileIntA("UIExt", "DoubleClickSelect", 1, ini) != 0) InstallDoubleClick();
        else LogLine("DoubleClickSelect=0 - double-click select off.");
    }

    /* 1. Check the bytes we are about to replace, and keep a copy for Uninstall(). */
    unsigned char *p = (unsigned char *)RECALL_GROUP_VA;
    for (int i = 0; i < 5; i++) {
        if (p[i] != EXPECT5[i]) {
            LogLine("VERIFY FAILED at 0x%08X+%d: got %02X want %02X - NOT patching (wrong build?)",
                    RECALL_GROUP_VA, i, p[i], EXPECT5[i]);
            return;
        }
    }
    memcpy(g_saved, p, 5);

    /* 2. Make the code page writable, write "E9 rel32" (jmp UIExt_HookEntry), and put the
     *    old protection back. rel32 is relative to the end of the 5-byte JMP. */
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
        LogLine("VirtualProtect failed err=%lu", GetLastError()); return;
    }
    p[0] = 0xE9;                                                   /* jmp rel32 -> hook */
    *(int *)(p + 1) = (int)((unsigned)UIExt_HookEntry - (RECALL_GROUP_VA + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);              /* make sure the CPU sees the new code */
    g_installed = 1;
    LogLine("INSTALLED: double-tap control group -> center camera. hook=%p", UIExt_HookEntry);
}

/* Put the original 5 bytes back. Safe to call more than once (op2ext's destroy export
 * and DLL_PROCESS_DETACH can both reach it); only the first call does anything. */
static void Uninstall(void)
{
    UninstallDoubleClick();
    if (!g_installed) return;
    unsigned char *p = (unsigned char *)RECALL_GROUP_VA;
    DWORD old = 0;
    if (VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(p, g_saved, 5);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        LogLine("[UIExt] uninstalled.");
    }
    g_installed = 0;
}

/* ---- op2ext entry points ----
 * Different op2ext versions call different names: the current loader calls
 * InitMod / RunMod / DestroyMod, older ones mod_init / mod_run / mod_destroy. All are
 * exported (see op2uiext.def). Init installs the hook, destroy removes it; the run
 * calls have nothing to do, because the hook does its work inside the game's own code. */
__declspec(dllexport) void InitMod(void)     { Install(); }
__declspec(dllexport) void mod_init(void)    { Install(); }
__declspec(dllexport) void mod_run(void)     { }
__declspec(dllexport) void mod_destroy(void) { Uninstall(); }
__declspec(dllexport) void DestroyMod(void)  { Uninstall(); }
__declspec(dllexport) void RunMod(void)      { }

/* DllMain: nothing is patched at load (that waits for op2ext's init call). At unload the
 * original bytes are put back, in case the loader unloads us without calling destroy. */
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); }  /* no per-thread calls needed */
    else if (reason == DLL_PROCESS_DETACH) { Uninstall(); }
    return TRUE;
}
