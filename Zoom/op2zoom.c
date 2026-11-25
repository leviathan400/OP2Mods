/* Zoom.dll (source op2zoom.c) - op2ext addon for Outpost 2 (OPU 1.4.1).
 *
 * Goal: a NEW, wider main-view zoom level - wider than the engine's current widest (zoom = 1) - so
 * the viewport shows MORE map. The engine's zoom is an integer DIVISOR: AllocSurface @ 0x004A9170
 * builds the map back-buffer at (clientW/zoom) x (clientH/zoom); present @ 0x004A9410 blits it
 * scaled UP by zoom (StretchDIBits, or BitBlt 1:1 when zoom==1). So zoom = 1 is already the widest
 * the divisor can express. Seeing MORE needs an effective sub-1 scale: a back-buffer LARGER than the
 * window, shrunk on present, with the coordinate math scaled to match. (See FINDINGS.md / PLAN.md.)
 *
 * HOW (approach 3a, two IAT hooks; engine zoom held at 1):
 *   1. Hook GetClientRect - for the game window, return (client x WIDEN). All size reads funnel
 *      through it, so AllocSurface builds a WIDEN-times-wider buffer AND the coordinate transforms
 *      (view-centre, visible span, scroll) treat the view as wider - consistently, because zoom = 1.
 *   2. Hook BitBlt - at zoom 1 the present is a 1:1 BitBlt of the (now over-sized) map buffer to the
 *      window. When the SOURCE bitmap is wider than the true client, redirect to StretchBlt to shrink
 *      it down to the real window (uniform for full + dirty-rect partial blits).
 *
 * Activation (MVP): zoom level 1 *becomes* the wide level, gated on [Game] WideZoom. The user plays
 * on zoom 3; zooming out to 1 fires the engine's realloc (GetClientRect -> AllocSurface) so the wide
 * buffer is built with no clamp patch / new level / div-by-zero. Both hooks are inert unless
 * WideZoom=1 AND the live zoom global == 1, so normal zoom 2/3 play is untouched.
 *
 * Later additions (see README.md): ScrollWindowEx hook (full repaint instead of a scroll smear),
 * Detail::GetAbsolutePos hook (mouse scaled to the wide view), own box-filter shrink + 32bpp copy,
 * per-map cap on the scale (small maps), and MISSION / ZOOM LEVEL lines in the log.
 *
 * Toggle: [Game] WideZoom = 1 (on, default) / 0 (off);  [Game] ZoomWiden = 2 (max scale factor).
 *
 * Install (OPU 1.4.1): copy Zoom.dll into the OPU folder and add to OPU\outpost2.ini:
 *     [ExternalModules] Zoom = yes
 *     [Zoom] Dll = Zoom.dll
 */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <intrin.h>
#include <tlhelp32.h>
#pragma intrinsic(_ReturnAddress)

/* Verified retail-1.3.6 addresses (FINDINGS.md). Zoom code is unchanged 1.3.6 -> 1.4.1 per the user. */
#define ADDR_PRESENT      0x004A9410u   /* FUN_004a9410 - GDI present (StretchDIBits / BitBlt 1:1)  */
#define ADDR_ALLOCSURFACE 0x004A9170u   /* FUN_004a9170 - back-buffer factory (CreateDIBSection)    */
#define ADDR_ZOOM_GLOBAL  0x00576264u   /* int zoom ([Game] Zoom)                                   */
#define GAMEWND_PTR       0x005756C8u   /* HWND global (same one scrollfix uses)                    */

/* Canonical retail-1.3.6 prologue bytes (from a retail 1.3.6 Outpost2.exe) - sanity-checked at load. */
static const BYTE REF_PRESENT[16]      = { 0x83,0xEC,0x08,0x53,0x56,0x57,0x8B,0xF1,0x8B,0x7C,0x24,0x18,0x55,0x85,0xFF,0x0F };
static const BYTE REF_ALLOCSURFACE[16] = { 0x81,0xEC,0x5C,0x04,0x00,0x00,0x89,0x54,0x24,0x00,0x53,0x89,0x4C,0x24,0x08,0x56 };

static HINSTANCE g_hInst;
static int g_wideZoom = 1;
static int g_widen    = 2;
static int g_smooth   = 1;   /* [Game] ZoomSmooth: 1 = box-filter shrink (thin lines survive), 0 = nearest pixel */
static volatile LONG g_hooked = 0;   /* 1 once both hooks are installed (verify passed, WideZoom=1) */

/* Captured TRUE client size of the game window (before we lie about it), used by the present hook. */
static volatile LONG g_trueW = 0, g_trueH = 0;
/* The size we told AllocSurface for the detail map's back-buffer (true size x WIDEN). The present
 * hook only shrinks blits whose SOURCE bitmap is this buffer, so no other wide bitmap (a tileset or
 * sprite sheet, say) can be squashed by mistake. */
static volatile LONG g_wideW = 0, g_wideH = 0;
#define WIDE_BUF_SLACK 32   /* tolerance in px, in case AllocSurface rounds the size it is given */

/* Effective widen factor for the CURRENT detail buffer, in 1/16ths (32 = 2.0x). 0 = buffer not
 * widened (yet, or it can't be): the view transforms and mouse are then left truthful, so they can
 * never disagree with the buffer the engine actually drew into. */
static volatile LONG g_scale16 = 0;

/* Engine limit: Viewport::SetSurface (0x0046F160) sizes five per-tile redraw bit vectors from the
 * surface size and clears them, but each lives in a fixed bitVectorBuf[400] inside the Viewport
 * (+0x34..+0x803). A bigger surface overruns them into the bit-vector pointers at +0x804..+0x814,
 * and the next clear writes through a NULL pointer -> AV at 0x0046F26C. Same formula as the engine:
 *   bytes = ((((w + 31) >> 5) + 1 + 9) >> 3) * (((h + 31) >> 5) + 2)                        */
#define VIEWPORT_BITVEC_BYTES 400
static int BitVecBytes(int w, int h) {
    int lineBytes = ((((w + 31) >> 5) + 1) + 9) >> 3;
    int rows      = ((h + 31) >> 5) + 2;
    return lineBytes * rows;
}

/* Under OPU the 400-byte limit is gone: OPUPatch's own hook in SetSurface supports larger vectors
 * (see InstallHooks). The only caps left are sanity ones: the widened buffer (<= 16 Mpx, 32 MB at
 * 16bpp) and bit vectors (<= 64 KB each, far beyond any real window). Without OPU the cap stays. */
static int g_bigVectors = 0;              /* 1 = no 400-byte cap (OPUPatch loaded, ZoomBigView=1) */
static int g_bigView = 1;                 /* [Game] ZoomBigView: 1 = lift the 400-byte cap (default) */
#define BIG_BITVEC_MAX   65536
#define WIDE_PIXELS_MAX  (4096 * 4096)

/* The widened view must not be larger than the map itself: the engine's per-frame tile loops
 * (e.g. Viewport::MarkAnimatedTilesForRedraw 0x0046FF80, DrawBackground) walk every visible tile
 * row and read the map cell array without a bound - a view taller than the map reads past it and
 * crashes (OPU 1.4.1 tutorial map at 2x, Outpost2.exe+0x7000A, Test 12). Map size in tiles: */
#define ADDR_MAP_TILEW 0x0054F808u
#define ADDR_MAP_TILEH 0x0054F810u
#define ADDR_MAP_PXMASK 0x0054F7FCu   /* map x wrap mask in pixels */
static int g_allocMapW = 0, g_allocMapH = 0;   /* map size (px) the current buffer was sized for */
static volatile LONG g_resizePosted = 0;       /* a WM_SIZE re-alloc is pending */
static void MapPixelSize(int *mw, int *mh) {
    int tw = 0, th = 0;
    if (!IsBadReadPtr((void*)ADDR_MAP_TILEW, 4)) tw = *(int*)ADDR_MAP_TILEW;
    if (!IsBadReadPtr((void*)ADDR_MAP_TILEH, 4)) th = *(int*)ADDR_MAP_TILEH;
    /* Usable size = map minus a 3-tile margin: the tile loops cover ((h + 31) >> 5) + 1 rows (plus an
     * extra column) from the view's first tile, so a view only just inside the map still reads a
     * row past it (Test 12: a 2040 px view on a 2048 px map crashed). 0 = no map loaded (shell). */
    *mw = (tw > 3 && tw <= 1024) ? (tw - 3) * 32 : 0;
    *mh = (th > 3 && th <= 1024) ? (th - 3) * 32 : 0;
    /* Width is also limited to HALF the map's wrap width: object positions relative to the view are
     * computed modulo the map width and folded into -W/2..+W/2 (e.g. DrawMapRectOutline: x & mask,
     * then "if (x > mask/2) x -= mask"), so anything more than half a map-width right of the view's
     * left edge counts as off-screen - structures vanished on the right of a 3192 px view on the
     * 4096 px tutorial map (Test 12). Wrap mask in pixels: DAT_0054F7FC. */
    if (*mw) {
        int mask = IsBadReadPtr((void*)ADDR_MAP_PXMASK, 4) ? 0 : *(int*)ADDR_MAP_PXMASK;
        int half = (mask > 0 && mask < (1 << 20)) ? (mask + 1) / 2 - 64 : 0;   /* 2-tile margin */
        if (half > 0 && half < *mw) *mw = half;
    }
}

/* Find the loaded mission DLL, for the log. The engine LoadLibrary's the mission (e.g. ct01.dll,
 * c_tutorial.dll) before it builds the map, so by the time a map size shows up the mission module is
 * in the process. Every mission DLL exports DescBlock (its AIModDesc) and InitProc; game/OPU modules
 * don't, so the first module exporting both is the mission. LevelDesc (exported char array) is the
 * mission's display name. Copies the module file name into name (empty when none is loaded) and
 * returns the LevelDesc text or "". */
static const char *FindMissionDll(char *name, int cb) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32 me;
    const char *desc = "";
    name[0] = 0;
    if (snap == INVALID_HANDLE_VALUE) return desc;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) do {
        if (GetProcAddress(me.hModule, "DescBlock") && GetProcAddress(me.hModule, "InitProc")) {
            const char *ld = (const char*)GetProcAddress(me.hModule, "LevelDesc");
            lstrcpynA(name, me.szModule, cb);
            if (ld && !IsBadStringPtrA(ld, 128)) desc = ld;
            break;
        }
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
    return desc;
}

/* Plain lines for the user, written when a mission is loaded (new mission DLL or map size) and
 * whenever the chosen scale changes (e.g. a window resize):
 *   MISSION: ct01.dll - "<LevelDesc>"
 *   ZOOM LEVEL: 1.06x (max 2.00x) - map WxH tiles, view WxH[, capped by map size]
 * Called from the detail-surface alloc (cat 1) and, when a newly loaded map keeps the current scale
 * so no re-alloc happens, from the view transforms (cat 2). Callers only call it once a map exists
 * (mw > 0): the shell/menu allocs have no map and no mission. s16 = scale in 1/16ths, w x hgt = the
 * true detail view, mw x mh = the usable map size from MapPixelSize. */
static void Log(const char *fmt, ...);

/* Mission (re)load detection. A mission can be exited back to the menu and another (or the same one
 * again) loaded without restarting the exe, and the next map may have the same size - so the map
 * size alone can't tell that a new mission started. LevelDLL::LoadScript (0x00402B20) loads the
 * mission - new games and saved games (LevelDLL::Load) alike - with Outpost2.exe's own LoadLibraryA
 * import; we hook that IAT slot and bump g_missionGen
 * whenever the loaded module is a mission DLL (exports DescBlock + InitProc). The view-transform
 * branch of MyGetClientRect sees the new generation, re-checks the scale for the new map and logs
 * MISSION / ZOOM LEVEL for it. Nothing is logged here: the map isn't loaded yet at this point. */
static volatile LONG g_missionGen = 0;
typedef HMODULE (WINAPI *LoadLibraryA_t)(LPCSTR);
static LoadLibraryA_t g_realLoadLibraryA = NULL;
static HMODULE WINAPI MyLoadLibraryA(LPCSTR name) {
    HMODULE m = g_realLoadLibraryA(name);
    if (m && GetProcAddress(m, "DescBlock") && GetProcAddress(m, "InitProc"))
        InterlockedIncrement(&g_missionGen);
    return m;
}

static void LogMapZoom(int s16, int w, int hgt, int mw, int mh) {
    static int lastMw = -1, lastMh = -1, lastS16 = -1;
    static LONG lastGen = -1;
    static char lastMission[MAX_PATH] = "";
    char mission[MAX_PATH];
    const char *desc = FindMissionDll(mission, sizeof(mission));
    LONG gen = g_missionGen;
    int newMission;
    if (!mission[0]) return;          /* menu / between missions: the map globals may be stale */
    newMission = gen != lastGen || lstrcmpiA(mission, lastMission) != 0;
    if (newMission) {
        Log("MISSION: %s - \"%s\"", mission, desc);
        lstrcpynA(lastMission, mission, sizeof(lastMission));
        lastGen = gen;
    }
    if (newMission || mw != lastMw || mh != lastMh || s16 != lastS16) {
        int z = s16 * 100 / 16, zmax = g_widen * 100;
        int tw = 0, th = 0;
        if (!IsBadReadPtr((void*)ADDR_MAP_TILEW, 4)) tw = *(int*)ADDR_MAP_TILEW;
        if (!IsBadReadPtr((void*)ADDR_MAP_TILEH, 4)) th = *(int*)ADDR_MAP_TILEH;
        Log("ZOOM LEVEL: %d.%02dx (max %d.%02dx) - map %dx%d tiles, view %dx%d%s",
            z / 100, z % 100, zmax / 100, zmax % 100, tw, th, w, hgt,
            s16 < g_widen * 16 ? ", capped by map size" : "");
        lastMw = mw; lastMh = mh; lastS16 = s16;
    }
}

/* Largest factor (1/16 steps, from ZoomWiden down) whose widened surface fits: the map, and the
 * engine's 400-byte bit vectors (or, under OPU, the sanity caps). Returns 16 (= 1.0x) when nothing
 * fits. */
static int FitScale16(int w, int h) {
    int s16, mw, mh;
    MapPixelSize(&mw, &mh);
    for (s16 = g_widen * 16; s16 > 16; s16--) {
        int W = MulDiv(w, s16, 16), H = MulDiv(h, s16, 16);
        if ((mw && W > mw) || (mh && H > mh)) continue;  /* view would be bigger than the map */
        if (g_bigVectors) {
            if ((double)W * H <= WIDE_PIXELS_MAX && BitVecBytes(W, H) <= BIG_BITVEC_MAX) return s16;
        } else if (BitVecBytes(W, H) <= VIEWPORT_BITVEC_BYTES) {
            return s16;
        }
    }
    return 16;
}

typedef BOOL (WINAPI *GetClientRect_t)(HWND, LPRECT);
typedef BOOL (WINAPI *BitBlt_t)(HDC,int,int,int,int,HDC,int,int,DWORD);
static GetClientRect_t g_realGCR    = NULL;
static BitBlt_t        g_realBitBlt = NULL;

/* The game's runtime folder: where the LIVE outpost2.ini is read and logs\ is written.
 * Under OPU, OPULauncher starts the game with the current directory = the OPU folder, while
 * Outpost2.exe itself can sit elsewhere (e.g. an unpatched copy one level up) - so prefer the
 * current directory when it holds an outpost2.ini, and fall back to the exe's folder (retail,
 * where the two are the same). Resolved once, without a trailing backslash. */
static char g_gameDir[MAX_PATH];
static const char *GameDir(void) {
    if (!g_gameDir[0]) {
        char dir[MAX_PATH], ini[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(MAX_PATH, dir);
        if (n > 0 && n < MAX_PATH - 16) {
            if (dir[n - 1] == '\\') dir[n - 1] = '\0';
            wsprintfA(ini, "%s\\outpost2.ini", dir);
            if (GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES) { lstrcpyA(g_gameDir, dir); return g_gameDir; }
        }
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        { char *bs = strrchr(dir, '\\'); if (bs) *bs = '\0'; else dir[0] = '\0'; }
        lstrcpyA(g_gameDir, dir[0] ? dir : ".");
    }
    return g_gameDir;
}

/* Build "<game dir>\logs\Zoom.log", creating the logs folder if needed. */
static void LogPath(char *out, int cb) {
    const char *dir = GameDir();
    { char logdir[MAX_PATH]; wsprintfA(logdir, "%s\\logs", dir); CreateDirectoryA(logdir, NULL); }
    wsprintfA(out, "%s\\logs\\Zoom.log", dir);
    (void)cb;
}

/* Two log levels, set by [Zoom] Debug in outpost2.ini:
 *   Log()  - always written: load result, settings, hook failures, MISSION / ZOOM LEVEL per mission,
 *            errors and warnings, and the ZoomStats lines (which have their own switch).
 *   LogD() - only with Debug=1: diagnostics - engine byte checks, GetClientRect callers, buffer
 *            allocations and re-sizes, present/shrink path details, scroll repaints, zoom changes. */
static int g_debug = 0;
static void LogV(const char *fmt, va_list ap);
static void Log(const char *fmt, ...)  { va_list ap; va_start(ap, fmt); LogV(fmt, ap); va_end(ap); }
static void LogD(const char *fmt, ...) {
    va_list ap;
    if (!g_debug) return;
    va_start(ap, fmt); LogV(fmt, ap); va_end(ap);
}
static void LogV(const char *fmt, va_list ap) {
    char buf[512]; wvsprintfA(buf, fmt, ap);
    SYSTEMTIME t; GetLocalTime(&t);
    char path[MAX_PATH]; LogPath(path, sizeof(path));
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, NULL, FILE_END);
        char line[700]; DWORD w;
        int n = wsprintfA(line, "[%04d-%02d-%02d %02d:%02d:%02d] %s\r\n",
                          t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, buf);
        WriteFile(h, line, n, &w, NULL); CloseHandle(h);
    }
}

/* Log the bytes at `va` and compare them with the retail reference. Returns 1 on a match. */
static int VerifyBytes(const char *label, DWORD va, const BYTE *ref, int n) {
    BYTE *p = (BYTE*)va; char hex[3*32+1]; int i, o = 0, match;
    if (n > 32) n = 32;
    if (IsBadReadPtr(p, n)) { Log("%s @ 0x%08X: UNREADABLE", label, va); return 0; }
    for (i = 0; i < n; i++) o += wsprintfA(hex + o, "%02X ", p[i]);
    match = (memcmp(p, ref, n) == 0);
    if (match) LogD("%s @ 0x%08X: %s [MATCH retail]", label, va, hex);
    else       Log("%s @ 0x%08X: %s [DIFFER from retail]", label, va, hex);
    return match;
}

/* The wide level is live only when enabled AND the engine zoom is at level 1 (our wide level). */
static int WideActive(void) {
    if (!g_wideZoom) return 0;
    if (IsBadReadPtr((void*)ADDR_ZOOM_GLOBAL, 4)) return 0;
    return *(volatile int*)ADDR_ZOOM_GLOBAL == 1;
}

/* Which GetClientRect callers belong to the MAP subsystem (and must see a WIDEN-ed rect). Scoped by
 * return-address range so panel-layout (FUN_004999a0 @ 0x4999a0, reads the MAIN window) stays truthful.
 *   1 = ALLOC  (FUN_0049d030 -> AllocSurface 0x4A9170: sizes the map back-buffer)
 *   2 = TRANSFORM (FUN_00407990 clamp, FUN_00408210 centre, FUN_004082f0/00408370/00408540 view math) */
static int MapCallerCat(DWORD r) {
    if (r >= 0x0049D030u && r < 0x0049D070u) return 1;   /* FUN_0049d030 - back-buffer alloc */
    if (r >= 0x00407980u && r < 0x00407A60u) return 2;   /* FUN_00407990 - clamp */
    if (r >= 0x00408200u && r < 0x004085F0u) return 2;   /* FUN_00408210/4082f0/408370/408540 cluster */
    return 0;
}

/* Log each distinct GetClientRect caller once (address, size, and whether we widen it). */
static DWORD g_callers[64];
static volatile LONG g_callerCount = 0;
static void NoteCaller(DWORD ret, HWND h, HWND gw, int w, int hgt, int cat) {
    int i, n = g_callerCount;
    for (i = 0; i < n; i++) if (g_callers[i] == ret) return;
    if (n < 64) { g_callers[n] = ret; g_callerCount = n + 1; }
    LogD("GCR caller=0x%08X hwnd=%s size=%dx%d %s", ret, (h == gw) ? "GAME" : "other", w, hgt,
        cat ? (cat == 1 ? "[WIDEN:alloc]" : "[WIDEN:transform]") : "(truthful)");
}

/* Hook 1: widen the client rect (x WIDEN) ONLY for the map callers, so the map renders more world
 * while panel layout (a different caller/window) stays truthful and panels keep their place. */
static BOOL WINAPI MyGetClientRect(HWND h, LPRECT r) {
    BOOL ok = g_realGCR(h, r);
    if (ok && r && WideActive()) {
        DWORD ret = (DWORD)(DWORD_PTR)_ReturnAddress();
        int cat = MapCallerCat(ret);
        int w = r->right - r->left, hgt = r->bottom - r->top;
        NoteCaller(ret, h, *(HWND*)GAMEWND_PTR, w, hgt, cat);
        /* cat 1 (alloc) is SHARED by the detail map AND the small panels (minimap/command pane). Only
         * widen the large detail surface; leave the small panel surfaces truthful so the sidebar is
         * untouched. cat 2 (transforms) only ever read the detail window: widen them by the SAME
         * factor the buffer got, and only once the buffer has been widened. */
        if (cat == 1 && w >= 400) {
            int s16 = FitScale16(w, hgt);
            {   /* remember the map size this buffer was sized for (the map cap needs a re-alloc when a
                 * mission loads a different map - see the cat 2 branch) */
                static int nAlloc = 0; int mw, mh;
                MapPixelSize(&mw, &mh);
                g_allocMapW = mw; g_allocMapH = mh; g_resizePosted = 0;
                if (nAlloc < 20) { nAlloc++;
                    LogD("alloc #%d: view %dx%d, map %dx%d px -> x%d/16", nAlloc, w, hgt, mw, mh, s16); }
                if (mw > 0) LogMapZoom(s16, w, hgt, mw, mh);
            }
            g_trueW = w; g_trueH = hgt;                     /* detail size = present shrink-target fallback */
            if (s16 > 16) {
                LONG ww = MulDiv(w, s16, 16), wh = MulDiv(hgt, s16, 16);
                if (s16 != g_scale16 || ww != g_wideW)
                    LogD("wide buffer: %dx%d -> %ldx%ld (x%d/16 = %d.%02dx%s)", w, hgt, ww, wh, s16,
                        s16 * 100 / 16 / 100, s16 * 100 / 16 % 100,
                        s16 >= g_widen * 16 ? "" : g_bigVectors ? ", reduced to fit the map (or the 16 Mpx cap)"
                                                 : ", reduced to fit the map / 400-byte tile bit vectors");
                g_wideW = ww; g_wideH = wh;                 /* the only buffer the present may shrink */
                g_scale16 = s16;
                r->right  = r->left + ww;
                r->bottom = r->top  + wh;
            } else {
                if (g_scale16 != 0 || g_wideW != 0)
                    LogD("wide buffer: %dx%d is too large to widen at all (engine tile bit vectors) - vanilla", w, hgt);
                g_wideW = g_wideH = 0;
                g_scale16 = 0;
            }
        } else if (cat == 2 && g_trueW > 0) {
            /* A mission loaded a different map since the buffer was sized (the engine does NOT
             * re-allocate the detail surface on mission load - Test 12): if the current scale no
             * longer fits this map, post WM_SIZE to the detail window. Its handler rebuilds the
             * surface (back through our cat 1 branch, now map-capped); a posted message is handled
             * before the game's next idle frame. The view transforms run when a mission centres its
             * view, i.e. before anything is drawn. */
            /* Also runs at 1.00x (g_scale16 == 0, a map too small to widen), so a bigger map loaded
             * next still gets widened, and on every new mission (g_missionGen, see MyLoadLibraryA)
             * even when the map size didn't change, so each mission gets its own log lines. */
            static LONG seenGen = 0;
            int mw, mh;
            LONG gen = g_missionGen;
            MapPixelSize(&mw, &mh);
            if (!g_resizePosted && mw > 0 && (mw != g_allocMapW || mh != g_allocMapH || gen != seenGen)) {
                int cur = g_scale16 > 16 ? g_scale16 : 16;   /* scale the buffer has now */
                int s16 = FitScale16(g_trueW, g_trueH);
                seenGen = gen;
                if (s16 != cur) {
                    g_resizePosted = 1;
                    LogD("map now %dx%d px (buffer sized for %dx%d): x%d/16 -> x%d/16, posting WM_SIZE to re-allocate",
                        mw, mh, g_allocMapW, g_allocMapH, cur, s16);
                    PostMessageA(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(g_trueW, g_trueH));
                } else {
                    g_allocMapW = mw; g_allocMapH = mh;      /* still fits - nothing to re-allocate */
                    LogMapZoom(cur, g_trueW, g_trueH, mw, mh);   /* new map/mission, same scale */
                }
            }
            if (g_scale16 > 16) {                            /* transforms match the widened buffer */
                r->right  = r->left + MulDiv(w,   g_scale16, 16);
                r->bottom = r->top  + MulDiv(hgt, g_scale16, 16);
            }
        }
    }
    return ok;
}

/* Mouse fix: the map mouse position (0x0057B7B4) is computed by IWnd::Pane::Detail::GetAbsolutePos
 * (0x00408070: client pixel -> map pixel, view origin + pixel / zoom), and the client pixel must be
 * scaled by our widen factor because the view now spans that many more buffer pixels. It is called
 * from two places, and both must be scaled:
 *   - Detail::CallFilters (0x00407A50), from each mouse message's lParam;
 *   - Dans_RULE_UIFrame::OnIdle (0x0049C310), EVERY FRAME, from GetCursorPos + ScreenToClient.
 * The old fix scaled lParam in the window procedure, which covered only the first: OnIdle overwrote
 * the position with an unscaled one each frame, so anything reading it between messages - e.g. the
 * moving corner of a selection box - sat at cursor / scale (Test 10). So we hook GetAbsolutePos
 * itself, via the Detail vtable slot 10 (0x004CF5E8; its only reference, no direct calls), and leave
 * the message lParam untouched. */
#define ADDR_DETAIL_GETABSPOS 0x00408070u
#define ADDR_DETAIL_VT_SLOT10 0x004CF5E8u   /* IWnd::Pane::Detail vtable (0x004CF5C0) + 10*4 */
typedef int (__fastcall *GetAbsPos_t)(void *pane, void *edxUnused, int px, int py, void *out);
static int __fastcall MyGetAbsolutePos(void *pane, void *edxUnused, int px, int py, void *out) {
    LONG s16 = g_scale16;
    if (s16 > 16 && WideActive()) { px = MulDiv(px, s16, 16); py = MulDiv(py, s16, 16); }
    return ((GetAbsPos_t)ADDR_DETAIL_GETABSPOS)(pane, edxUnused, px, py, out);
}
static int HookDetailMouse(void) {
    DWORD *slot = (DWORD*)ADDR_DETAIL_VT_SLOT10, op;
    if (IsBadReadPtr(slot, 4) || *slot != ADDR_DETAIL_GETABSPOS) {
        Log("mouse: Detail vtable slot 10 = %08X, expected %08X - NOT hooked",
            IsBadReadPtr(slot, 4) ? 0 : *slot, ADDR_DETAIL_GETABSPOS);
        return 0;
    }
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &op)) return 0;
    *slot = (DWORD)(DWORD_PTR)MyGetAbsolutePos;
    VirtualProtect(slot, 4, op, &op);
    return 1;
}

static HWND   g_detailWnd = NULL;
static WNDPROC g_origProc = NULL;
static volatile LONG g_subclassed = 0;
#define ZOOM_TIMER_ID 0x5A4D        /* our refresh timer on the detail window ('ZM') */
static void OnRefreshTimer(HWND hw);
static int g_stats;   /* [Game] ZoomStats (defined with the statistics below) */
/* Diagnostics (ZoomStats=1): one log line per left-button down/up on the map - the raw and scaled
 * lParam, where Windows says the cursor is (GetCursorPos -> ScreenToClient), the window's true client
 * size, the game's resulting map mouse position (0x0057B7B4/B8) and the view origin, so a mouse/
 * picture coordinate mismatch can be measured instead of guessed. */
#define ADDR_MAP_MOUSE_X   0x0057B7B4u
#define ADDR_DETAIL_SURFACE 0x005758C8u   /* gameFrame + 0x1F4 (detailPane) + 0x14 (surface) */
static void LogClick(HWND hw, UINT msg, LPARAM rawLp, LPARAM newLp) {
    POINT cp; RECT rc; int vx = -1, vy = -1;
    BYTE *surface;
    GetCursorPos(&cp); ScreenToClient(hw, &cp);
    if (!g_realGCR || !g_realGCR(hw, &rc)) SetRectEmpty(&rc);
    if (!IsBadReadPtr((void*)ADDR_DETAIL_SURFACE, 4) && (surface = *(BYTE**)ADDR_DETAIL_SURFACE) != NULL &&
        !IsBadReadPtr(surface + 0x110 + 0x830, 8)) {
        vx = *(int*)(surface + 0x110 + 0x830); vy = *(int*)(surface + 0x110 + 0x834);
    }
    Log("click %s: lParam raw=(%d,%d) scaled=(%d,%d)  cursor(client)=(%d,%d)  client=%dx%d  "
        "mapMouse=(%d,%d) viewOrigin=(%d,%d) -> rel=(%d,%d)  scale=%ld/16",
        msg == WM_LBUTTONDOWN ? "DOWN" : "UP",
        (short)LOWORD(rawLp), (short)HIWORD(rawLp), (short)LOWORD(newLp), (short)HIWORD(newLp),
        cp.x, cp.y, rc.right - rc.left, rc.bottom - rc.top,
        *(int*)ADDR_MAP_MOUSE_X, *(int*)(ADDR_MAP_MOUSE_X + 4), vx, vy,
        *(int*)ADDR_MAP_MOUSE_X - vx, *(int*)(ADDR_MAP_MOUSE_X + 4) - vy, g_scale16);
}

/* Detail window subclass: our refresh timer, and the click diagnostics. Mouse scaling is NOT done here
 * any more (see MyGetAbsolutePos) - messages pass through unchanged. */
static LRESULT CALLBACK MyDetailProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    LONG s16 = g_scale16;
    if (msg == WM_TIMER && wp == ZOOM_TIMER_ID) { OnRefreshTimer(hw); return 0; }   /* ours, not the engine's */
    if (g_stats && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)) {
        LRESULT r = CallWindowProcA(g_origProc, hw, msg, wp, lp);
        LPARAM scaled = lp;
        if (WideActive() && s16 > 16)   /* the pixel MyGetAbsolutePos converts, for the log */
            scaled = (LPARAM)MAKELONG(MulDiv((int)(short)LOWORD(lp), s16, 16) & 0xFFFF,
                                      MulDiv((int)(short)HIWORD(lp), s16, 16) & 0xFFFF);
        LogClick(hw, msg, lp, scaled);
        return r;
    }
    return CallWindowProcA(g_origProc, hw, msg, wp, lp);
}

/* Is this source bitmap the over-sized detail map buffer we asked AllocSurface for? */
static int IsWideMapBuffer(const BITMAP *bm) {
    LONG ww = g_wideW, wh = g_wideH;
    if (ww <= 0 || wh <= 0) return 0;
    return bm->bmWidth  >= ww - WIDE_BUF_SLACK && bm->bmWidth  <= ww + WIDE_BUF_SLACK &&
           bm->bmHeight >= wh - WIDE_BUF_SLACK && bm->bmHeight <= wh + WIDE_BUF_SLACK;
}

/* Hook 2: at zoom 1 the present is a 1:1 BitBlt of the map buffer. When the source bitmap is OUR
 * over-sized detail buffer (exactly the size the alloc hook handed out) and wider than its destination
 * window, shrink it to fit via StretchBlt. Targets the destination window's true client (WindowFromDC),
 * falling back to the alloc-captured detail size. Scale is derived from bitmap-vs-target dims, so full
 * and partial (dirty-rect) blits both map. Any other bitmap passes through untouched. */
static volatile LONG g_blitLogged = 0, g_skipLogged = 0;

/* Present statistics while wide-active, logged every STATS_MS when [Game] ZoomStats=1 (off by
 * default - it adds a log line every 5 s). Used to diagnose speed / redraw problems. */
#define STATS_MS 5000
static int g_stats = 0;   /* [Game] ZoomStats */
static struct {
    LONG blits;        /* every BitBlt seen while wide-active */
    LONG shrinkWnd;    /* shrinks whose destination is a window (the real present) */
    LONG shrinkMem;    /* shrinks whose destination is NOT a window (memory DC) */
    LONG shrinkFull;   /* shrinks covering (nearly) the whole buffer */
    LONG scrolls;      /* ScrollWindowEx calls replaced by a full repaint */
    LONG refreshes;    /* periodic full repaints (safety net) */
    double srcPx;      /* source pixels shrunk */
    double ms;         /* time spent shrinking (resample + blit) */
    double blitMs;     /* of which: the final 1:1 BitBlt to the window */
} g_st;
static DWORD g_stStart = 0;
static LARGE_INTEGER g_qpf;
static void StatsMaybeFlush(void) {
    DWORD now = GetTickCount();
    if (!g_stats) return;
    if (!g_stStart) { g_stStart = now; return; }
    if (now - g_stStart < STATS_MS) return;
    {
        double secs = (now - g_stStart) / 1000.0;
        int msTotal = (int)(g_st.ms + 0.5), msPerSec = (int)(g_st.ms / secs + 0.5);
        int mpx = (int)(g_st.srcPx / 1e6 + 0.5);
        Log("stats %ds: blits=%ld shrink(wnd)=%ld shrink(mem)=%ld full=%ld scroll-repaints=%ld refreshes=%ld  src=%d Mpx  shrink time=%d ms (%d ms/s, of which blit %d ms/s)",
            (int)(secs + 0.5), g_st.blits, g_st.shrinkWnd, g_st.shrinkMem, g_st.shrinkFull, g_st.scrolls,
            g_st.refreshes, mpx, msTotal, msPerSec, (int)(g_st.blitMs / secs + 0.5));
    }
    ZeroMemory(&g_st, sizeof(g_st));
    g_stStart = now;
}

/* ---- Fast, seam-free shrink ---------------------------------------------------------------
 * GDI StretchBlt per dirty rect was slow (~25-33% of the frame) and, because each small rect is
 * resampled on its own with its own rounding, a redrawn tile sampled different pixels than its
 * neighbours -> visible shimmer on every redraw. Instead we keep our own window-sized "small"
 * DIB in the wide buffer's own pixel format, and one GLOBAL source mapping (small pixel X -> wide
 * pixel xmap[X]), so a pixel always comes from the same source pixel whichever rect redraws it.
 * Each dirty rect is resampled straight from the wide DIB's bits into the small DIB, then presented
 * with a plain 1:1 BitBlt (the same fast path vanilla zoom 1 uses). Used when the wide buffer is a
 * DIB section (8/16/24/32 bpp, BI_RGB or BI_BITFIELDS) and rop is SRCCOPY; anything else falls back
 * to StretchBlt. The buffer's format is logged once. */
static HDC     g_smallDC  = NULL;
static HBITMAP g_smallBmp = NULL;
static BYTE   *g_smallBits = NULL;
static int     g_smallW = 0, g_smallH = 0, g_smallStride = 0;   /* stride in bytes */
static int     g_smallBpp = 0; static DWORD g_smallComp = 0, g_smallMasks[3];
static int    *g_xmap = NULL, *g_ymap = NULL;                    /* small px -> wide px */
static int     g_mapSrcW = 0, g_mapSrcH = 0;
static volatile LONG g_fastLogged = 0, g_fallbackLogged = 0, g_fmtLogged = 0;

/* 32bpp small copy for a 16bpp buffer ([Game] ZoomBlit32, default 1): the final 1:1 BitBlt to the
 * (32bpp) screen then needs no per-pixel 16->32 conversion. Pixels are converted while resampling
 * through g_lut[65536], which is built BY GDI ITSELF (all 65536 values blitted from a 16bpp DIB in
 * the buffer's exact format into a 32bpp DIB), so colours are identical to GDI's own conversion. */
static int    g_blit32 = 1;               /* [Game] ZoomBlit32 */
static DWORD *g_lut = NULL;               /* 16bpp pixel -> 0x00RRGGBB */
static DWORD  g_lutComp = 0xFFFFFFFF, g_lutMasks[3];
static int BuildLut(HDC hd, const DIBSECTION *ds) {
    DWORD comp = ds->dsBmih.biCompression;
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } si;
    BITMAPINFO di; void *sbits = NULL, *dbits = NULL; HBITMAP sb = NULL, db = NULL;
    HDC sdc = NULL, ddc = NULL; int ok = 0, i;
    if (g_lut && g_lutComp == comp && (comp != BI_BITFIELDS || memcmp(g_lutMasks, ds->dsBitfields, sizeof(g_lutMasks)) == 0))
        return 1;
    ZeroMemory(&si, sizeof(si)); ZeroMemory(&di, sizeof(di));
    si.h.biSize = sizeof(BITMAPINFOHEADER); si.h.biWidth = 256; si.h.biHeight = -256;
    si.h.biPlanes = 1; si.h.biBitCount = 16; si.h.biCompression = comp;
    if (comp == BI_BITFIELDS) memcpy(si.masks, ds->dsBitfields, sizeof(si.masks));
    di.bmiHeader = si.h; di.bmiHeader.biBitCount = 32; di.bmiHeader.biCompression = BI_RGB;
    sb = CreateDIBSection(hd, (BITMAPINFO*)&si, DIB_RGB_COLORS, &sbits, NULL, 0);
    db = CreateDIBSection(hd, &di, DIB_RGB_COLORS, &dbits, NULL, 0);
    sdc = CreateCompatibleDC(hd); ddc = CreateCompatibleDC(hd);
    if (sb && db && sbits && dbits && sdc && ddc) {
        HGDIOBJ os = SelectObject(sdc, sb), od = SelectObject(ddc, db);
        for (i = 0; i < 65536; i++) ((WORD*)sbits)[i] = (WORD)i;        /* 256x256, stride 512 B */
        GdiFlush();
        if (g_realBitBlt(ddc, 0, 0, 256, 256, sdc, 0, 0, SRCCOPY)) {
            GdiFlush();
            if (!g_lut) g_lut = (DWORD*)HeapAlloc(GetProcessHeap(), 0, sizeof(DWORD) * 65536);
            if (g_lut) {
                for (i = 0; i < 65536; i++) g_lut[i] = ((DWORD*)dbits)[i] & 0x00FFFFFF;
                g_lutComp = comp; memcpy(g_lutMasks, ds->dsBitfields, sizeof(g_lutMasks)); ok = 1;
            }
        }
        SelectObject(sdc, os); SelectObject(ddc, od);
    }
    if (sdc) DeleteDC(sdc); if (ddc) DeleteDC(ddc);
    if (sb) DeleteObject(sb); if (db) DeleteObject(db);
    LogD("present: 16->32bpp lookup table %s (compression=%lu, white 0x7FFF -> %06lX)",
        ok ? "built by GDI" : "FAILED - using a 16bpp copy", comp, ok ? g_lut[0x7FFF] : 0);
    return ok;
}

/* (Re)create our small DIB. Same pixel format as the wide buffer (bit depth, BI_RGB or BI_BITFIELDS
 * masks) so a pixel can be copied as-is - except a 16bpp buffer gets a 32bpp copy when ZoomBlit32 is
 * on (see above). 8bpp colour tables are synced per present. */
static int EnsureSmall(HDC hd, int tw, int th, int bw, int bh, const DIBSECTION *ds) {
    int bpp = ds->dsBm.bmBitsPixel; DWORD comp = ds->dsBmih.biCompression;
    if (bpp == 16 && g_blit32 && BuildLut(hd, ds)) { bpp = 32; comp = BI_RGB; }
    if (!g_smallDC || g_smallW != tw || g_smallH != th || g_smallBpp != bpp || g_smallComp != comp ||
        (comp == BI_BITFIELDS && memcmp(g_smallMasks, ds->dsBitfields, sizeof(g_smallMasks)) != 0)) {
        struct { BITMAPINFOHEADER h; DWORD extra[256]; } bi;   /* room for masks or a colour table */
        void *bits = NULL; HBITMAP nb;
        ZeroMemory(&bi, sizeof(bi));
        bi.h.biSize = sizeof(BITMAPINFOHEADER);
        bi.h.biWidth = tw; bi.h.biHeight = -th;                        /* top-down */
        bi.h.biPlanes = 1; bi.h.biBitCount = (WORD)bpp; bi.h.biCompression = comp;
        if (comp == BI_BITFIELDS) memcpy(bi.extra, ds->dsBitfields, sizeof(DWORD) * 3);
        if (bpp == 8) bi.h.biClrUsed = 256;
        nb = CreateDIBSection(hd, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!nb || !bits) { if (nb) DeleteObject(nb); return 0; }
        if (!g_smallDC) g_smallDC = CreateCompatibleDC(hd);
        if (!g_smallDC) { DeleteObject(nb); return 0; }
        SelectObject(g_smallDC, nb);
        if (g_smallBmp) DeleteObject(g_smallBmp);
        g_smallBmp = nb; g_smallBits = (BYTE*)bits;
        g_smallW = tw; g_smallH = th; g_smallStride = ((tw * bpp + 31) / 32) * 4;
        g_smallBpp = bpp; g_smallComp = comp; memcpy(g_smallMasks, ds->dsBitfields, sizeof(g_smallMasks));
        g_mapSrcW = g_mapSrcH = 0;                                     /* force map rebuild */
    }
    if (g_mapSrcW != bw || g_mapSrcH != bh) {
        int i;
        HeapFree(GetProcessHeap(), 0, g_xmap); HeapFree(GetProcessHeap(), 0, g_ymap);
        g_xmap = (int*)HeapAlloc(GetProcessHeap(), 0, sizeof(int) * (tw + 1));
        g_ymap = (int*)HeapAlloc(GetProcessHeap(), 0, sizeof(int) * (th + 1));
        if (!g_xmap || !g_ymap) { g_mapSrcW = g_mapSrcH = 0; return 0; }
        for (i = 0; i <= tw; i++) g_xmap[i] = (int)(((__int64)i * bw) / tw);
        for (i = 0; i <= th; i++) g_ymap[i] = (int)(((__int64)i * bh) / th);
        g_mapSrcW = bw; g_mapSrcH = bh;
    }
    return 1;
}

/* Row order of the wide DIB. GetObject reports a POSITIVE biHeight for the engine's buffer although
 * it is stored top-down (trusting the sign drew the map upside down), so find out from the pixels:
 * compare GDI's view of logical row 0 with the first and last rows in memory, at columns where those
 * two rows differ. Decided once per buffer (keyed by its bits pointer); 16bpp only, else trust sign. */
static const void *g_orientBits = NULL;
static int g_orientTopDown = 1;
static COLORREF Px16ToRgb(WORD c, const DWORD *m) {
    int r, g, b;
    if (m[1] == 0x07E0) { r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31; g = (g << 2) | (g >> 4); }
    else                { r = (c >> 10) & 31; g = (c >> 5) & 31; b = c & 31; g = (g << 3) | (g >> 2); }
    r = (r << 3) | (r >> 2); b = (b << 3) | (b >> 2);
    return RGB(r, g, b);
}
static int RgbClose(COLORREF a, COLORREF b) {
    return abs((int)GetRValue(a) - (int)GetRValue(b)) <= 8 && abs((int)GetGValue(a) - (int)GetGValue(b)) <= 8 &&
           abs((int)GetBValue(a) - (int)GetBValue(b)) <= 8;
}
static int DetectTopDown(HDC hs, const DIBSECTION *ds) {
    const BYTE *bits = (const BYTE*)ds->dsBm.bmBits;
    int bw = ds->dsBm.bmWidth, bh = ds->dsBm.bmHeight, stride = ds->dsBm.bmWidthBytes, x, votesTop = 0, votesBottom = 0;
    if (bits == g_orientBits) return g_orientTopDown;
    if (ds->dsBm.bmBitsPixel != 16) { g_orientBits = bits; g_orientTopDown = ds->dsBmih.biHeight < 0; return g_orientTopDown; }
    for (x = 0; x < bw && votesTop + votesBottom < 8; x += 7) {
        WORD first = ((const WORD*)bits)[x], last = ((const WORD*)(bits + (size_t)(bh - 1) * stride))[x];
        COLORREF gdi;
        if (first == last) continue;
        gdi = GetPixel(hs, x, 0);
        if (gdi == CLR_INVALID) break;
        if (RgbClose(gdi, Px16ToRgb(first, ds->dsBitfields)))     votesTop++;
        else if (RgbClose(gdi, Px16ToRgb(last, ds->dsBitfields))) votesBottom++;
    }
    if (votesTop + votesBottom == 0) {
        /* Undecided (GetPixel never matched either row). Don't retry: the scan + GetPixel is far too
         * slow to repeat on every present. Assume top-down - what the engine's buffer was observed to
         * be in live testing (retail 1.3.6) - and decide once for this buffer. */
        g_orientBits = bits; g_orientTopDown = 1;
        LogD("present: wide buffer row order undecided from pixels - assuming top-down (biHeight=%ld)",
            ds->dsBmih.biHeight);
        return g_orientTopDown;
    }
    g_orientBits = bits; g_orientTopDown = votesTop >= votesBottom;
    LogD("present: wide buffer row order = %s (votes top=%d bottom=%d, biHeight=%ld)",
        g_orientTopDown ? "top-down" : "bottom-up", votesTop, votesBottom, ds->dsBmih.biHeight);
    return g_orientTopDown;
}

/* Smallest small-pixel index whose source falls at or after wide pixel p: ceil(p * t / b). */
static int CeilMap(int p, int t, int b) { return (int)(((__int64)p * t + b - 1) / b); }

/* Present one dirty rect of the wide buffer (source bitmap `ob`, selected in hs) to window DC hd.
 * Returns 1 if handled, 0 to fall back to StretchBlt. */
static int FastShrink(HDC hd, int x, int y, int w, int h, HDC hs, HGDIOBJ ob, int sx, int sy, int tw, int th) {
    DIBSECTION ds; const BYTE *src; int sStride, bw, bh, topDown, bpp, Bpp, got;
    int X0, X1, Y0, Y1, X, Y, offX, offY, to32;
    ZeroMemory(&ds, sizeof(ds));
    got = GetObjectA(ob, sizeof(ds), &ds);
    bpp = ds.dsBm.bmBitsPixel;
    if (InterlockedExchange(&g_fmtLogged, 1) == 0)
        LogD("present: wide buffer format: GetObject=%d (DIBSECTION=%d) bits=%p bpp=%d compression=%lu "
            "biHeight=%ld masks=%08lX/%08lX/%08lX", got, (int)sizeof(ds), ds.dsBm.bmBits, bpp,
            ds.dsBmih.biCompression, ds.dsBmih.biHeight, ds.dsBitfields[0], ds.dsBitfields[1], ds.dsBitfields[2]);
    if (got != sizeof(ds) || !ds.dsBm.bmBits) return 0;                       /* not a DIB section */
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    if (ds.dsBmih.biCompression != BI_RGB && ds.dsBmih.biCompression != BI_BITFIELDS) return 0;
    Bpp = bpp / 8;
    bw = ds.dsBm.bmWidth; bh = ds.dsBm.bmHeight; sStride = ds.dsBm.bmWidthBytes;
    GdiFlush();                                       /* finish pending GDI drawing before reading bits */
    topDown = DetectTopDown(hs, &ds);
    if (!EnsureSmall(hd, tw, th, bw, bh, &ds)) return 0;
    to32 = (Bpp == 2 && g_smallBpp == 32);            /* 16bpp source -> 32bpp copy through g_lut */
    if (bpp == 8) {                                   /* keep our colour table identical to the source's */
        RGBQUAD pal[256]; UINT n = GetDIBColorTable(hs, 0, 256, pal);
        if (n) SetDIBColorTable(g_smallDC, 0, n, pal);
    }
    if (sx < 0) { w += sx; x -= sx; sx = 0; }
    if (sy < 0) { h += sy; y -= sy; sy = 0; }
    if (sx + w > bw) w = bw - sx;
    if (sy + h > bh) h = bh - sy;
    if (w <= 0 || h <= 0) return 1;
    X0 = CeilMap(sx, tw, bw); X1 = CeilMap(sx + w, tw, bw); if (X1 > tw) X1 = tw;
    Y0 = CeilMap(sy, th, bh); Y1 = CeilMap(sy + h, th, bh); if (Y1 > th) Y1 = th;
    if (g_smooth && Bpp == 2) {
        /* box filter: every small pixel whose block TOUCHES the rect, so even a 1 px rect (a selection
         * box edge, or its erase) on a column/row nearest-sampling would skip still reaches the screen */
        X0 = (int)(((__int64)sx * tw) / bw); Y0 = (int)(((__int64)sy * th) / bh);
        if (X1 <= X0) X1 = X0 + 1;
        if (Y1 <= Y0) Y1 = Y0 + 1;
        if (X1 > tw) X1 = tw;
        if (Y1 > th) Y1 = th;
    }
    if (X1 <= X0 || Y1 <= Y0) return 1;               /* rect too thin to own any small pixel */
    src = (const BYTE*)ds.dsBm.bmBits;
    if (g_smooth && Bpp == 2) {
        /* Box filter: each small pixel = average of the source block it covers,
         * [xmap[X], xmap[X+1]) x [ymap[Y], ymap[Y+1]). Thin (1 px) lines then show as a continuous,
         * dimmer line instead of being dropped on skipped rows/columns. Same global mapping as the
         * nearest copy, so a redrawn rect still matches its neighbours exactly. */
        int is565 = (ds.dsBmih.biCompression == BI_BITFIELDS && ds.dsBitfields[1] == 0x07E0);
        int rs = is565 ? 11 : 10, gm = is565 ? 63 : 31;
        static unsigned recip[129];                    /* recip[n] = 65536/n: divide by multiply+shift */
        if (g_scale16 > 16 && g_scale16 <= 32) {
            /* Fast path for scale <= 2: every block is 1 or 2 source pixels on each axis, so average
             * packed 16bpp pixels directly - (a & b) + (((a ^ b) & M) >> 1) is the per-channel floor
             * average of a and b (M clears each channel's lowest bit so the halves can't carry into
             * the next channel). A 2x2 block = average of the two row averages. */
            const unsigned M = is565 ? 0xF7DEu : 0x7BDEu;
            #define AVG2(a, b) (((a) & (b)) + ((((a) ^ (b)) & M) >> 1))
            for (Y = Y0; Y < Y1; Y++) {
                int y0 = g_ymap[Y], yy1 = y0 + 1;
                const WORD *r0, *r1;
                WORD *d = (WORD*)(g_smallBits + (size_t)Y * g_smallStride);
                DWORD *d32 = (DWORD*)d;
                r0 = (const WORD*)(src + (size_t)(topDown ? y0 : (bh - 1 - y0)) * sStride);
                if (g_ymap[Y + 1] - y0 >= 2 && yy1 < bh)
                    r1 = (const WORD*)(src + (size_t)(topDown ? yy1 : (bh - 1 - yy1)) * sStride);
                else
                    r1 = r0;
                if (r1 == r0) {
                    for (X = X0; X < X1; X++) {
                        int x0 = g_xmap[X];
                        unsigned a = r0[x0];
                        if (g_xmap[X + 1] - x0 >= 2 && x0 + 1 < bw) a = AVG2(a, (unsigned)r0[x0 + 1]);
                        if (to32) d32[X] = g_lut[a]; else d[X] = (WORD)a;
                    }
                } else {
                    for (X = X0; X < X1; X++) {
                        int x0 = g_xmap[X];
                        unsigned a = r0[x0], b = r1[x0];
                        if (g_xmap[X + 1] - x0 >= 2 && x0 + 1 < bw) {
                            a = AVG2(a, (unsigned)r0[x0 + 1]);
                            b = AVG2(b, (unsigned)r1[x0 + 1]);
                        }
                        a = AVG2(a, b);
                        if (to32) d32[X] = g_lut[a]; else d[X] = (WORD)a;
                    }
                }
            }
            #undef AVG2
            goto present;
        }
        if (!recip[1]) { int i; for (i = 1; i <= 128; i++) recip[i] = 65536u / (unsigned)i; }
        for (Y = Y0; Y < Y1; Y++) {
            int y0 = g_ymap[Y], y1 = g_ymap[Y + 1], ny, k;
            const WORD *rows[16];                         /* source rows of this block (<= 9 at x8) */
            WORD *d = (WORD*)(g_smallBits + (size_t)Y * g_smallStride);
            DWORD *d32 = (DWORD*)d;
            if (y1 <= y0) y1 = y0 + 1;
            if (y1 > bh) y1 = bh;
            ny = y1 - y0; if (ny > 16) ny = 16;
            for (k = 0; k < ny; k++) {
                int yy = y0 + k;
                rows[k] = (const WORD*)(src + (size_t)(topDown ? yy : (bh - 1 - yy)) * sStride);
            }
            for (X = X0; X < X1; X++) {
                int x0 = g_xmap[X], x1 = g_xmap[X + 1], xx, nx;
                unsigned r = 0, g = 0, b = 0, inv;
                if (x1 <= x0) x1 = x0 + 1;
                if (x1 > bw) x1 = bw;
                nx = x1 - x0;
                for (k = 0; k < ny; k++) {
                    const WORD *s = rows[k] + x0;
                    for (xx = 0; xx < nx; xx++) {
                        unsigned c = s[xx];
                        r += (c >> rs) & 31; g += (c >> 5) & gm; b += c & 31;
                    }
                }
                inv = recip[(nx * ny) <= 128 ? nx * ny : 128];
                {
                    unsigned v = (((r * inv) >> 16) << rs) | (((g * inv) >> 16) << 5) | ((b * inv) >> 16);
                    if (to32) d32[X] = g_lut[v & 0xFFFF]; else d[X] = (WORD)v;
                }
            }
        }
        goto present;
    }
    for (Y = Y0; Y < Y1; Y++) {
        int syy = g_ymap[Y];
        const BYTE *srow = src + (size_t)(topDown ? syy : (bh - 1 - syy)) * sStride;
        BYTE *drow = g_smallBits + (size_t)Y * g_smallStride;
        switch (Bpp) {
        case 1: for (X = X0; X < X1; X++) drow[X] = srow[g_xmap[X]]; break;
        case 2: { WORD *d = (WORD*)drow; DWORD *d32 = (DWORD*)drow; const WORD *s = (const WORD*)srow;
                  if (to32) for (X = X0; X < X1; X++) d32[X] = g_lut[s[g_xmap[X]]];
                  else      for (X = X0; X < X1; X++) d[X] = s[g_xmap[X]]; } break;
        case 4: { DWORD *d = (DWORD*)drow; const DWORD *s = (const DWORD*)srow;
                  for (X = X0; X < X1; X++) d[X] = s[g_xmap[X]]; } break;
        default: for (X = X0; X < X1; X++) { const BYTE *p = srow + g_xmap[X] * 3; BYTE *q = drow + X * 3;
                  q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; } break;
        }
    }
present:
    offX = MulDiv(x - sx, tw, bw); offY = MulDiv(y - sy, th, bh);  /* usually 0: window coords = buffer coords */
    {
        LARGE_INTEGER b0, b1;
        if (g_stats && g_qpf.QuadPart) QueryPerformanceCounter(&b0);
        g_realBitBlt(hd, X0 + offX, Y0 + offY, X1 - X0, Y1 - Y0, g_smallDC, X0, Y0, SRCCOPY);
        if (g_stats && g_qpf.QuadPart) {
            QueryPerformanceCounter(&b1);
            g_st.blitMs += (double)(b1.QuadPart - b0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
        }
    }
    return 1;
}

#define REFRESH_MS 1000             /* periodic full repaint while wide-active (artifact safety net) */
#define TIMER_MS 250                /* refresh timer tick on the detail window */
static DWORD g_lastFullRefresh = 0;

/* Last wide-buffer present: the engine's memory DC + bitmap and the window it went to. */
static HDC     g_lastSrcDC = NULL;
static HGDIOBJ g_lastSrcBmp = NULL;
static HWND    g_lastDestWnd = NULL;

/* ---- Scrolling ---------------------------------------------------------------------------
 * GFXClippedSurface::ScrollScreen (0x004A9890) shifts what is already on screen with
 * ScrollWindowEx(detail window, dx, dy) - at zoom 1 by dx/dy in BUFFER pixels. On the shrunk
 * picture that moves everything ~2x too far (smeared until the tiles redraw). By the time it is
 * called the engine has already scrolled its buffer in memory, so at the wide level we skip the
 * screen shift and repaint the whole view from the buffer through the same global mapping. */
typedef int (WINAPI *ScrollWindowEx_t)(HWND, int, int, const RECT*, const RECT*, HRGN, LPRECT, UINT);
static ScrollWindowEx_t g_realScrollWindowEx = NULL;
static volatile LONG g_scrollLogged = 0;
static int FastShrink(HDC hd, int x, int y, int w, int h, HDC hs, HGDIOBJ ob, int sx, int sy, int tw, int th);

/* Repaint the whole detail view from the wide buffer (as of the last present) through the global
 * mapping. Returns 1 if done. Used by the scroll hook and the refresh timer. */
static int RepaintAll(HWND hw) {
    RECT rc; BITMAP bm; HDC dc; int ok;
    if (!(WideActive() && g_scale16 > 16 && hw && hw == g_lastDestWnd && g_lastSrcDC && g_lastSrcBmp)) return 0;
    if (!g_realGCR(hw, &rc) || rc.right <= rc.left) return 0;
    if (GetObjectA(g_lastSrcBmp, sizeof(bm), &bm) != sizeof(bm) || !IsWideMapBuffer(&bm)) return 0;
    dc = GetDC(hw);
    if (!dc) return 0;
    ok = FastShrink(dc, 0, 0, bm.bmWidth, bm.bmHeight, g_lastSrcDC, g_lastSrcBmp, 0, 0,
                    rc.right - rc.left, rc.bottom - rc.top);
    ReleaseDC(hw, dc);
    return ok;
}

static int WINAPI MyScrollWindowEx(HWND hw, int dx, int dy, const RECT *scroll, const RECT *clip,
                                   HRGN rgn, LPRECT upd, UINT flags) {
    if (RepaintAll(hw)) {
        g_st.scrolls++;
        if (InterlockedExchange(&g_scrollLogged, 1) == 0)
            LogD("scroll: ScrollWindowEx(%d,%d) replaced by a full repaint from the wide buffer", dx, dy);
        if (upd) SetRectEmpty(upd);
        return SIMPLEREGION;
    }
    return g_realScrollWindowEx(hw, dx, dy, scroll, clip, rgn, upd, flags);
}

/* Timer (WM_TIMER on the detail window: the game's UI thread, BETWEEN frames, so the buffer is
 * complete - and it fires even in a still scene where nothing else presents).
 * Known issue it does NOT fix: white selection-box lines can stay on screen while the units they
 * belong to remain selected; they are redrawn by the engine every frame (they survived engine
 * full-background redraws and full presents in testing) and vanish as soon as the units are
 * deselected. Not resolved - see TESTLOG.md, Test 6. */
static void OnRefreshTimer(HWND hw) {
    DWORD now = GetTickCount();
    if (!(WideActive() && g_scale16 > 16)) return;
    /* slow safety net for anything stale on screen only */
    if (now - g_lastFullRefresh >= REFRESH_MS) {
        g_lastFullRefresh = now;
        if (RepaintAll(hw)) g_st.refreshes++;
    }
}

static BOOL WINAPI MyBitBlt(HDC hd,int x,int y,int w,int h,HDC hs,int sx,int sy,DWORD rop) {
    if (WideActive() && hs) {
        g_st.blits++;
        StatsMaybeFlush();
        HGDIOBJ ob = GetCurrentObject(hs, OBJ_BITMAP);
        BITMAP bm;
        if (ob && GetObjectA(ob, sizeof(bm), &bm) == sizeof(bm)) {
            int tw = 0, th = 0; const char *via = "trueW";
            HWND dw = WindowFromDC(hd);
            RECT rc;
            if (dw && g_realGCR(dw, &rc) && rc.right > rc.left) {
                tw = rc.right - rc.left; th = rc.bottom - rc.top; via = "WindowFromDC";
            }
            if (tw <= 0 && g_trueW > 0) { tw = g_trueW; th = g_trueH; }
            if (tw > 0 && bm.bmWidth > tw && !IsWideMapBuffer(&bm)) {
                /* wider than its target but NOT our buffer: the old rule would have shrunk this */
                if (InterlockedExchange(&g_skipLogged, 1) == 0)
                    LogD("present: left a %dx%d blit unscaled (not the %ldx%ld map buffer)",
                        bm.bmWidth, bm.bmHeight, g_wideW, g_wideH);
            } else if (tw > 0 && bm.bmWidth > tw) {   /* our over-sized map buffer -> shrink to fit */
                /* first time we identify the detail window, subclass it to scale mouse coords */
                if (dw && InterlockedCompareExchange(&g_subclassed, 1, 0) == 0) {
                    g_detailWnd = dw;
                    g_origProc = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(dw, GWLP_WNDPROC, (LONG_PTR)MyDetailProc);
                    SetTimer(dw, ZOOM_TIMER_ID, TIMER_MS, NULL);
                    LogD("subclassed detail window 0x%08X for mouse scaling (xWIDEN) + refresh timer", (DWORD)(DWORD_PTR)dw);
                }
                int dx = MulDiv(x, tw, bm.bmWidth), dy = MulDiv(y, th, bm.bmHeight);
                int dw2 = MulDiv(w, tw, bm.bmWidth), dh2 = MulDiv(h, th, bm.bmHeight);
                if (InterlockedExchange(&g_blitLogged, 1) == 0)
                    LogD("present shrink: buf=%dx%d -> %dx%d (via %s)", bm.bmWidth, bm.bmHeight, tw, th, via);
                {
                    LARGE_INTEGER t0, t1; BOOL r;
                    if (dw) g_st.shrinkWnd++; else g_st.shrinkMem++;
                    if (w >= bm.bmWidth - 64 && h >= bm.bmHeight - 64) g_st.shrinkFull++;
                    g_st.srcPx += (double)w * h;
                    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
                    QueryPerformanceCounter(&t0);
                    if (dw && rop == SRCCOPY && FastShrink(hd, x, y, w, h, hs, ob, sx, sy, tw, th)) {
                        if (InterlockedExchange(&g_fastLogged, 1) == 0)
                            LogD("present: fast shrink (own resample + 1:1 BitBlt)");
                        g_lastSrcDC = hs; g_lastSrcBmp = ob; g_lastDestWnd = dw;   /* for scroll/timer repaints */
                        r = TRUE;
                    } else {
                        if (InterlockedExchange(&g_fallbackLogged, 1) == 0)
                            LogD("present: StretchBlt fallback (dest window=%s rop=0x%08lX)", dw ? "yes" : "no", rop);
                        SetStretchBltMode(hd, COLORONCOLOR);
                        r = StretchBlt(hd, dx, dy, dw2, dh2, hs, sx, sy, w, h, rop);
                    }
                    QueryPerformanceCounter(&t1);
                    g_st.ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
                    return r;
                }
            }
        }
    }
    return g_realBitBlt(hd, x, y, w, h, hs, sx, sy, rop);
}

/* Patch one IAT slot of Outpost2.exe (match by import name, fall back to current address). */
static int HookIAT(const char *dll, const char *fn, void *repl, void **orig) {
    BYTE *base = (BYTE*)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    HMODULE hmod = GetModuleHandleA(dll);
    FARPROC target = hmod ? GetProcAddress(hmod, fn) : NULL;
    IMAGE_IMPORT_DESCRIPTOR *d;
    if (!impRva) return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR*)(base + impRva); d->Name; d++) {
        const char *mod = (const char*)(base + d->Name);
        IMAGE_THUNK_DATA *oft, *ft;
        if (lstrcmpiA(mod, dll) != 0) continue;
        oft = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk) : NULL;
        ft  = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; ft->u1.Function; ft++) {
            int match = 0;
            if (oft) {
                if (!(oft->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
                    if (lstrcmpA((char*)ibn->Name, fn) == 0) match = 1;
                }
                oft++;
            }
            if (!match && target && (FARPROC)ft->u1.Function == target) match = 1;
            if (match) {
                DWORD op;
                if (orig) *orig = (void*)ft->u1.Function;
                if (VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &op)) {
                    ft->u1.Function = (ULONG_PTR)repl;
                    VirtualProtect(&ft->u1.Function, sizeof(void*), op, &op);
                    return 1;
                }
                return 0;
            }
        }
    }
    return 0;
}

/* BitBlt and the mouse scaling first: the GetClientRect lie builds an over-sized buffer that only the
 * BitBlt hook shrinks back, and puts the map in coordinates only the GetAbsolutePos hook scales the
 * mouse into - so never install the lie without both. Returns 1 when all three are in place. */
static int InstallHooks(void) {
    int b = HookIAT("gdi32.dll",  "BitBlt",        (void*)MyBitBlt,        (void**)&g_realBitBlt);
    int m = b ? HookDetailMouse() : 0;
    /* The engine's 400-byte bit-vector limit: OPU (OPUPatch.dll) hooks the middle of
     * Viewport::SetSurface (a jmp at 0x0046F1D2, where the redraw vector pointer is stored) and
     * supports larger vectors - its ForceFullRedraw relies on that hook too (Test 11). So under OPU
     * we just drop our own cap and leave SetSurface to OPU. Without OPU the cap stays. */
    {
        int opu = GetModuleHandleA("OPUPatch.dll") != NULL;
        BYTE *p = (BYTE*)0x0046F1D2;
        if (!IsBadReadPtr(p, 8))
            LogD("bit vectors: bytes at 0x0046F1D2 = %02X %02X %02X %02X %02X %02X %02X %02X (vanilla 89 86 08 08 00 00 ..), OPUPatch %s",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], opu ? "loaded" : "not loaded");
        g_bigVectors = g_bigView && opu;
        LogD("bit vectors: %s", g_bigVectors ? "OPU handles SetSurface - 400-byte cap lifted"
                               : !g_bigView ? "ZoomBigView=0 - 400-byte cap kept"
                                            : "no OPUPatch - 400-byte cap kept");
    }
    int a = (b && m) ? HookIAT("user32.dll", "GetClientRect", (void*)MyGetClientRect, (void**)&g_realGCR) : 0;
    int s = a ? HookIAT("user32.dll", "ScrollWindowEx", (void*)MyScrollWindowEx, (void**)&g_realScrollWindowEx) : 0;
    /* Mission-load detection for the log / per-mission re-check only; optional (map-size changes are
     * still caught without it). */
    int l = a ? HookIAT("kernel32.dll", "LoadLibraryA", (void*)MyLoadLibraryA, (void**)&g_realLoadLibraryA) : 0;
    if (!l || g_debug) Log("hooks: LoadLibraryA(mission detect)=%s", l ? "OK" : "FAILED (same-size map reloads not detected)");
    Log("hooks: BitBlt=%s  GetAbsolutePos(mouse)=%s  GetClientRect=%s  ScrollWindowEx=%s", b ? "OK" : "FAILED",
        m ? "OK" : (b ? "FAILED" : "skipped"), a ? "OK" : ((b && m) ? "FAILED" : "skipped"),
        s ? "OK" : (a ? "FAILED (scrolling will smear)" : "skipped"));
    if (!a || !b || !m) Log("WARNING: a hook failed - wide zoom will not work; report this log.");
    return a && b && m;
}

static void ReadIni(void) {
    char path[MAX_PATH];
    wsprintfA(path, "%s\\outpost2.ini", GameDir());       /* the LIVE ini (OPU folder under OPU) */
    g_wideZoom = GetPrivateProfileIntA("Game", "WideZoom",  1, path);
    g_widen    = GetPrivateProfileIntA("Game", "ZoomWiden", 2, path);
    if (g_widen < 2) g_widen = 2;                        /* must widen to show more */
    if (g_widen > 8) g_widen = 8;                        /* keep scaled mouse coords inside 16 bits */
    g_smooth   = GetPrivateProfileIntA("Game", "ZoomSmooth", 1, path) != 0;
    g_stats    = GetPrivateProfileIntA("Game", "ZoomStats",  0, path) != 0;
    g_blit32   = GetPrivateProfileIntA("Game", "ZoomBlit32", 1, path) != 0;
    g_bigView  = GetPrivateProfileIntA("Game", "ZoomBigView", 1, path) != 0;
    g_debug    = GetPrivateProfileIntA("Zoom", "Debug", 0, path) != 0;   /* 1 = full diagnostic log */
    Log("WideZoom=%d ZoomWiden=%d ZoomSmooth=%d ZoomBlit32=%d ZoomBigView=%d ZoomStats=%d Debug=%d  (%s)",
        g_wideZoom, g_widen, g_smooth, g_blit32, g_bigView, g_stats, g_debug, path);
}

/* Watch the engine zoom global and log every transition (read-only poll). */
static volatile LONG g_stopPoll = 0;
static DWORD WINAPI ZoomPollThread(LPVOID arg) {
    int last = -999;
    (void)arg;
    while (!g_stopPoll) {
        if (!IsBadReadPtr((void*)ADDR_ZOOM_GLOBAL, 4)) {
            int z = *(volatile int*)ADDR_ZOOM_GLOBAL;
            if (z != last) {
                if (last == -999) LogD("zoom: initial value = %d", z);
                else              LogD("zoom changed: %d -> %d  (wide level %s)",
                                      last, z, (z == 1 && g_hooked) ? "ACTIVE" : "off");
                last = z;
            }
        }
        Sleep(150);
    }
    return 0;
}

static volatile LONG g_done = 0;
static void Init(void) {
    if (InterlockedExchange(&g_done, 1) != 0) return;    /* op2ext may call more than one entry point */
    Log("=== op2ext init (InitMod) - Zoom.dll wide-zoom build ===");
    ReadIni();
    {
        /* Verify-before-patch: the caller address ranges and coordinate maths below are only valid
         * for the vanilla renderer, so install nothing unless BOTH reference prologues match. */
        int okPresent = VerifyBytes("present (FUN_004a9410)",      ADDR_PRESENT,      REF_PRESENT,      16);
        int okAlloc   = VerifyBytes("AllocSurface (FUN_004a9170)", ADDR_ALLOCSURFACE, REF_ALLOCSURFACE, 16);
        if (!g_wideZoom)
            Log("WideZoom=0 -> hooks not installed (vanilla zoom).");
        else if (!okPresent || !okAlloc)
            Log("ABORT: renderer bytes differ from retail 1.3.6 / OPU 1.4.1 - unknown Outpost2.exe, "
                "hooks NOT installed (vanilla zoom).");
        else if (InstallHooks())
            g_hooked = 1;
    }
    CreateThread(NULL, 0, ZoomPollThread, NULL, 0, NULL);
    Log(g_hooked ?"loaded OK. Wide level engages at engine zoom 1 (zoom all the way out)."
               : "loaded, but wide zoom is OFF (see above) - vanilla zoom only.");
}

/* ---- op2ext module entry points ---- */
__declspec(dllexport) void InitMod(void)     { Init(); }
__declspec(dllexport) void mod_init(void)    { Init(); }
__declspec(dllexport) void mod_run(void)     { }
__declspec(dllexport) void mod_destroy(void) { }
__declspec(dllexport) void DestroyMod(void)  { }
__declspec(dllexport) void RunMod(void)      { }

static volatile LONG g_detached = 0;
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = h; DisableThreadLibraryCalls(h);
        LogD("=== process attach - Outpost2 loaded Zoom.dll ===");
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stopPoll = 1;
        if (InterlockedExchange(&g_detached, 1) == 0)
            LogD("=== process detach - Zoom.dll unloading (game exit) ===");
    }
    return TRUE;
}
