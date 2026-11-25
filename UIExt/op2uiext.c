/* op2uiext.c  -  UIExt ("UI Extension"): double-tap a control group to centre the camera
 * =====================================================================================
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
 * the module off it does not.
 *
 * INSTALL (OPU 1.4.1, in OPU\outpost2.ini):
 *     [ExternalModules]  UIExt=yes        ; OPU's loader: must be yes/no, so leave it yes
 *     [UIExt]            Dll=UIExt.dll
 *                        Enabled=1        ; 1 = double-tap centring on, 0 = off
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
