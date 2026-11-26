/* op2shot.dll - op2ext module for Outpost 2 (OPU 1.4.1).
 *
 * Saves a .bmp on a hotkey, in three modes:
 *
 *   F12  viewport only  (op2shot_view_*.bmp) - the rendered game view, no UI panels.
 *   F11  full window    (op2shot_full_*.bmp) - the whole OS window: title bar, borders, viewport
 *                                              and every UI panel.
 *   F10  full map       (op2shot_map_*.bmp)  - the ENTIRE map rendered from the in-memory tile grid
 *                                              (not just the viewport), reflecting live game state.
 *
 * WHY THREE MECHANISMS: OPU runs the renderer in GDI windowed mode, so the composited frame lives on
 * GDI surfaces we can read - but the back-buffer only ever holds the VIEWPORT.
 *   - Full window: GetWindowDC(gameWnd) + BitBlt the whole window, with the size read in the
 *     window's own DPI mode so Windows display scaling doesn't add a black margin. See CaptureWindow.
 *   - Viewport only: the game presents the view with BitBlt(windowDC, ..., viewSurfaceDC, ...), so
 *     the present's SOURCE DC is the view surface. We IAT-hook BitBlt, arm a one-shot capture on the
 *     key, and on the next present from a viewport-sized surface (>= MinWidth x MinHeight, which
 *     skips the small panels) save that WHOLE surface. See MyBitBlt.
 *   - Full map cannot be screen-grabbed (off-viewport map is never drawn), so we read the engine's
 *     in-memory tile grid + tilesets and render the image ourselves - only the playable area (the
 *     engine's map clip rect), so maps that don't wrap lose their unused padding. See RenderFullMap.
 *
 * All three run on the game's own thread (from the keyboard hook or the BitBlt hook), so they see
 * a consistent frame / map and never race the engine.
 *
 * Output: <game dir>\screenshots\op2shot_{view|full|map}_YYYYMMDD_HHMMSS[_N].bmp (never overwritten)
 * Log:    <game dir>\logs\op2shot.log
 * <game dir> is the folder holding the live outpost2.ini - under OPU, the OPU folder (see GameDir).
 *
 * Install (OPU 1.4.1): copy to OPU\op2shot.dll and add to OPU\outpost2.ini:
 *     [ExternalModules]  Screenshot=yes
 *     [Screenshot]       Dll=op2shot.dll
 * Optional [Screenshot] keys: HotKey/WindowKey/MapKey (VK codes), MapScale, MapLight, MapDiag,
 * MinWidth/MinHeight.
 */
#include <windows.h>
#include <string.h>
#include <stdarg.h>

#define GAMEWND_PTR 0x005756C8u       /* HWND of the game window (the engine's main frame, +8) */

/* The engine's single static Map object (base 0x0054F7F8 in Outpost2.exe, same in OPU 1.4.1), as
 * read by GetTile @ 0x476d00 and the renderer. Each define is the ADDRESS of one field; the code
 * reads the value stored there. Fields not listed (pixel width, unit pool, light tables, ...) are
 * not needed here. */
#define MAP_TILEXMASK    0x0054F804u  /* int tileXMask  (wraps tile X; = tileWidth-1) */
#define MAP_TILEWIDTH    0x0054F808u  /* int tileWidth  (tiles) */
#define MAP_TILEHEIGHT   0x0054F810u  /* int tileHeight (tiles) */
#define MAP_CLIPX1       0x0054F814u  /* int clip rect, inclusive tile coords: the playable area */
#define MAP_CLIPY1       0x0054F818u  /*   (x 32..width+31 on maps that don't wrap; x -1..0x7FFFFFFF */
#define MAP_CLIPX2       0x0054F81Cu  /*   on 512-wide maps that do; y 0..height-1)                   */
#define MAP_CLIPY2       0x0054F820u
#define MAP_LGTILEHEIGHT 0x0054F82Cu  /* log2(tileHeight); a dword, only its low byte is used (as the
                                         engine does: it is a shift count) */
#define MAP_TILEDATA     0x0054FC5Cu  /* int*  tileData base (packed-Tile grid, one dword per tile) */
#define MAP_TILESETARR   0x0054FC60u  /* TileSet** array base, indexed by tilesetIndex */
#define MAP_TERRAINMGR   0x0054FC64u  /* TerrainTypeManager*; +4 = TileMapping[] base */

/* ---- settings (read once by ReadIni) and state ---- */
static HINSTANCE g_hInst;             /* this DLL, for SetWindowsHookEx */
static int  g_key    = VK_F12;       /* [Screenshot] HotKey   - viewport-only capture (no UI) */
static int  g_winKey = VK_F11;        /* [Screenshot] WindowKey - full window (viewport + all UI) */
static int  g_mapKey = VK_F10;        /* [Screenshot] MapKey   - full-map terrain render from memory */
static int  g_mapScale = 1;           /* [Screenshot] MapScale - 1=32px/tile (full), 2=16, 4=8, ... */
static int  g_mapLight = 0;           /* [Screenshot] MapLight - light-level palette (0 = full day) */
static int  g_mapDiag  = 0;           /* [Screenshot] MapDiag  - 1 = log tile/palette diagnostics */
static int  g_minW = 400;             /* viewport id: source bitmap must be at least this wide ... */
static int  g_minH = 240;             /* ... and this tall (panels are smaller) */
static volatile LONG g_capture = 0;   /* one-shot: armed by the hotkey, cleared on capture */
static HHOOK g_kbHook = NULL;         /* the thread keyboard hook (KbProc) */

typedef BOOL (WINAPI *BitBlt_t)(HDC,int,int,int,int,HDC,int,int,DWORD);
static BitBlt_t g_realBitBlt = NULL;  /* what was in the IAT slot before us: gdi32's BitBlt, or another
                                         module's hook that we chain to */

/* The game's runtime folder: where the live outpost2.ini is read and logs\ and screenshots\ are
 * written. Under OPU, OPULauncher starts the game with the current directory = the OPU folder, while
 * Outpost2.exe itself sits one level up (next to an old, unused outpost2.ini) - so prefer the current
 * directory when it holds an outpost2.ini, and fall back to the exe's folder. Resolved once, without
 * a trailing backslash. */
static char g_gameDir[MAX_PATH];
static const char *GameDir(void) {
    if (!g_gameDir[0]) {
        char dir[MAX_PATH], ini[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(MAX_PATH, dir);
        if (n > 0 && n < MAX_PATH - 16) {
            if (dir[n - 1] == '\\') dir[n - 1] = '\0';                 /* drive root: drop the '\' */
            wsprintfA(ini, "%s\\outpost2.ini", dir);
            if (GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES) { lstrcpyA(g_gameDir, dir); return g_gameDir; }
        }
        GetModuleFileNameA(NULL, dir, MAX_PATH);                       /* fallback: the exe's folder */
        { char *bs = strrchr(dir, '\\'); if (bs) *bs = '\0'; else dir[0] = '\0'; }
        lstrcpyA(g_gameDir, dir[0] ? dir : ".");
    }
    return g_gameDir;
}

/* ---- logging (<game dir>\logs\op2shot.log) ----
 * LogPath builds the log file name, creating logs\ if needed. Log appends one timestamped,
 * printf-style line (wvsprintfA: no %f; lines are capped at 511 chars); the file is opened and
 * closed per line so the log survives a crash. */
static void LogPath(char *out) {
    const char *dir = GameDir();
    { char logdir[MAX_PATH]; wsprintfA(logdir, "%s\\logs", dir); CreateDirectoryA(logdir, NULL); }
    wsprintfA(out, "%s\\logs\\op2shot.log", dir);
}
static void Log(const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt); wvsprintfA(buf, fmt, ap); va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    char path[MAX_PATH]; LogPath(path);
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

/* Create a new, uniquely named .bmp in <game dir>\screenshots\ and return its open handle (path in
 * `out`), or INVALID_HANDLE_VALUE. `tag` distinguishes the mode: "view" (viewport only), "full"
 * (whole window), "map" (full-map render). The name is op2shot_<tag>_YYYYMMDD_HHMMSS.bmp; if that
 * already exists (two shots in the same second) it becomes ..._HHMMSS_2.bmp, _3, ... CREATE_NEW
 * means an existing screenshot is never overwritten. */
static HANDLE CreateShotFile(char *out, const char *tag) {
    char shotdir[MAX_PATH]; wsprintfA(shotdir, "%s\\screenshots", GameDir()); CreateDirectoryA(shotdir, NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    char stem[MAX_PATH];
    wsprintfA(stem, "%s\\op2shot_%s_%04d%02d%02d_%02d%02d%02d",
              shotdir, tag, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    int i;
    for (i = 1; i <= 99; i++) {
        if (i == 1) wsprintfA(out, "%s.bmp", stem); else wsprintfA(out, "%s_%d.bmp", stem, i);
        HANDLE f = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) return f;
        if (GetLastError() != ERROR_FILE_EXISTS) break;                /* a real error: stop */
    }
    return INVALID_HANDLE_VALUE;
}

/* WriteFile that reports a short or failed write. `*ok` is cleared on the first failure and later
 * writes are skipped, so the caller checks it once at the end. */
static void WriteAll(HANDLE f, const void *p, DWORD n, int *ok) {
    DWORD w = 0;
    if (!*ok) return;
    if (!WriteFile(f, p, n, &w, NULL) || w != n) *ok = 0;
}

/* Close a screenshot file and log the result. A failed write (e.g. disk full) deletes the
 * cut-off file instead of leaving a broken .bmp behind. */
static void FinishShotFile(HANDLE f, const char *path, int ok, const char *what) {
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(f);
    if (ok) Log("%s saved %s", what, path);
    else  { DeleteFileA(path); Log("%s: write to %s FAILED (err %lu) - file removed", what, path, err); }
}

/* Copy region (sx,sy,w,h) of `src` into a 24bpp DIB and write a .bmp file. We blit into our own DIB
 * section (rather than GetDIBits on a surface whose bitmap is selected into `src`, which GetDIBits
 * forbids), and use the REAL BitBlt where available to avoid re-entering our own hook. Works for
 * either a surface memory DC (viewport) or a window DC (full window).
 * A 24bpp bottom-up DIB has exactly the BMP file's pixel layout (rows padded to 4 bytes), so its
 * bits are written straight after the two headers. */
static void SaveDcRegion(HDC src, int sx, int sy, int w, int h, const char *tag) {
    HDC memdc = CreateCompatibleDC(src);
    if (!memdc) { Log("capture: CreateCompatibleDC failed"); return; }
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = h;     /* +height = bottom-up (BMP file order) */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(src, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(memdc, dib);
        (g_realBitBlt ? g_realBitBlt : BitBlt)(memdc, 0, 0, w, h, src, sx, sy, SRCCOPY);
        SelectObject(memdc, old);
        int stride = (w * 3 + 3) & ~3; DWORD imgSize = (DWORD)stride * h;
        BITMAPFILEHEADER fh; ZeroMemory(&fh, sizeof(fh));
        fh.bfType    = 0x4D42;                               /* 'BM' */
        fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
        fh.bfSize    = fh.bfOffBits + imgSize;
        bi.bmiHeader.biSizeImage = imgSize;
        char path[MAX_PATH];
        HANDLE f = CreateShotFile(path, tag);
        if (f != INVALID_HANDLE_VALUE) {
            int ok = 1;
            WriteAll(f, &fh, sizeof(fh), &ok);
            WriteAll(f, &bi.bmiHeader, sizeof(BITMAPINFOHEADER), &ok);
            WriteAll(f, bits, imgSize, &ok);
            Log("capture: %s %dx%d", tag, w, h);
            FinishShotFile(f, path, ok, "capture:");
        } else {
            Log("capture: could not create a file in screenshots\\ (err %lu)", GetLastError());
        }
    } else {
        Log("capture: CreateDIBSection failed");
    }
    if (dib) DeleteObject(dib);
    DeleteDC(memdc);
}

/* Hooked BitBlt: when a capture is armed, the first present whose SOURCE SURFACE is viewport-sized
 * (>= MinWidth x MinHeight, which skips the small UI panels) is saved - the WHOLE surface, which is
 * the complete current view. Not just the area this call copies: the game often presents only the
 * part of the view that changed (e.g. 1138x481 after a scroll), so saving the copied area gives
 * fragments. The view surface is exactly the viewport size.
 * Inert (single branch) the rest of the time. */
static BOOL WINAPI MyBitBlt(HDC hd,int x,int y,int w,int h,HDC hs,int sx,int sy,DWORD rop) {
    if (g_capture && hs) {
        HGDIOBJ ob = GetCurrentObject(hs, OBJ_BITMAP); BITMAP bm;
        if (ob && GetObjectA(ob, sizeof(bm), &bm) == sizeof(bm)
            && bm.bmWidth >= g_minW && bm.bmHeight >= g_minH) {
            InterlockedExchange(&g_capture, 0);             /* clear before save (re-entry safety) */
            Log("capture: %dx%d view surface (this present copied %dx%d of it)", bm.bmWidth, bm.bmHeight, w, h);
            SaveDcRegion(hs, 0, 0, bm.bmWidth, bm.bmHeight, "view");
        }
    }
    return g_realBitBlt(hd, x, y, w, h, hs, sx, sy, rop);
}

/* Resolve the game window: the engine's HWND global, falling back to the foreground window. */
static HWND GameWnd(void) {
    HWND h = NULL;
    if (!IsBadReadPtr((void*)GAMEWND_PTR, sizeof(HWND))) h = *(HWND*)GAMEWND_PTR;
    if (!h || !IsWindow(h)) h = GetForegroundWindow();
    return h;
}

/* DPI helpers (Windows 10 1607+), looked up at run time so the DLL still loads on older Windows,
 * where they are simply skipped (no per-window DPI there, so no mismatch to correct). */
typedef void *DpiCtx;
typedef DpiCtx (WINAPI *GetWindowDpiAwarenessContext_t)(HWND);
typedef DpiCtx (WINAPI *SetThreadDpiAwarenessContext_t)(DpiCtx);
typedef UINT   (WINAPI *GetDpiForWindow_t)(HWND);

/* Full-window capture: grab the ENTIRE game window - title bar, borders and the whole client
 * (viewport + every UI panel) - straight off its window DC. GetWindowDC + GetWindowRect cover the
 * non-client frame too; the composited frame is already on the window, so no present hook is needed.
 *
 * DPI: the window DC is in the window's own coordinates. If this thread runs with a different DPI
 * awareness than the window (e.g. the window is DPI-unaware and Windows scales it 125%), GetWindowRect
 * returns the scaled size while the DC only holds the unscaled pixels, and the extra comes out
 * black. So the size is read with this thread temporarily switched to the window's own awareness.
 * Seen on OPU 1.4.1 at 125% scaling: the thread saw 2560x1440, the window (96 dpi) has 2048x1152.
 * The log line records both sizes and the window's DPI. */
static void CaptureWindow(void) {
    HWND h = GameWnd();
    if (!h) { Log("window capture: no game window"); return; }
    HMODULE u32 = GetModuleHandleA("user32.dll");
    GetWindowDpiAwarenessContext_t pGetWinCtx = (GetWindowDpiAwarenessContext_t)GetProcAddress(u32, "GetWindowDpiAwarenessContext");
    SetThreadDpiAwarenessContext_t pSetThrCtx = (SetThreadDpiAwarenessContext_t)GetProcAddress(u32, "SetThreadDpiAwarenessContext");
    GetDpiForWindow_t              pDpiForWin = (GetDpiForWindow_t)GetProcAddress(u32, "GetDpiForWindow");
    DpiCtx oldCtx = NULL;
    RECT raw; GetWindowRect(h, &raw);                              /* as this thread sees it */
    if (pGetWinCtx && pSetThrCtx) oldCtx = pSetThrCtx(pGetWinCtx(h));
    RECT wr; GetWindowRect(h, &wr);                                /* in the window's own coordinates */
    int w = wr.right - wr.left, ht = wr.bottom - wr.top;
    if (w <= 0 || ht <= 0) { Log("window capture: empty window rect"); if (oldCtx) pSetThrCtx(oldCtx); return; }
    HDC wdc = GetWindowDC(h);
    if (!wdc) { Log("window capture: GetWindowDC failed"); if (oldCtx) pSetThrCtx(oldCtx); return; }
    Log("window capture: %dx%d (thread saw %dx%d, window dpi %u) hwnd=%08X", w, ht,
        (int)(raw.right - raw.left), (int)(raw.bottom - raw.top), pDpiForWin ? pDpiForWin(h) : 0,
        (unsigned)(DWORD_PTR)h);
    SaveDcRegion(wdc, 0, 0, w, ht, "full");
    ReleaseDC(h, wdc);
    if (oldCtx) pSetThrCtx(oldCtx);                                /* back to this thread's own awareness */
}

/* Full-map terrain render straight from the in-memory tile grid (live state: laid tubes, walls,
 * bulldozed/levelled terrain, lava, scorch - everything the engine has baked into tileMappingIndex).
 * For every cell: tile word -> tileMappingIndex -> TileMapping{tilesetIndex,tileGraphicIndex} ->
 * TileSet 8bpp pixels -> full-daylight light-level palette (level 0, 16bpp 555) -> RGB. The image
 * (the playable area's tiles x 32/MapScale px each) is streamed to a 24bpp BMP one tile row at a
 * time, so even huge maps use bounded memory. Runs synchronously on the game thread (called from
 * the keyboard hook), so the snapshot is sim-consistent; big maps freeze the game briefly.
 *
 * Tile word address: the grid is stored in column strips 32 tiles wide. Strip s = x>>5 holds
 * tileHeight rows of 32 dwords, so tile (x,y) is at
 *     data + (((x>>5) << lgTileHeight) + y) * 32*4 + (x & 31) * 4
 * which is what the "* 0x80 + (x & 0x1f) * 4" expressions below compute.
 *
 * All offsets/addresses below match the game's own code: GetTile @ 0x476d00
 * (tile-word read + column-strip layout), the viewport tile pass near 0x4083xx (the
 * tileMappingIndex -> TileMapping -> TileSet -> pixel/palette resolution), and AllocSurface @
 * 0x4a9170 (the back-buffer is a plain BI_RGB 16bpp DIB, i.e. 555 R-high). */
static void RenderFullMap(void) {
    /* 1. Read the map fields and refuse when no map is loaded (main menu: zero/garbage sizes). */
    if (IsBadReadPtr((void*)MAP_TILEDATA, 4)) { Log("map: globals unreadable"); return; }
    int mapW  = *(int*)MAP_TILEWIDTH;
    int mapH  = *(int*)MAP_TILEHEIGHT;
    int xmask = *(int*)MAP_TILEXMASK;
    int lgH   = *(unsigned char*)MAP_LGTILEHEIGHT;
    int data  = *(int*)MAP_TILEDATA;
    int tsArr = *(int*)MAP_TILESETARR;
    int tmgr  = *(int*)MAP_TERRAINMGR;
    if (mapW < 16 || mapW > 8192 || mapH < 16 || mapH > 8192 || !data || !tsArr || !tmgr) {
        Log("map: not loaded or bad dims (w=%d h=%d data=%08X mgr=%08X) - load a mission first",
            mapW, mapH, data, tmgr);
        return;
    }
    int tmArr = *(int*)(tmgr + 4);                                 /* TileMapping[], 8 bytes each */
    if (!tmArr) { Log("map: no TileMapping array"); return; }

    /* 2. Optional diagnostic ([Screenshot] MapDiag=1): dump real runtime values for the centre tile -
     * tileset/graphic indices, the TileSet struct fields, raw tile bytes and the palette entry at
     * several light levels. This is how the light-level/format details above were pinned down; leave
     * it off in normal use. */
    if (g_mapDiag) {
        int cx = mapW / 2, cy = mapH / 2;
        unsigned int word = *(unsigned int*)(
            ((((cx >> 5) << lgH) + cy) * 0x80) + (cx & 0x1f) * 4 + data);
        int mapIdx = (word & 0xffe0) >> 5;
        short *tm = (short*)(tmArr + mapIdx * 8);
        int tsi = (unsigned short)tm[0], tgi = (unsigned short)tm[1];
        int ts = *(int*)(tsArr + tsi * 4);
        Log("DIAG center(%d,%d) word=%08X mapIdx=%d tsi=%d tgi=%d anim=%d/%d ts=%08X",
            cx, cy, word, mapIdx, tsi, tgi, (unsigned short)tm[2], (unsigned short)tm[3], ts);
        if (ts && !IsBadReadPtr((void*)ts, 0x100)) {
            int pitch = *(int*)(ts + 0x08), bdep = *(int*)(ts + 0x0C);
            int lvl1  = *(int*)(ts + 0x20), bpt = *(int*)(ts + 0xF4);
            int pix   = *(int*)(ts + 0xF8), palB = *(int*)(ts + 0x2C);
            Log("DIAG ts: pitch=%d bitDepth=%d levels+1=%d bytesPerTile=%d pixelData=%08X palBase=%08X",
                pitch, bdep, lvl1, bpt, pix, palB);
            if (pix && !IsBadReadPtr((void*)(pix + tgi*bpt), 16)) {
                unsigned char *tp = (unsigned char*)(pix + tgi * bpt);
                Log("DIAG tilebytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    tp[0],tp[1],tp[2],tp[3],tp[4],tp[5],tp[6],tp[7],tp[8],tp[9],tp[10],tp[11]);
                int sidx = tp[0];
                if (palB && !IsBadReadPtr((void*)palB, 33*0x200)) {
                    short *p = (short*)palB;
                    Log("DIAG pal[idx=%d] L0=%04X L8=%04X L16=%04X L24=%04X L32=%04X", sidx,
                        (unsigned short)p[0*256+sidx], (unsigned short)p[8*256+sidx],
                        (unsigned short)p[16*256+sidx], (unsigned short)p[24*256+sidx],
                        (unsigned short)p[32*256+sidx]);
                    Log("DIAG pal L32 [0..5]=%04X %04X %04X %04X %04X %04X",
                        (unsigned short)p[32*256+0],(unsigned short)p[32*256+1],(unsigned short)p[32*256+2],
                        (unsigned short)p[32*256+3],(unsigned short)p[32*256+4],(unsigned short)p[32*256+5]);
                }
            }
        }
    }

    /* 3. The playable area: the engine's map clip rect (inclusive tile coords). Maps narrower than 512
     * tiles are stored at double width with 32 unused columns on the left (and the rest on the right)
     * so they don't wrap; the clip rect covers only the real columns (x 32 .. width+31). 512-wide
     * maps wrap and have no x clip (-1 .. 0x7FFFFFFF). Clamped to the stored grid either way, so only
     * the playable map is rendered. */
    int x0 = *(int*)MAP_CLIPX1, y0 = *(int*)MAP_CLIPY1, x1 = *(int*)MAP_CLIPX2, y1 = *(int*)MAP_CLIPY2;
    if (x0 < 0) x0 = 0;  if (x1 > mapW - 1) x1 = mapW - 1;
    if (y0 < 0) y0 = 0;  if (y1 > mapH - 1) y1 = mapH - 1;
    if (x1 < x0 || y1 < y0) { x0 = 0; x1 = mapW - 1; y0 = 0; y1 = mapH - 1; }   /* nonsense: whole grid */
    int nW = x1 - x0 + 1, nH = y1 - y0 + 1;                        /* tiles rendered */

    /* 4. Output size. MapScale N draws each 32x32 tile as (32/N)x(32/N) pixels by taking every Nth
     *    source pixel (nearest neighbour, no blending). Rows are padded to 4 bytes (BMP rule). */
    int scale = g_mapScale; if (scale < 1) scale = 1; if (scale > 32) scale = 32;
    int outTile = 32 / scale; if (outTile < 1) outTile = 1;      /* output pixels per tile edge */
    int W = nW * outTile, H = nH * outTile;
    int stride = (W * 3 + 3) & ~3;                               /* bytes per BMP row */
    Log("map render: grid %dx%d tiles, playable x %d-%d y %d-%d (%dx%d), scale=%d -> %dx%d px (~%d MB BMP)",
        mapW, mapH, x0, x1, y0, y1, nW, nH, scale, W, H, (int)(((double)stride * H + 54) / 1048576.0));

    /* 5. Create the file and write the BMP headers (bottom-up 24bpp, sizes known up front). */
    char path[MAX_PATH];
    HANDLE f = CreateShotFile(path, "map");
    if (f == INVALID_HANDLE_VALUE) { Log("map: could not create a file in screenshots\\ (err %lu)", GetLastError()); return; }
    int ok = 1;                                                    /* cleared by the first failed write */
    BITMAPFILEHEADER fh; ZeroMemory(&fh, sizeof fh);
    BITMAPINFOHEADER ih; ZeroMemory(&ih, sizeof ih);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + (DWORD)stride * H;
    ih.biSize = sizeof ih; ih.biWidth = W; ih.biHeight = H; ih.biPlanes = 1;
    ih.biBitCount = 24; ih.biCompression = BI_RGB; ih.biSizeImage = (DWORD)stride * H;
    WriteAll(f, &fh, sizeof fh, &ok); WriteAll(f, &ih, sizeof ih, &ok);

    /* 6. Render. BMP rows go bottom-up, so walk tile rows from y1 down to y0, and within each tile
     *    row its pixel rows from the bottom one up. For each tile row, first resolve every tile to
     *    its pixel data, pitch and palette (rowPix/rowPit/rowPal), then emit outTile scanlines from
     *    those caches - one scanline buffer is all the image memory needed. */
    HANDLE heap = GetProcessHeap();
    unsigned char *scan = (unsigned char*)HeapAlloc(heap, 0, stride);   /* one output scanline */
    /* per-tile-row caches (resolve each tile once, then emit its pixel rows) */
    unsigned char **rowPix = (unsigned char**)HeapAlloc(heap, 0, nW * sizeof(void*));
    short        **rowPal = (short**)        HeapAlloc(heap, 0, nW * sizeof(void*));
    int           *rowPit = (int*)            HeapAlloc(heap, 0, nW * sizeof(int));
    if (scan && rowPix && rowPal && rowPit) {
        int tyTile;
        for (tyTile = y1; tyTile >= y0; tyTile--) {              /* BMP is bottom-up */
            int x;
            for (x = 0; x < nW; x++) {                            /* x = output column, map column x0+x */
                int mx = (x0 + x) & xmask;
                unsigned int word = *(unsigned int*)(
                    ((((mx >> 5) << lgH) + tyTile) * 0x80) + (mx & 0x1f) * 4 + data);
                int mapIdx = (word & 0xffe0) >> 5;                 /* bits 5-15: tileMappingIndex */
                short *tm = (short*)(tmArr + mapIdx * 8);         /* TileMapping{tileset, graphic, ...} */
                int tsi = (unsigned short)tm[0], tgi = (unsigned short)tm[1];
                int ts = *(int*)(tsArr + tsi * 4);                /* the TileSet, or 0 if not loaded */
                rowPix[x] = NULL; rowPal[x] = NULL; rowPit[x] = 0; /* missing data -> black tile */
                if (ts) {
                    /* light-level palettes run 0 (full daylight, true colours) -> higher = darker;
                     * the last level (numLightLevels) is a special "fully-lit" LUT that washes the
                     * colours out, so we default to level 0 for a natural full-bright map. */
                    int maxLvl = *(int*)(ts + 0x20) - 1; if (maxLvl < 0) maxLvl = 0;
                    int lvl = g_mapLight; if (lvl < 0) lvl = 0; if (lvl > maxLvl) lvl = maxLvl;
                    int palBase = *(int*)(ts + 0x2C);             /* light-level LUTs, 0x200 bytes each */
                    int bpt     = *(int*)(ts + 0xF4);             /* bytes per tile graphic */
                    int pix     = *(int*)(ts + 0xF8);             /* 8bpp pixels of all graphics */
                    rowPit[x] = *(int*)(ts + 0x08);               /* bytes per pixel row (32) */
                    if (pix)     rowPix[x] = (unsigned char*)(pix + tgi * bpt);
                    if (palBase) rowPal[x] = (short*)(palBase + lvl * 0x200);
                }
            }
            int py;
            for (py = outTile - 1; py >= 0; py--) {              /* pixel rows, bottom-up */
                int srcTy = py * scale;                           /* source row inside the tile */
                unsigned char *sp = scan;
                for (x = 0; x < nW; x++) {
                    unsigned char *tp = rowPix[x]; short *pal = rowPal[x]; int pit = rowPit[x];
                    int ox;
                    for (ox = 0; ox < outTile; ox++) {
                        int r = 0, g = 0, b = 0;
                        if (tp && pal) {
                            /* 8bpp index -> 16bpp 555 colour -> 8-bit channels; "x |= x >> 5"
                             * copies the top bits down so 0x1F becomes 0xFF (full brightness). */
                            int idx = tp[srcTy * pit + ox * scale];
                            unsigned short c = (unsigned short)pal[idx];
                            r = ((c >> 10) & 0x1f) << 3; r |= r >> 5;
                            g = ((c >> 5)  & 0x1f) << 3; g |= g >> 5;
                            b = ( c        & 0x1f) << 3; b |= b >> 5;
                        }
                        *sp++ = (unsigned char)b; *sp++ = (unsigned char)g; *sp++ = (unsigned char)r;  /* BMP order: B,G,R */
                    }
                }
                WriteAll(f, scan, stride, &ok);
            }
            if (!ok) break;                                    /* stop rendering after a failed write */
        }
    } else {
        Log("map render: out of memory");
        ok = 0;
    }
    if (scan)   HeapFree(heap, 0, scan);
    if (rowPix) HeapFree(heap, 0, rowPix);
    if (rowPal) HeapFree(heap, 0, rowPal);
    if (rowPit) HeapFree(heap, 0, rowPit);
    FinishShotFile(f, path, ok, "map render:");
}

/* Thread keyboard hook: viewport key arms a one-shot present capture; window key grabs the whole
 * window; map key renders the full map from memory. All fire on a fresh keydown only: lParam bit 31
 * is set on key-up and bit 30 when the key was already down (auto-repeat), so both must be clear.
 * Runs on the game's thread inside its message loop. The key is passed on to the game either way. */
static LRESULT CALLBACK KbProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && !(lp & 0xC0000000)) {
        if ((int)wp == g_key) {
            InterlockedExchange(&g_capture, 1);
            Log("hotkey 0x%02X pressed - viewport capture armed", g_key);
        } else if ((int)wp == g_winKey) {
            Log("hotkey 0x%02X pressed - capturing full window", g_winKey);
            CaptureWindow();
        } else if ((int)wp == g_mapKey) {
            Log("hotkey 0x%02X pressed - rendering full map from memory", g_mapKey);
            RenderFullMap();
        }
    }
    return CallNextHookEx(g_kbHook, code, wp, lp);
}

/* Patch one IAT slot of Outpost2.exe (match by import name, fall back to current address).
 * `*orig` receives what was in the slot (the real function, or another mod's hook that we then
 * chain to); `*slot` receives the slot's address so Uninit() can put `*orig` back.
 * Walks the exe's import descriptors to the one for `dll`, then its thunks: OriginalFirstThunk has
 * the import names, FirstThunk (the IAT) the resolved addresses the game calls through. Matching
 * by address as well catches a slot whose name table is missing. Returns 1 when a slot was patched. */
static int HookIAT(const char *dll, const char *fn, void *repl, void **orig, ULONG_PTR **slot) {
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
                    if (slot) *slot = &ft->u1.Function;
                    return 1;
                }
                return 0;
            }
        }
    }
    return 0;
}

/* Read the [Screenshot] settings from the live outpost2.ini (under OPU: OPU\outpost2.ini). */
static void ReadIni(void) {
    char path[MAX_PATH]; wsprintfA(path, "%s\\outpost2.ini", GameDir());
    g_key      = GetPrivateProfileIntA("Screenshot", "HotKey",    VK_F12, path);
    g_winKey   = GetPrivateProfileIntA("Screenshot", "WindowKey", VK_F11, path);
    g_mapKey   = GetPrivateProfileIntA("Screenshot", "MapKey",    VK_F10, path);
    g_mapScale = GetPrivateProfileIntA("Screenshot", "MapScale",  1,      path);
    g_mapLight = GetPrivateProfileIntA("Screenshot", "MapLight",  0,      path);
    g_mapDiag  = GetPrivateProfileIntA("Screenshot", "MapDiag",   0,      path);
    g_minW     = GetPrivateProfileIntA("Screenshot", "MinWidth",  400,    path);
    g_minH     = GetPrivateProfileIntA("Screenshot", "MinHeight", 240,    path);
    Log("HotKey(viewport)=0x%02X  WindowKey(full)=0x%02X  MapKey=0x%02X  MapScale=%d  MinViewport=%dx%d  (%s)",
        g_key, g_winKey, g_mapKey, g_mapScale, g_minW, g_minH, path);
}

static volatile LONG g_done = 0;
static ULONG_PTR *g_bitbltSlot = NULL;   /* the IAT slot we patched, for Uninit() */

/* Install both hooks, once. op2ext calls this from the game's main (UI) thread, so the keyboard
 * hook is a thread hook on THAT thread (GetCurrentThreadId): it sees the game's keys only, and
 * its handlers run on the thread that owns the window, the view surface and the map. */
static void Init(void) {
    if (InterlockedExchange(&g_done, 1) != 0) return;       /* op2ext may call more than one entry */
    Log("=== op2ext init (InitMod) - op2shot viewport screenshot ===");
    ReadIni();
    int b = HookIAT("gdi32.dll", "BitBlt", (void*)MyBitBlt, (void**)&g_realBitBlt, &g_bitbltSlot);
    Log("BitBlt hook = %s", b ? "OK" : "FAILED");
    g_kbHook = SetWindowsHookExA(WH_KEYBOARD, KbProc, g_hInst, GetCurrentThreadId());
    Log("keyboard hook = %s (thread %lu)", g_kbHook ? "OK" : "FAILED", GetCurrentThreadId());
    if (!b || !g_kbHook) Log("WARNING: a hook failed - screenshots will not work; report this log.");
    Log("loaded OK - F12 (viewport) / F11 (full window) / F10 (full map) -> %s\\screenshots\\", GameDir());
}

/* Undo Init(): remove the keyboard hook and put the original BitBlt back in the IAT slot, so
 * nothing points into this DLL once it is unloaded. Only restores the slot if it still holds our
 * hook - if another mod hooked BitBlt after us, it chains to us and we leave it alone.
 * Safe to call more than once. */
static void Uninit(void) {
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = NULL; }
    if (g_bitbltSlot && g_realBitBlt) {
        if (*g_bitbltSlot == (ULONG_PTR)MyBitBlt) {
            DWORD op;
            if (VirtualProtect(g_bitbltSlot, sizeof(void*), PAGE_READWRITE, &op)) {
                *g_bitbltSlot = (ULONG_PTR)g_realBitBlt;
                VirtualProtect(g_bitbltSlot, sizeof(void*), op, &op);
                Log("BitBlt hook removed");
            }
        } else {
            Log("BitBlt slot was re-hooked by another module - left in place");
        }
        g_bitbltSlot = NULL;
    }
}

/* ---- op2ext module entry points ----
 * The current op2ext calls InitMod / RunMod / DestroyMod, older ones mod_init / mod_run /
 * mod_destroy; all are exported (op2shot.def). Run has nothing to do: the hooks do the work. */
__declspec(dllexport) void InitMod(void)     { Init(); }
__declspec(dllexport) void mod_init(void)    { Init(); }
__declspec(dllexport) void mod_run(void)     { }
__declspec(dllexport) void mod_destroy(void) { Uninit(); }
__declspec(dllexport) void DestroyMod(void)  { Uninit(); }
__declspec(dllexport) void RunMod(void)      { }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = h; DisableThreadLibraryCalls(h);
        Log("=== process attach - Outpost2 loaded op2shot.dll ===");
    } else if (reason == DLL_PROCESS_DETACH) {
        Uninit();                                    /* in case the loader skipped DestroyMod */
        Log("=== process detach - op2shot.dll unloading ===");
    }
    return TRUE;
}
