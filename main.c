#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <dbt.h>

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define MAX_HISTORY 50
#define MAX_iSTACK_SIZE 10
#define MAX_MONITORS 64
#define SystemTimePointer ((_KSYSTEM_TIME*)0x7FFE0014)
#define IDI_APP_ICON 101
#define TOAST_DURATION_MS 1500

#define APP_NAME L"BackgroundHotkeyThing"
#define APP_NAME_A "BackgroundHotkeyThing"
#define APP_CLASS L"BgHotkeyMsgWindow"
#define INI_FILENAME L"BackgroundHotkeyThing.ini"
#define INI_SEC_SETTINGS L"Settings"
#define INI_SEC_FAVS L"Favs"
#define MUTEX_NAME L"Local\\BackgroundHotkeyThing_SingleInstance"
#define TIMER_MAIN 1
#define TIMER_TOAST 2
#define TIMER_HOLD 3
#define TIMER_DISPLAY 4
#define DISPLAY_DEBOUNCE_MS 400
#define DEFAULT_NSFW_INDEX 4000
#define MS_PER_MIN 60000
//hold next/prev: first extra step after this, then this often (wallpaper set isnt free)
#define HOLD_INITIAL_MS 800
#define HOLD_REPEAT_MS 400
#define HOLD_NONE 0
#define HOLD_NEXT 1
#define HOLD_PREV 2
//dont re-pick a wallpaper that showed up in the last N history entries (clamped if pool is tiny)
#define RECENT_EXCLUDE 10
//how deep to walk under the wallpaper root (0 = root only, 4 = root + 4 levels of subfolders)
#define SCAN_MAX_DEPTH 4
//subfolders whose name starts with this (case-insensitive) are not scanned
#define IGNORE_PREFIX L"ignore-"
#define IGNORE_PREFIX_CCH 7
//wide path buffer (beats classic ANSI MAX_PATH for most real libraries)
#define PATH_CCH 1024

//flip this on when you want to snoop via DebugView / a debugger
//#define DEBUG

#ifdef DEBUG
	#define printf(...) do { wchar_t _dbg[1024]; wsprintfW(_dbg, __VA_ARGS__); OutputDebugStringW(_dbg); } while(0)
#else
	#define printf(...) 
#endif

typedef enum {
	HK_QUIT = 1,
	HK_TOGGLE_ICONS,
	HK_SAVE_FAV,
	HK_NEXT_BG,
	HK_PREV_BG,
	HK_PAUSE,
	HK_TOGGLE_NSFW,
	HK_CYCLE_FAVS,
	HK_CLEAR_FAVS,
	HK_OPEN_EXPLORER,
	HK_TOGGLE_NOTIF,
	HK_CYCLE_ADVANCE
} HotkeyID;

#define ADVANCE_ALL 0
#define ADVANCE_ROUND_ROBIN 1
#define ADVANCE_RANDOM 2
#define ADVANCE_MODE_COUNT 3

typedef struct {
	int onlyFavs;
	int nsfw;
	int loop_pause;
	int notifications;
	int advanceMode; //ADVANCE_ALL / ROUND_ROBIN / RANDOM
} AppSettings;

//KUSER_SHARED_DATA system time fields (usermode mapping @ 0x7FFE0000)
typedef struct {
	ULONG LowPart;
	LONG High1Time;
	LONG High2Time;
} _KSYSTEM_TIME;

//favorites stack: upsert-to-top, FIFO-drop oldest when full
typedef struct intStack {
	__int64 top;
	__int64 pointer;
	int inds[MAX_iSTACK_SIZE];
} intStack;

//one slot per display adapter path. disconnected monitors keep history for replug.
typedef struct {
	wchar_t id[PATH_CCH];
	wchar_t origPath[PATH_CCH];
	RECT rect;
	int connected;
	int curbg; //index into bgs, or -1 if the screen is still on its launch wallpaper
	//prev[] = advances only. prevInd==-1 is "home" (that monitor's origPath)
	int prev[MAX_HISTORY];
	int prevInd;
} MonitorState;

typedef struct {
	wchar_t** bgs;
	int numBgs;
	//partition: [0]=launch snapshot, [1..nsfwIndex)=SFW, [nsfwIndex..numBgs)=NSFW
	int nsfwIndex;
	MonitorState mons[MAX_MONITORS];
	int rrSlot; //last monitor advanced in round-robin / random
	intStack* favs;
} AppState;

//GUID_DEVINTERFACE_MONITOR — display arrival/removal (ntddvdeo.h)
static const GUID GUID_MONITOR_INTERFACE =
	{0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};

AppState* g_state = NULL;
IDesktopWallpaper* g_desk = NULL;
static int g_comInited = 0;
static HDEVNOTIFY g_hDevNotify = NULL;
static HWND g_hwnd = NULL;
static wchar_t g_iniPath[PATH_CCH] = {0};

//growing list of wide paths (used while scanning, then folded into AppState.bgs)
typedef struct {
	wchar_t** paths;
	int count;
	int capacity;
} PathList;

AppSettings settings = {0};
int g_sfwCount = 0;
int g_nsfwCount = 0;

static unsigned long long g_ms_x = 0;
static unsigned long long g_ms_w = 0;
static unsigned long long g_ms_s = 0xb5ad4eceda1ce2a9ULL;

void* xmalloc(size_t n) {
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n);
}
void* xrealloc(void* p, size_t n) {
	if(!p) return xmalloc(n);
	return HeapReAlloc(GetProcessHeap(), 0, p, n);
}
void xfree(void* p) {
	if(p) HeapFree(GetProcessHeap(), 0, p);
}

int nextRand(void) {
	unsigned long long x = g_ms_x;
	x *= x;
	x += (g_ms_w += g_ms_s);
	x = (x >> 32) | (x << 32);
	g_ms_x = x;
	return (int)(x & 0x7fffffffULL);
}

void seedRng(unsigned int seed) {
	g_ms_x = 0;
	g_ms_w = 0;
	g_ms_s = 0xb5ad4eceda1ce2a9ULL ^ ((unsigned long long)seed * 0x9E3779B97F4A7C15ULL);
	g_ms_s |= 1ULL;
	for(int i = 0; i < 16; i++) nextRand();
}

int parsePositiveIntW(const wchar_t* s) {
	int n = 0;
	if(!s || !*s) return 0;
	for(; *s; s++) {
		if(*s < L'0' || *s > L'9') return 0;
		n = n * 10 + (int)(*s - L'0');
	}
	return n;
}

int wlen(const wchar_t* s) {
	int n = 0;
	if(!s) return 0;
	while(s[n]) n++;
	return n;
}

void wcpy(wchar_t* dst, const wchar_t* src, int cch) {
	if(!dst || cch < 1) return;
	if(!src) { dst[0] = 0; return; }
	int i = 0;
	for(; i < cch - 1 && src[i]; i++) dst[i] = src[i];
	dst[i] = 0;
}

//case-insensitive wide compare (Win32, no CRT)
int wicmp(const wchar_t* a, const wchar_t* b) {
	return CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a, -1, b, -1) - CSTR_EQUAL;
}

int wnicmp(const wchar_t* a, const wchar_t* b, int n) {
	return CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a, n, b, n) - CSTR_EQUAL;
}

int folderNameIsIgnored(const wchar_t* name) {
	if(!name || wlen(name) < IGNORE_PREFIX_CCH) return 0;
	return wnicmp(name, IGNORE_PREFIX, IGNORE_PREFIX_CCH) == 0;
}

int pathExistsAsFileW(const wchar_t* path) {
	if(!path || !path[0]) return 0;
	DWORD attr = GetFileAttributesW(path);
	if(attr == INVALID_FILE_ATTRIBUTES) return 0;
	if(attr & FILE_ATTRIBUTE_DIRECTORY) return 0;
	return 1;
}

wchar_t* lastPathSepW(wchar_t* s) {
	wchar_t* p = NULL;
	for(; *s; s++) {
		if(*s == L'\\' || *s == L'/') p = s;
	}
	return p;
}

const wchar_t* lastDotW(const wchar_t* s) {
	const wchar_t* p = NULL;
	for(; *s; s++) {
		if(*s == L'.') p = s;
	}
	return p;
}

//true if any path *segment* is "NSFW" (case-insensitive): \NSFW\, \NSFW, NSFW\...
int pathHasNsfwSegment(const wchar_t* path) {
	if(!path || !path[0]) return 0;
	const wchar_t* p = path;
	while(*p) {
		while(*p == L'\\' || *p == L'/') p++;
		if(!*p) break;
		const wchar_t* start = p;
		while(*p && *p != L'\\' && *p != L'/') p++;
		int len = (int)(p - start);
		if(len == 4 && wnicmp(start, L"NSFW", 4) == 0) return 1;
	}
	return 0;
}

int isAbsoluteWinPathW(const wchar_t* p) {
	if(!p || !p[0]) return 0;
	if(p[0] == L'\\' && p[1] == L'\\') return 1;
	if(p[0] == L'\\' || p[0] == L'/') return 1;
	if(((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':') {
		if(p[2] == 0 || p[2] == L'\\' || p[2] == L'/') return 1;
	}
	return 0;
}

//absolute/UNC stay rooted; relative hangs off the *exe* dir (not cwd)
int resolveBgPathW(const wchar_t* input, wchar_t* out, DWORD cchOut) {
	wchar_t combined[PATH_CCH] = {0};

	if(!input || !input[0] || !out || cchOut < 2) return 0;

	if(isAbsoluteWinPathW(input)) {
		DWORD n = GetFullPathNameW(input, cchOut, out, NULL);
		if(n == 0 || n >= cchOut) wcpy(out, input, (int)cchOut);
		return 1;
	}

	wchar_t exePath[PATH_CCH] = {0};
	if(!GetModuleFileNameW(NULL, exePath, PATH_CCH)) return 0;
	wchar_t* slash = lastPathSepW(exePath);
	if(slash) *slash = 0;
	else {
		wcpy(out, input, (int)cchOut);
		return 1;
	}

	wsprintfW(combined, L"%s\\%s", exePath, input);
	DWORD n = GetFullPathNameW(combined, cchOut, out, NULL);
	if(n == 0 || n >= cchOut) wcpy(out, combined, (int)cchOut);
	printf(L"Resolved relative path:\n  in : %s\n  out: %s\n", input, out);
	return 1;
}

int hasAllowedExtW(const wchar_t* fileName) {
	const wchar_t* dot = lastDotW(fileName);
	if(!dot || !dot[1]) return 0;
	if(wicmp(dot, L".png") == 0) return 1;
	if(wicmp(dot, L".jpg") == 0) return 1;
	if(wicmp(dot, L".jpeg") == 0) return 1;
	if(wicmp(dot, L".bmp") == 0) return 1;
	return 0;
}

int pathListInit(PathList* list, int cap) {
	list->count = 0;
	list->capacity = cap > 0 ? cap : 64;
	list->paths = xmalloc((size_t)list->capacity * sizeof(wchar_t*));
	return list->paths != NULL;
}

int pathListAdd(PathList* list, const wchar_t* path) {
	if(list->count >= list->capacity) {
		int nc = list->capacity * 2;
		wchar_t** np = xrealloc(list->paths, (size_t)nc * sizeof(wchar_t*));
		if(!np) return 0;
		list->paths = np;
		list->capacity = nc;
	}
	int n = wlen(path) + 1;
	list->paths[list->count] = xmalloc((size_t)n * sizeof(wchar_t));
	if(!list->paths[list->count]) return 0;
	wcpy(list->paths[list->count], path, n);
	list->count++;
	return 1;
}

void pathListFree(PathList* list) {
	if(!list || !list->paths) return;
	for(int i = 0; i < list->count; i++) xfree(list->paths[i]);
	xfree(list->paths);
	list->paths = NULL;
	list->count = 0;
	list->capacity = 0;
}

//recursive folder walk. depth 0 = wallpaper root. NSFW = any path segment "NSFW".
void scanDirW(const wchar_t* dir, int depth, PathList* sfw, PathList* nsfw) {
	wchar_t pattern[PATH_CCH];
	wchar_t full[PATH_CCH];
	WIN32_FIND_DATAW fd;
	HANDLE hFind;

	wsprintfW(pattern, L"%s\\*", dir);
	hFind = FindFirstFileW(pattern, &fd);
	if(hFind == INVALID_HANDLE_VALUE) return;

	do {
		if(wicmp(fd.cFileName, L".") == 0 || wicmp(fd.cFileName, L"..") == 0) continue;

		//join carefully
		int dlen = wlen(dir);
		int flen = wlen(fd.cFileName);
		if(dlen + 1 + flen + 1 > PATH_CCH) continue;
		wcpy(full, dir, PATH_CCH);
		full[dlen] = L'\\';
		wcpy(full + dlen + 1, fd.cFileName, PATH_CCH - dlen - 1);

		if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			//optional: skip reparse points so we dont wander into junctions forever
			if(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
			if(folderNameIsIgnored(fd.cFileName)) {
				printf(L"skip ignore- folder: %s\n", full);
				continue;
			}
			if(depth < SCAN_MAX_DEPTH) {
				scanDirW(full, depth + 1, sfw, nsfw);
			}
		} else if(hasAllowedExtW(fd.cFileName)) {
			if(pathHasNsfwSegment(full)) {
				pathListAdd(nsfw, full);
				printf(L"NSFW: %s\n", full);
			} else {
				pathListAdd(sfw, full);
				printf(L"SFW : %s\n", full);
			}
		}
	} while(FindNextFileW(hFind, &fd));

	FindClose(hFind);
}

//fills bgs[]: [0] reserved by caller, then all SFW, then all NSFW. sets *nsfwInd.
int ListDirectoryContentsW(const wchar_t* sDir, wchar_t*** bgs_ptr, int* capacity, int* nsfwInd) {
	PathList sfw = {0}, nsfw = {0};
	if(!pathListInit(&sfw, 256) || !pathListInit(&nsfw, 64)) {
		pathListFree(&sfw);
		pathListFree(&nsfw);
		return 1;
	}

	scanDirW(sDir, 0, &sfw, &nsfw);

	int need = 1 + sfw.count + nsfw.count;
	if(need > *capacity) {
		wchar_t** nb = xrealloc(*bgs_ptr, (size_t)need * sizeof(wchar_t*));
		if(!nb) {
			pathListFree(&sfw);
			pathListFree(&nsfw);
			return 1;
		}
		*bgs_ptr = nb;
		*capacity = need;
	}

	int ind = 1;
	for(int i = 0; i < sfw.count; i++) {
		(*bgs_ptr)[ind++] = sfw.paths[i];
		sfw.paths[i] = NULL; //ownership moved
	}
	*nsfwInd = ind;
	for(int i = 0; i < nsfw.count; i++) {
		(*bgs_ptr)[ind++] = nsfw.paths[i];
		nsfw.paths[i] = NULL;
	}

	//free list shells (paths stolen)
	xfree(sfw.paths);
	xfree(nsfw.paths);
	return ind;
}

int randRange(int lo, int hi) {
	if(hi < lo) return lo;
	int span = hi - lo + 1;
	if(span <= 1) return lo;
	unsigned int limit = (unsigned int)(0x80000000UL - (0x80000000UL % (unsigned int)span));
	unsigned int r;
	do {
		r = (unsigned int)nextRand();
	} while(r >= limit);
	return lo + (int)(r % (unsigned int)span);
}

int wasRecent(MonitorState* m, int bgInd, int window) {
	if(bgInd == m->curbg) return 1;
	if(window <= 0 || m->prevInd < 0) return 0;
	int start = m->prevInd - window + 1;
	if(start < 0) start = 0;
	for(int i = start; i <= m->prevInd; i++) {
		if(m->prev[i] == bgInd) return 1;
	}
	return 0;
}

int findBgByPath(AppState* state, const wchar_t* path) {
	if(!state || !path || !path[0] || !state->bgs) return -1;
	for(int i = 0; i < state->numBgs; i++) {
		if(state->bgs[i] && wicmp(state->bgs[i], path) == 0) return i;
	}
	return -1;
}

int connectedMonitorCount(AppState* state) {
	int n = 0;
	if(!state) return 0;
	for(int i = 0; i < MAX_MONITORS; i++) {
		if(state->mons[i].connected) n++;
	}
	return n;
}

int findMonById(AppState* state, const wchar_t* id) {
	if(!state || !id || !id[0]) return -1;
	for(int i = 0; i < MAX_MONITORS; i++) {
		if(state->mons[i].id[0] && wicmp(state->mons[i].id, id) == 0) return i;
	}
	return -1;
}

int allocMonSlot(AppState* state) {
	for(int i = 0; i < MAX_MONITORS; i++) {
		if(!state->mons[i].id[0]) return i;
	}
	for(int i = 0; i < MAX_MONITORS; i++) {
		if(!state->mons[i].connected) return i;
	}
	return -1;
}

int firstConnectedMonitor(AppState* state) {
	if(!state) return 0;
	for(int i = 0; i < MAX_MONITORS; i++) {
		if(state->mons[i].connected) return i;
	}
	return 0;
}

int cursorMonitorIndex(AppState* state) {
	int first = firstConnectedMonitor(state);
	if(!state) return 0;

	POINT pt;
	GetCursorPos(&pt);
	for(int i = 0; i < MAX_MONITORS; i++) {
		if(state->mons[i].connected && PtInRect(&state->mons[i].rect, pt)) return i;
	}

	HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi;
	mi.cbSize = sizeof(mi);
	if(hm && GetMonitorInfoW(hm, &mi)) {
		for(int i = 0; i < MAX_MONITORS; i++) {
			if(!state->mons[i].connected) continue;
			if(state->mons[i].rect.left == mi.rcMonitor.left &&
				state->mons[i].rect.top == mi.rcMonitor.top) return i;
		}
	}
	return first;
}

int imageInUse(AppState* state, int bgInd, int exceptMon) {
	if(!state || bgInd < 0) return 0;
	for(int i = 0; i < MAX_MONITORS; i++) {
		if(i == exceptMon) continue;
		if(state->mons[i].connected && state->mons[i].curbg == bgInd) return 1;
	}
	return 0;
}

void retireBg(AppState* state, int bgInd);
int ensureBg(AppState* state, int bgInd);
void UpdateTrayTip(HWND hwnd);
void saveFavs(const wchar_t* efavfpath, intStack* favs, wchar_t** bgs);

int pickFreshBg(AppState* state, int mon, int lo, int hi) {
	if(hi < lo) return -1;
	int span = hi - lo + 1;

	int window = RECENT_EXCLUDE;
	if(window >= span) window = span - 1;
	if(window < 0) window = 0;

	MonitorState* m = &state->mons[mon];

	if(span <= 1) {
		if(ensureBg(state, lo)) return lo;
		return -1;
	}

	for(int attempt = 0; attempt < 48; attempt++) {
		int c = randRange(lo, hi);
		if(!ensureBg(state, c)) continue;
		if(c == m->curbg) continue;
		if(wasRecent(m, c, window)) continue;
		if(imageInUse(state, c, mon)) continue;
		printf(L"pickFresh mon %d: %d (window=%d)\n", mon, c, window);
		return c;
	}

	int start = randRange(lo, hi);
	for(int n = 0; n < span; n++) {
		int c = lo + ((start - lo + n) % span);
		if(!ensureBg(state, c)) continue;
		if(c == m->curbg || wasRecent(m, c, window) || imageInUse(state, c, mon)) continue;
		printf(L"pickFresh scan mon %d: %d (window=%d)\n", mon, c, window);
		return c;
	}

	for(int n = 0; n < span; n++) {
		int c = lo + ((start - lo + n) % span);
		if(!ensureBg(state, c)) continue;
		if(c == m->curbg || wasRecent(m, c, window)) continue;
		printf(L"pickFresh reuse-across-monitors mon %d: %d\n", mon, c);
		return c;
	}

	for(int n = 0; n < span; n++) {
		int c = lo + ((start - lo + n) % span);
		if(ensureBg(state, c)) {
			printf(L"pickFresh: pool exhausted, allowing repeat %d\n", c);
			return c;
		}
	}

	printf(L"pickFresh: no usable files in range lo=%d hi=%d\n", lo, hi);
	return -1;
}

void applyPathOnMonitor(MonitorState* m, const wchar_t* path) {
	if(!m || !path || !path[0]) return;
	if(!pathExistsAsFileW(path)) {
		printf(L"apply skip missing %s\n", path);
		return;
	}

	if(g_desk && m->id[0]) {
		IDesktopWallpaper_SetPosition(g_desk, DWPOS_FILL);
		HRESULT hr = IDesktopWallpaper_SetWallpaper(g_desk, m->id, path);
		if(SUCCEEDED(hr)) return;
		printf(L"SetWallpaper failed hr=%08x for %s\n", (unsigned)hr, m->id);
	}

	SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path, SPIF_SENDCHANGE);
}

void applyBgIndex(AppState* state, int mon, int bgInd) {
	if(!state || mon < 0 || mon >= MAX_MONITORS) return;
	if(bgInd >= 1 && ensureBg(state, bgInd)) {
		applyPathOnMonitor(&state->mons[mon], state->bgs[bgInd]);
		return;
	}
	if(bgInd == 0 && state->bgs[0] && pathExistsAsFileW(state->bgs[0])) {
		applyPathOnMonitor(&state->mons[mon], state->bgs[0]);
	}
}

const wchar_t* monitorBgPath(AppState* state, int mon) {
	if(!state || mon < 0 || mon >= MAX_MONITORS) return NULL;
	int bg = state->mons[mon].curbg;
	if(bg >= 0 && bg < state->numBgs && state->bgs[bg]) return state->bgs[bg];
	if(state->mons[mon].origPath[0]) return state->mons[mon].origPath;
	if(state->numBgs > 0 && state->bgs[0]) return state->bgs[0];
	return NULL;
}

void reapplyMonitor(AppState* state, int slot) {
	if(!state || slot < 0 || slot >= MAX_MONITORS) return;
	const wchar_t* path = monitorBgPath(state, slot);
	if(path) applyPathOnMonitor(&state->mons[slot], path);
}

void initDummyMonitor(AppState* state) {
	MonitorState* m = &state->mons[0];
	if(!m->id[0] && !m->connected) {
		m->prevInd = -1;
		m->curbg = 0;
		if(state->bgs && state->bgs[0]) wcpy(m->origPath, state->bgs[0], PATH_CCH);
	}
	m->connected = 1;
	m->rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
	m->rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
	m->rect.right = m->rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
	m->rect.bottom = m->rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

void snapshotMonitorWallpaper(AppState* state, int slot, const wchar_t* monitorId) {
	MonitorState* m = &state->mons[slot];
	LPWSTR wp = NULL;
	if(g_desk && SUCCEEDED(IDesktopWallpaper_GetWallpaper(g_desk, monitorId, &wp)) && wp) {
		wcpy(m->origPath, wp, PATH_CCH);
		m->curbg = findBgByPath(state, wp);
		CoTaskMemFree(wp);
	} else if(state->bgs && state->bgs[0]) {
		wcpy(m->origPath, state->bgs[0], PATH_CCH);
		m->curbg = 0;
	}
}

int rectUsable(const RECT* r) {
	return r && (r->right > r->left) && (r->bottom > r->top);
}

typedef struct {
	RECT rects[MAX_MONITORS];
	int count;
} LiveRects;

BOOL CALLBACK enumLiveMonProc(HMONITOR hMon, HDC hdc, LPRECT lprcMonitor, LPARAM lp) {
	LiveRects* live = (LiveRects*)lp;
	(void)hdc;
	if(!live || live->count >= MAX_MONITORS) return FALSE;

	MONITORINFO mi;
	mi.cbSize = sizeof(mi);
	if(hMon && GetMonitorInfoW(hMon, &mi)) {
		live->rects[live->count++] = mi.rcMonitor;
	} else if(lprcMonitor && rectUsable(lprcMonitor)) {
		live->rects[live->count++] = *lprcMonitor;
	}
	return TRUE;
}

int rectInList(const RECT* r, const RECT* list, int n) {
	if(!r || !list) return 0;
	for(int i = 0; i < n; i++) {
		if(EqualRect(r, &list[i])) return 1;
	}
	return 0;
}

int slotWithRect(AppState* state, const RECT* r, int except) {
	if(!state || !r) return -1;
	for(int i = 0; i < MAX_MONITORS; i++) {
		if(i == except) continue;
		if(state->mons[i].connected && EqualRect(r, &state->mons[i].rect)) return i;
	}
	return -1;
}

void refreshMonitors(AppState* state) {
	if(!state) return;

	if(!g_desk) {
		initDummyMonitor(state);
		printf(L"refreshMonitors: no IDesktopWallpaper, single SPI fallback\n");
		return;
	}

	IDesktopWallpaper_SetPosition(g_desk, DWPOS_FILL);

	UINT count = 0;
	if(FAILED(IDesktopWallpaper_GetMonitorDevicePathCount(g_desk, &count))) {
		printf(L"GetMonitorDevicePathCount failed\n");
		initDummyMonitor(state);
		return;
	}

	LiveRects live = {0};
	EnumDisplayMonitors(NULL, NULL, enumLiveMonProc, (LPARAM)&live);
	printf(L"refreshMonitors: wallpaper paths=%u live=%d SM_CMONITORS=%d\n",
		count, live.count, GetSystemMetrics(SM_CMONITORS));

	int wasConnected[MAX_MONITORS];
	for(int i = 0; i < MAX_MONITORS; i++) {
		wasConnected[i] = state->mons[i].connected;
		state->mons[i].connected = 0;
	}

	if(count > MAX_MONITORS) count = MAX_MONITORS;

	for(UINT i = 0; i < count; i++) {
		LPWSTR id = NULL;
		if(FAILED(IDesktopWallpaper_GetMonitorDevicePathAt(g_desk, i, &id)) || !id) continue;

		RECT r = {0};
		HRESULT hrRect = IDesktopWallpaper_GetMonitorRECT(g_desk, id, &r);
		if(FAILED(hrRect) || !rectUsable(&r)) {
			printf(L"skip inactive wallpaper target %s hr=%08x\n", id, (unsigned)hrRect);
			CoTaskMemFree(id);
			continue;
		}
		if(live.count > 0 && !rectInList(&r, live.rects, live.count)) {
			printf(L"skip ghost wallpaper target %s rect={%d,%d,%d,%d}\n",
				id, r.left, r.top, r.right, r.bottom);
			CoTaskMemFree(id);
			continue;
		}
		if(slotWithRect(state, &r, -1) >= 0) {
			printf(L"skip duplicate wallpaper target %s (same rect)\n", id);
			CoTaskMemFree(id);
			continue;
		}

		int slot = findMonById(state, id);
		int isNew = 0;
		if(slot < 0) {
			slot = allocMonSlot(state);
			if(slot < 0) {
				printf(L"refreshMonitors: no free slot for %s\n", id);
				CoTaskMemFree(id);
				continue;
			}
			ZeroMemory(&state->mons[slot], sizeof(MonitorState));
			wcpy(state->mons[slot].id, id, PATH_CCH);
			state->mons[slot].prevInd = -1;
			state->mons[slot].curbg = -1;
			snapshotMonitorWallpaper(state, slot, id);
			isNew = 1;
			printf(L"monitor add slot %d: %s curbg=%d\n", slot, state->mons[slot].id, state->mons[slot].curbg);
		}

		state->mons[slot].connected = 1;
		state->mons[slot].rect = r;

		if(!isNew && !wasConnected[slot]) {
			printf(L"monitor replug slot %d: restoring wallpaper\n", slot);
			reapplyMonitor(state, slot);
		}

		CoTaskMemFree(id);
	}

	for(int i = 0; i < MAX_MONITORS; i++) {
		if(wasConnected[i] && !state->mons[i].connected) {
			printf(L"monitor unplug slot %d: %s (history kept)\n", i, state->mons[i].id);
		}
	}

	if(connectedMonitorCount(state) <= 0) {
		printf(L"refreshMonitors: no live wallpaper targets, dummy fallback\n");
		initDummyMonitor(state);
	}

	printf(L"refreshMonitors: %d connected\n", connectedMonitorCount(state));
}

void initDesktopWallpaper(void) {
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if(SUCCEEDED(hr)) {
		g_comInited = 1;
	} else if(hr != RPC_E_CHANGED_MODE) {
		printf(L"CoInitializeEx failed hr=%08x\n", (unsigned)hr);
		return;
	}

	hr = CoCreateInstance(&CLSID_DesktopWallpaper, NULL, CLSCTX_ALL,
		&IID_IDesktopWallpaper, (void**)&g_desk);
	if(FAILED(hr) || !g_desk) {
		printf(L"IDesktopWallpaper unavailable hr=%08x\n", (unsigned)hr);
		g_desk = NULL;
		return;
	}

	IDesktopWallpaper_Enable(g_desk, TRUE);
	IDesktopWallpaper_SetPosition(g_desk, DWPOS_FILL);
}

void shutdownDesktopWallpaper(void) {
	if(g_desk) {
		IDesktopWallpaper_Release(g_desk);
		g_desk = NULL;
	}
	if(g_comInited) {
		CoUninitialize();
		g_comInited = 0;
	}
}

void requestMonitorRefresh(HWND hwnd) {
	if(hwnd) SetTimer(hwnd, TIMER_DISPLAY, DISPLAY_DEBOUNCE_MS, NULL);
	else if(g_state) refreshMonitors(g_state);
}

HWND gethShellViewWin(void) {
	HWND prgMan = FindWindowW(L"Progman", L"Program Manager");
	HWND hShellViewWin = FindWindowExW(prgMan, 0, L"SHELLDLL_DefView", L"");

	if(hShellViewWin == 0x00) {
		HWND hWorkerW = 0x00;
		do {
			hWorkerW = FindWindowExW(0, hWorkerW, L"WorkerW", L"");
			hShellViewWin = FindWindowExW(hWorkerW, 0, L"SHELLDLL_DefView", L"");
		} while (hShellViewWin == 0x00 && hWorkerW != 0x00);
	}
	return hShellViewWin;
}

int findInIntStack(intStack* stack, int data) {
	for(int i = 0; i <= stack->top; i++) {
		if(stack->inds[i] == data) return i;
	}
	return -1;
}

void removeIntStackAt(intStack* stack, int idx) {
	if(idx < 0 || idx > stack->top) return;
	if(idx < stack->top) {
		MoveMemory(&stack->inds[idx], &stack->inds[idx + 1],
			sizeof(int) * (size_t)(stack->top - idx));
	}
	stack->top--;
	if(stack->top < -1) stack->top = -1;
	stack->pointer = stack->top;
}

int pushIntStack(intStack* stack, int data){
	stack->top++;
	if(stack->top >= MAX_iSTACK_SIZE){
		stack->top = MAX_iSTACK_SIZE - 1;
		MoveMemory(stack->inds,&stack->inds[1],(sizeof(int)*(MAX_iSTACK_SIZE-1)));
		stack->inds[stack->top] = data;
		stack->pointer = stack->top;
		return data;
	}
	stack->inds[stack->top] = data;
	stack->pointer = stack->top;
	return data;
}

int upsertIntStack(intStack* stack, int data) {
	int existing = findInIntStack(stack, data);
	if(existing >= 0) {
		removeIntStackAt(stack, existing);
		printf(L"fav upsert: moved existing slot %d to top\n", existing);
	}
	return pushIntStack(stack, data);
}

int peekIntStackItr(intStack* stack){
	if(stack->top <= -1) { stack->top = -1; return 0; }
	int data = stack->inds[stack->pointer];
	if(--stack->pointer < 0)
		stack->pointer = (stack->top < 0 ? 0 : stack->top);
	return data;
}

void printFavs(intStack* favs, wchar_t** bgs){
	printf(L"------Current Favorites------\n");
	for(int i = 0; i <= favs->top; i++){
		printf(L"fav[%d]:%d = %s\n", i, favs->inds[i], bgs[favs->inds[i]]);
	}
}

void saveFavs(const wchar_t* efavfpath, intStack* favs, wchar_t** bgs){
	WritePrivateProfileStringW(INI_SEC_FAVS, NULL, NULL, efavfpath);
	if(favs->top < 0) return;

	wchar_t slotName[16] = {0};
	for(int i = 0; i <= favs->top; i++){
		int idx = favs->inds[i];
		if(!bgs || idx < 0 || !bgs[idx] || !bgs[idx][0]) continue;
		wsprintfW(slotName, L"Fav-%d", i);
		WritePrivateProfileStringW(INI_SEC_FAVS, slotName, bgs[idx], efavfpath);
	}
}

void retireBg(AppState* state, int bgInd) {
	if(!state || bgInd < 1 || bgInd >= state->numBgs) return;
	if(!state->bgs[bgInd]) return;

	printf(L"retire missing bg [%d] %s\n", bgInd, state->bgs[bgInd]);

	int nsfw = (bgInd >= state->nsfwIndex);
	xfree(state->bgs[bgInd]);
	state->bgs[bgInd] = NULL;

	if(nsfw) {
		if(g_nsfwCount > 0) g_nsfwCount--;
	} else {
		if(g_sfwCount > 0) g_sfwCount--;
	}

	if(state->favs) {
		int favChanged = 0;
		int f;
		while((f = findInIntStack(state->favs, bgInd)) >= 0) {
			removeIntStackAt(state->favs, f);
			favChanged = 1;
		}
		if(favChanged && g_iniPath[0])
			saveFavs(g_iniPath, state->favs, state->bgs);
	}

	if(g_hwnd) UpdateTrayTip(g_hwnd);
}

int ensureBg(AppState* state, int bgInd) {
	if(!state || bgInd < 1 || bgInd >= state->numBgs) return 0;
	if(!state->bgs[bgInd] || !state->bgs[bgInd][0]) return 0;
	if(pathExistsAsFileW(state->bgs[bgInd])) return 1;
	retireBg(state, bgInd);
	return 0;
}

void saveSettings(const wchar_t* efavfpath, AppSettings* s) {
	wchar_t ival[8] = {0};
	wsprintfW(ival, L"%d", s->onlyFavs);
	WritePrivateProfileStringW(INI_SEC_SETTINGS, L"Set-0", ival, efavfpath);
	wsprintfW(ival, L"%d", s->nsfw);
	WritePrivateProfileStringW(INI_SEC_SETTINGS, L"Set-1", ival, efavfpath);
	wsprintfW(ival, L"%d", s->loop_pause);
	WritePrivateProfileStringW(INI_SEC_SETTINGS, L"Set-2", ival, efavfpath);
	wsprintfW(ival, L"%d", s->notifications);
	WritePrivateProfileStringW(INI_SEC_SETTINGS, L"Set-3", ival, efavfpath);
	wsprintfW(ival, L"%d", s->advanceMode);
	WritePrivateProfileStringW(INI_SEC_SETTINGS, L"Set-4", ival, efavfpath);
}

void loadSettings(const wchar_t* efavfpath, AppSettings* s) {
	s->onlyFavs = GetPrivateProfileIntW(INI_SEC_SETTINGS, L"Set-0", s->onlyFavs, efavfpath);
	s->nsfw = GetPrivateProfileIntW(INI_SEC_SETTINGS, L"Set-1", s->nsfw, efavfpath);
	s->loop_pause = GetPrivateProfileIntW(INI_SEC_SETTINGS, L"Set-2", s->loop_pause, efavfpath);
	s->notifications = GetPrivateProfileIntW(INI_SEC_SETTINGS, L"Set-3", s->notifications, efavfpath);
	s->advanceMode = GetPrivateProfileIntW(INI_SEC_SETTINGS, L"Set-4", s->advanceMode, efavfpath);
	if(s->advanceMode < 0 || s->advanceMode >= ADVANCE_MODE_COUNT) s->advanceMode = ADVANCE_ALL;
}

void importFavs(const wchar_t* efavfpath, intStack* favs, wchar_t** bgs, int numBgs){
	wchar_t favPath[PATH_CCH] = {0};
	wchar_t slotName[16] = {0};
	for(int i = 0; i < MAX_iSTACK_SIZE; i++){
		wsprintfW(slotName, L"Fav-%d", i);
		if(!GetPrivateProfileStringW(INI_SEC_FAVS, slotName, L"", favPath, PATH_CCH, efavfpath))
			break;
		for(int o = 0; o < numBgs; o++){
			if(wicmp(bgs[o], favPath) == 0){
				pushIntStack(favs, o);
				break;
			}
		}
	}
}

void historyPushMon(MonitorState* m, int bgInd) {
	if(m->prevInd < 0) {
		m->prevInd = 0;
		m->prev[0] = bgInd;
	} else if(m->prevInd < MAX_HISTORY - 1) {
		m->prev[++m->prevInd] = bgInd;
	} else {
		MoveMemory(&m->prev[0], &m->prev[1], sizeof(int) * (MAX_HISTORY - 1));
		m->prev[MAX_HISTORY - 1] = bgInd;
		m->prevInd = MAX_HISTORY - 1;
	}
	m->curbg = bgInd;
}

int nextFav(AppState* state, int mon) {
	if(state->favs->top < 0) return -1;

	int attempts = (int)state->favs->top + 1;
	int fallback = -1;
	for(int n = 0; n < attempts; n++) {
		int favsp = (int)state->favs->pointer;
		int favSlot = peekIntStackItr(state->favs);

		if(favSlot < 1 || favSlot >= state->numBgs || !ensureBg(state, favSlot)) {
			printf(L"skip missing fav[%d]=%d\n", favsp, favSlot);
			continue;
		}
		if(favSlot >= state->nsfwIndex && settings.nsfw == 0) {
			printf(L"skip NSFW fav[%d]=%d (nsfw mode off)\n", favsp, favSlot);
			continue;
		}
		if(favSlot == state->mons[mon].curbg) continue;
		if(imageInUse(state, favSlot, mon)) {
			if(fallback < 0) fallback = favSlot;
			continue;
		}
		printf(L"load fav[%d] = %d mon %d - bg: %s\n", favsp, favSlot, mon, state->bgs[favSlot]);
		applyBgIndex(state, mon, favSlot);
		return favSlot;
	}

	if(fallback >= 0 && ensureBg(state, fallback)) {
		printf(L"load fav fallback = %d mon %d\n", fallback, mon);
		applyBgIndex(state, mon, fallback);
		return fallback;
	}

	printf(L"No loadable favorites for current NSFW mode\n");
	return -1;
}

int initBGs(const wchar_t* relpath, AppState* appState, wchar_t* orgPaper, int orgCch){
	SystemParametersInfoW(SPI_GETDESKWALLPAPER, (UINT)orgCch, orgPaper, 0);

	int capacity = 1000;
	appState->bgs = xmalloc((size_t)capacity * sizeof(wchar_t*));
	if (!appState->bgs) return 0;

	int ogLen = wlen(orgPaper) + 1;
	appState->bgs[0] = xmalloc((size_t)ogLen * sizeof(wchar_t));
	wcpy(appState->bgs[0], orgPaper, ogLen);

	int numBgs = ListDirectoryContentsW(relpath, &(appState->bgs), &capacity, &(appState->nsfwIndex));

	printf(L"%d Backgrounds Loaded.\nNSFW Begins at:%d\n", numBgs, appState->nsfwIndex);

	if(numBgs <= 1) {
		wchar_t errmsg[PATH_CCH + 64];
		wsprintfW(errmsg, L"Path or Images not found at:\n[%s]\n\n(png/jpg/jpeg/bmp, nested up to depth %d)",
			relpath, SCAN_MAX_DEPTH);
		MessageBoxW(0, errmsg, L"Whoops!", MB_OK | MB_ICONWARNING);
		return 0;
	}

	return numBgs;
}

#define WM_TRAYICON (WM_APP + 1)
#define TRAY_ICON_ID 1

void AdvanceFavorite(AppState* state, int mon) {
	int nextFfavs = nextFav(state, mon);
	if(nextFfavs != -1){
		historyPushMon(&state->mons[mon], nextFfavs);
	}
}

void AdvanceBackground(AppState* state, int mon) {
	if(mon < 0 || mon >= MAX_MONITORS) return;
	if(!state->mons[mon].connected) return;

	if(settings.onlyFavs){
		AdvanceFavorite(state, mon);
		return;
	}

	int lo = 1;
	int hi = state->numBgs - 1;

	if(!settings.nsfw) {
		lo = 1;
		hi = state->nsfwIndex - 1;
	} else if(settings.nsfw == 2) {
		lo = state->nsfwIndex;
		hi = state->numBgs - 1;
	} else {
		lo = 1;
		hi = state->numBgs - 1;
	}

	if(hi < lo) {
		printf(L"No backgrounds available for current NSFW mode (lo=%d hi=%d)\n", lo, hi);
		return;
	}

	int nextBg = pickFreshBg(state, mon, lo, hi);
	if(nextBg < 0 || !ensureBg(state, nextBg)) {
		printf(L"No usable backgrounds for current NSFW mode (lo=%d hi=%d)\n", lo, hi);
		return;
	}
	historyPushMon(&state->mons[mon], nextBg);
	printf(L"Setting mon %d:[%d]%s\n", mon, nextBg, state->bgs[nextBg]);
	applyBgIndex(state, mon, nextBg);
}

void AdvanceAllMonitors(AppState* state) {
	for(int i = 0; i < MAX_MONITORS; i++) {
		if(state->mons[i].connected) AdvanceBackground(state, i);
	}
}

int nextRoundRobinMonitor(AppState* state) {
	int start = state->rrSlot;
	for(int n = 1; n <= MAX_MONITORS; n++) {
		int i = start + n;
		if(i >= MAX_MONITORS) i -= MAX_MONITORS;
		if(i < 0) i = 0;
		if(state->mons[i].connected) {
			state->rrSlot = i;
			return i;
		}
	}
	return firstConnectedMonitor(state);
}

int randomConnectedMonitor(AppState* state) {
	int ids[MAX_MONITORS];
	int n = 0;
	for(int i = 0; i < MAX_MONITORS; i++) {
		if(state->mons[i].connected) ids[n++] = i;
	}
	if(n <= 0) return 0;
	if(n == 1) return ids[0];

	int pick = ids[randRange(0, n - 1)];
	if(pick == state->rrSlot) pick = ids[randRange(0, n - 1)];
	state->rrSlot = pick;
	return pick;
}

const wchar_t* advanceModeName(int mode) {
	if(mode == ADVANCE_ROUND_ROBIN) return L"Round robin";
	if(mode == ADVANCE_RANDOM) return L"Random";
	return L"All monitors";
}

void AdvanceNextByMode(AppState* state) {
	int n = connectedMonitorCount(state);
	if(n <= 0) return;

	if(settings.advanceMode == ADVANCE_ALL || n == 1) {
		printf(L"advance mode All (%d monitors)\n", n);
		AdvanceAllMonitors(state);
		return;
	}
	if(settings.advanceMode == ADVANCE_ROUND_ROBIN) {
		int mon = nextRoundRobinMonitor(state);
		printf(L"advance mode RR -> mon %d\n", mon);
		AdvanceBackground(state, mon);
		return;
	}
	{
		int mon = randomConnectedMonitor(state);
		printf(L"advance mode Random -> mon %d\n", mon);
		AdvanceBackground(state, mon);
	}
}

void PreviousBackground(AppState* state, int mon) {
	if(mon < 0 || mon >= MAX_MONITORS) return;
	MonitorState* m = &state->mons[mon];
	if(!m->connected || m->prevInd < 0) return;

	while(m->prevInd >= 0) {
		if(m->prevInd == 0) {
			m->prevInd = -1;
			m->curbg = findBgByPath(state, m->origPath);
			if(m->origPath[0] && pathExistsAsFileW(m->origPath)) {
				printf(L"Setting mon %d: home %s\n", mon, m->origPath);
				applyPathOnMonitor(m, m->origPath);
			} else {
				printf(L"Setting mon %d: home missing, not applied\n", mon);
			}
			return;
		}

		m->prevInd--;
		int bg = m->prev[m->prevInd];
		if(!ensureBg(state, bg)) {
			printf(L"prev skip missing [%d] mon %d\n", bg, mon);
			continue;
		}
		m->curbg = bg;
		printf(L"Setting mon %d:[%d]%s\n", mon, bg, state->bgs[bg]);
		applyPathOnMonitor(&state->mons[mon], state->bgs[bg]);
		return;
	}
}

int keyDown(int vk) {
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

int winKeyDown(void) {
	return keyDown(VK_LWIN) || keyDown(VK_RWIN);
}

int isNextHeld(void) {
	return winKeyDown() && keyDown(VK_SHIFT) && keyDown('N');
}
int isPrevHeld(void) {
	return winKeyDown() && keyDown(VK_SHIFT) && keyDown('B');
}

void stopHoldCycle(UINT_PTR* holdTimer, int* holdDir, int* holdMon) {
	if(holdTimer && *holdTimer) {
		KillTimer(NULL, *holdTimer);
		*holdTimer = 0;
	}
	if(holdDir) *holdDir = HOLD_NONE;
	if(holdMon) *holdMon = -1;
}

void startHoldCycle(int dir, int mon, UINT_PTR* holdTimer, int* holdDir, int* holdMon) {
	*holdDir = dir;
	*holdMon = mon;
	if(*holdTimer) KillTimer(NULL, *holdTimer);
	*holdTimer = SetTimer(NULL, TIMER_HOLD, HOLD_INITIAL_MS, NULL);
}

void UpdateTrayTip(HWND hwnd) {
	NOTIFYICONDATAW nid = {0};
	nid.cbSize = sizeof(NOTIFYICONDATAW);
	nid.hWnd = hwnd;
	nid.uID = TRAY_ICON_ID;
	nid.uFlags = NIF_TIP;
	wsprintfW(nid.szTip, L"%s\nSFW: %d | NSFW: %d | Monitors: %d",
		APP_NAME, g_sfwCount, g_nsfwCount, connectedMonitorCount(g_state));
	Shell_NotifyIconW(NIM_MODIFY, &nid);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch(msg) {
		case WM_TRAYICON:
			if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
				POINT pt;
				GetCursorPos(&pt);
				HMENU hMenu = CreatePopupMenu();

				wchar_t infoLine[96];
				wsprintfW(infoLine, L"Loaded  SFW: %d  |  NSFW: %d  |  Monitors: %d",
					g_sfwCount, g_nsfwCount, connectedMonitorCount(g_state));
				AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, infoLine);
				AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

				AppendMenuW(hMenu, MF_STRING, HK_NEXT_BG, L"Next Background\tWin+Shift-N");
				AppendMenuW(hMenu, MF_STRING, HK_PREV_BG, L"Previous Background\tWin+Shift-B");
				AppendMenuW(hMenu, MF_STRING | (settings.loop_pause ? MF_CHECKED : MF_UNCHECKED), HK_PAUSE, L"Toggle Pause\tWin+Alt-P");

				wchar_t advMenuText[64];
				if (settings.advanceMode == ADVANCE_ALL) {
					wcpy(advMenuText, L"Auto-rotate: All monitors\tWin+Alt-U", 64);
				} else if (settings.advanceMode == ADVANCE_ROUND_ROBIN) {
					wcpy(advMenuText, L"[-] Auto-rotate: Round robin\tWin+Alt-U", 64);
				} else {
					wcpy(advMenuText, L"Auto-rotate: Random\tWin+Alt-U", 64);
				}
				AppendMenuW(hMenu, MF_STRING | (settings.advanceMode ? MF_CHECKED : MF_UNCHECKED), HK_CYCLE_ADVANCE, advMenuText);

				wchar_t nsfwMenuText[64];
				UINT nsfwState = MF_UNCHECKED;
				if (settings.nsfw == 0) {
					wcpy(nsfwMenuText, L"NSFW Mode: Off\tWin+Shift-X", 64);
				} else if (settings.nsfw == 1) {
					wcpy(nsfwMenuText, L"[-] NSFW Mode: Combined\tWin+Shift-X", 64);
				} else {
					wcpy(nsfwMenuText, L"NSFW Mode: Only NSFW\tWin+Shift-X", 64);
					nsfwState = MF_CHECKED;
				}
				AppendMenuW(hMenu, MF_STRING | nsfwState, HK_TOGGLE_NSFW, nsfwMenuText);

				AppendMenuW(hMenu, MF_STRING | (settings.onlyFavs ? MF_CHECKED : MF_UNCHECKED), HK_CYCLE_FAVS, L"Toggle Cycle Favs\tWin+Shift-L");
				AppendMenuW(hMenu, MF_STRING | (settings.notifications ? MF_CHECKED : MF_UNCHECKED), HK_TOGGLE_NOTIF, L"Toggle Notifications\tWin+Alt-N");
				AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
				AppendMenuW(hMenu, MF_STRING, HK_SAVE_FAV, L"Save Favorite\tWin+Shift-A");
				AppendMenuW(hMenu, MF_STRING, HK_CLEAR_FAVS, L"Clear Favorites\tWin+Shift-C");
				AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
				AppendMenuW(hMenu, MF_STRING, HK_TOGGLE_ICONS, L"Toggle Desktop Icons\tWin+Shift-Z");
				AppendMenuW(hMenu, MF_STRING, HK_OPEN_EXPLORER, L"Open in Explorer\tWin+Shift-O");
				AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
				AppendMenuW(hMenu, MF_STRING, HK_QUIT, L"Quit\tWin+Alt-Q");

				SetForegroundWindow(hwnd);
				int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
				PostMessage(hwnd, WM_NULL, 0, 0);
				DestroyMenu(hMenu);

				if (cmd != 0) {
					if (cmd == HK_QUIT) {
						PostQuitMessage(0);
					} else {
						PostMessage(hwnd, WM_HOTKEY, cmd, 0);
					}
				}
			}
			break;
		case WM_DESTROY:
			PostQuitMessage(0);
			break;
		case WM_DISPLAYCHANGE:
			requestMonitorRefresh(hwnd);
			break;
		case WM_DEVICECHANGE:
			if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE ||
				wParam == DBT_DEVNODES_CHANGED) {
				requestMonitorRefresh(hwnd);
			}
			break;
		case WM_TIMER:
			if (wParam == TIMER_TOAST) {
				KillTimer(hwnd, TIMER_TOAST);
				NOTIFYICONDATAW nid = {0};
				nid.cbSize = sizeof(NOTIFYICONDATAW);
				nid.hWnd = hwnd;
				nid.uID = TRAY_ICON_ID;
				nid.uFlags = NIF_INFO;
				nid.szInfo[0] = 0;
				Shell_NotifyIconW(NIM_MODIFY, &nid);
			} else if (wParam == TIMER_DISPLAY) {
				KillTimer(hwnd, TIMER_DISPLAY);
				refreshMonitors(g_state);
				UpdateTrayTip(hwnd);
			}
			break;
		default:
			return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
	return 0;
}

void InitTrayIcon(HWND hwnd) {
	NOTIFYICONDATAW nid = {0};
	nid.cbSize = sizeof(NOTIFYICONDATAW);
	nid.hWnd = hwnd;
	nid.uID = TRAY_ICON_ID;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_TRAYICON;
	nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));
	wcpy(nid.szTip, APP_NAME, 128);
	Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon(HWND hwnd) {
	NOTIFYICONDATAW nid = {0};
	nid.cbSize = sizeof(NOTIFYICONDATAW);
	nid.hWnd = hwnd;
	nid.uID = TRAY_ICON_ID;
	Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowNotification(HWND hwnd, const wchar_t* title, const wchar_t* message, int timeoutMs) {
	if (!settings.notifications) return;

	NOTIFYICONDATAW nid = {0};
	nid.cbSize = sizeof(NOTIFYICONDATAW);
	nid.hWnd = hwnd;
	nid.uID = TRAY_ICON_ID;
	nid.uFlags = NIF_INFO;
	wcpy(nid.szInfoTitle, title, 64);
	wcpy(nid.szInfo, message, 256);
	nid.dwInfoFlags = NIIF_NOSOUND;
	nid.uTimeout = timeoutMs;
	Shell_NotifyIconW(NIM_MODIFY, &nid);

	if (timeoutMs > 0) {
		SetTimer(hwnd, TIMER_TOAST, timeoutMs, NULL);
	} else {
		KillTimer(hwnd, TIMER_TOAST);
	}
}

void bumpAutoTimer(UINT_PTR* timerId, UINT timerInterval) {
	if (!settings.loop_pause) {
		if (*timerId) KillTimer(NULL, *timerId);
		*timerId = SetTimer(NULL, TIMER_MAIN, timerInterval, NULL);
	}
}

void HandleHotkey(int hotkeyId, AppState* appState, HWND hwnd, const wchar_t* efavfpath,
	UINT_PTR* timerId, UINT timerInterval, int* running,
	UINT_PTR* holdTimer, int* holdDir, int* holdMon) {
	int mon = cursorMonitorIndex(appState);
	printf(L"hotkey %d cursor-mon %d\n", hotkeyId, mon);

	switch (hotkeyId) {
		case HK_QUIT:
			stopHoldCycle(holdTimer, holdDir, holdMon);
			*running = 0;
			break;
		case HK_TOGGLE_ICONS:
			{
				HWND hShellViewWin = gethShellViewWin();
				if(hShellViewWin) SendMessage(hShellViewWin, 0x0111, 0x7402, 0);
			}
			break;
		case HK_SAVE_FAV:
			{
				int bg = appState->mons[mon].curbg;
				if(bg < 1 || bg >= appState->numBgs || !ensureBg(appState, bg)) {
					ShowNotification(hwnd, L"Favorites", L"Current wallpaper is not in the library.", TOAST_DURATION_MS);
					break;
				}
				upsertIntStack(appState->favs, bg);
				saveFavs(efavfpath, appState->favs, appState->bgs);
				printFavs(appState->favs, appState->bgs);
				ShowNotification(hwnd, L"Favorites", L"Saved current background to favorites!", TOAST_DURATION_MS);
			}
			break;
		case HK_NEXT_BG:
			bumpAutoTimer(timerId, timerInterval);
			AdvanceBackground(appState, mon);
			startHoldCycle(HOLD_NEXT, mon, holdTimer, holdDir, holdMon);
			break;
		case HK_CYCLE_ADVANCE:
			settings.advanceMode = (settings.advanceMode + 1) % ADVANCE_MODE_COUNT;
			printf(L"AdvanceMode:%d\n", settings.advanceMode);
			ShowNotification(hwnd, L"Auto-rotate", advanceModeName(settings.advanceMode), TOAST_DURATION_MS);
			saveSettings(efavfpath, &settings);
			break;
		case HK_PREV_BG:
			bumpAutoTimer(timerId, timerInterval);
			if(appState->mons[mon].prevInd >= 0) {
				PreviousBackground(appState, mon);
			}
			startHoldCycle(HOLD_PREV, mon, holdTimer, holdDir, holdMon);
			break;
		case HK_PAUSE:
			settings.loop_pause ^= 1;
			if (settings.loop_pause) {
				if (*timerId) KillTimer(NULL, *timerId);
				*timerId = 0;
				ShowNotification(hwnd, APP_NAME, L"Auto Rotate: Paused", TOAST_DURATION_MS);
			} else {
				if (*timerId) KillTimer(NULL, *timerId);
				*timerId = SetTimer(NULL, TIMER_MAIN, timerInterval, NULL);
				ShowNotification(hwnd, APP_NAME, L"Auto Rotate: Resumed", TOAST_DURATION_MS);
			}
			saveSettings(efavfpath, &settings);
			break;
		case HK_TOGGLE_NSFW:
			settings.nsfw = ++settings.nsfw % 3;
			printf(L"NSFW:%d\n", settings.nsfw);
			if (settings.nsfw == 0) ShowNotification(hwnd, L"NSFW Mode", L"Off", TOAST_DURATION_MS);
			else if (settings.nsfw == 1) ShowNotification(hwnd, L"NSFW Mode", L"Combined", TOAST_DURATION_MS);
			else ShowNotification(hwnd, L"NSFW Mode", L"Only NSFW", TOAST_DURATION_MS);
			saveSettings(efavfpath, &settings);
			break;
		case HK_CYCLE_FAVS:
			settings.onlyFavs ^= 1;
			printf(L"Cycle:%s\n", (settings.onlyFavs ? L"Only Favorites" : L"Normal"));
			ShowNotification(hwnd, L"Cycle Mode", settings.onlyFavs ? L"Only Favorites" : L"Normal", TOAST_DURATION_MS);
			saveSettings(efavfpath, &settings);
			break;
		case HK_CLEAR_FAVS:
			printf(L"Clearing Favorites\n");
			appState->favs->top = -1;
			appState->favs->pointer = 0;
			ZeroMemory(appState->favs->inds, sizeof(int) * MAX_iSTACK_SIZE);
			saveFavs(efavfpath, appState->favs, appState->bgs);
			ShowNotification(hwnd, L"Favorites", L"Cleared all favorites!", TOAST_DURATION_MS);
			break;
		case HK_OPEN_EXPLORER:
			{
				const wchar_t* path = monitorBgPath(appState, mon);
				if(path && path[0]) {
					wchar_t args[PATH_CCH + 32] = {0};
					wsprintfW(args, L"/select,\"%s\"", path);
					ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
				}
			}
			break;
		case HK_TOGGLE_NOTIF:
			settings.notifications ^= 1;
			saveSettings(efavfpath, &settings);
			if (settings.notifications) {
				ShowNotification(hwnd, APP_NAME, L"Notifications Enabled", TOAST_DURATION_MS);
			}
			break;
	}
}

void RegisterAppHotkeys(void) {
	wchar_t hkErrors[1024] = {0};

	if(!RegisterHotKey(NULL, HK_QUIT, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'Q')) lstrcatW(hkErrors, L"- Win+Alt-Q (Quit)\n");
	if(!RegisterHotKey(NULL, HK_TOGGLE_ICONS, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'Z')) lstrcatW(hkErrors, L"- Win+Shift-Z (Toggle Icons)\n");
	if(!RegisterHotKey(NULL, HK_SAVE_FAV, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'A')) lstrcatW(hkErrors, L"- Win+Shift-A (Save Fav)\n");
	if(!RegisterHotKey(NULL, HK_NEXT_BG, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'N')) lstrcatW(hkErrors, L"- Win+Shift-N (Next BG)\n");
	if(!RegisterHotKey(NULL, HK_PREV_BG, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'B')) lstrcatW(hkErrors, L"- Win+Shift-B (Prev BG)\n");
	if(!RegisterHotKey(NULL, HK_PAUSE, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'P')) lstrcatW(hkErrors, L"- Win+Alt-P (Pause)\n");
	if(!RegisterHotKey(NULL, HK_TOGGLE_NSFW, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'X')) lstrcatW(hkErrors, L"- Win+Shift-X (Toggle NSFW)\n");
	if(!RegisterHotKey(NULL, HK_CYCLE_FAVS, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'L')) lstrcatW(hkErrors, L"- Win+Shift-L (Cycle Favs)\n");
	if(!RegisterHotKey(NULL, HK_CLEAR_FAVS, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'C')) lstrcatW(hkErrors, L"- Win+Shift-C (Clear Favs)\n");
	if(!RegisterHotKey(NULL, HK_OPEN_EXPLORER, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'O')) lstrcatW(hkErrors, L"- Win+Shift-O (Open Explorer)\n");
	if(!RegisterHotKey(NULL, HK_TOGGLE_NOTIF, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'N')) lstrcatW(hkErrors, L"- Win+Alt-N (Toggle Notifications)\n");
	if(!RegisterHotKey(NULL, HK_CYCLE_ADVANCE, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'U')) lstrcatW(hkErrors, L"- Win+Alt-U (Auto-rotate mode)\n");

	if (hkErrors[0] != 0) {
		wchar_t errorMsg[2048] = {0};
		wsprintfW(errorMsg, L"The following hotkeys failed to register (they might be in use by Windows or another app):\n\n%s", hkErrors);
		MessageBoxW(0, errorMsg, L"Hotkey Registration Warning", MB_ICONWARNING | MB_OK);
	}
}

void UnregisterAppHotkeys(void) {
	for (int i = HK_QUIT; i <= HK_CYCLE_ADVANCE; i++) {
		UnregisterHotKey(NULL, i);
	}
}

HWND InitializeHiddenWindow(void) {
	WNDCLASSW wc = {0};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
	wc.lpszClassName = APP_CLASS;
	RegisterClassW(&wc);
	HWND hwnd = CreateWindowW(wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
	InitTrayIcon(hwnd);
	return hwnd;
}

int main(void) {
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	HANDLE hMutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
	if(!hMutex) {
		MessageBoxW(0, L"Failed to create single-instance mutex.", APP_NAME, MB_ICONERROR | MB_OK);
		if(argv) LocalFree(argv);
		return 1;
	}
	if(GetLastError() == ERROR_ALREADY_EXISTS) {
		MessageBoxW(0,
			L"BackgroundHotkeyThing is already running.\nCheck the tray / notification area.",
			APP_NAME, MB_ICONINFORMATION | MB_OK);
		CloseHandle(hMutex);
		if(argv) LocalFree(argv);
		return 0;
	}

	_KSYSTEM_TIME st;
	AppState appState = {0};
	g_state = &appState;
	appState.nsfwIndex = DEFAULT_NSFW_INDEX;
	appState.rrSlot = -1;
	appState.favs = xmalloc(sizeof(intStack));
	for(int i = 0; i < MAX_MONITORS; i++) {
		appState.mons[i].prevInd = -1;
		appState.mons[i].curbg = -1;
	}

	if (!appState.favs) {
		MessageBoxW(0, L"Failed to allocate memory.", APP_NAME, MB_ICONERROR | MB_OK);
		CloseHandle(hMutex);
		if(argv) LocalFree(argv);
		return 1;
	}

	ZeroMemory(appState.favs->inds, sizeof(int)*MAX_iSTACK_SIZE);
	appState.favs->top = -1;
	appState.favs->pointer = 0;

	wchar_t orgPaper[PATH_CCH] = {0};
	int running = 1;
	int approx_minutes = 2;
	settings.loop_pause = 0;
	settings.nsfw = 0;
	settings.onlyFavs = 0;
	settings.notifications = 1;
	settings.advanceMode = ADVANCE_ALL;

	if(!argv || argc < 2) {
		wchar_t usage[512];
		wsprintfW(usage,
			L"Usage: BackgroundHotkeyThing.exe <path to BG images> <rotation delay in minutes>\n\n"
			L"Scans nested folders (depth %d) for png/jpg/jpeg/bmp.\n"
			L"Any path segment named NSFW is treated as NSFW (case-insensitive).",
			SCAN_MAX_DEPTH);
		MessageBoxW(0, usage, L"Woops", MB_OK);
		xfree(appState.favs);
		CloseHandle(hMutex);
		if(argv) LocalFree(argv);
		return 0;
	}

	if(argc > 2) {
		int tmp = parsePositiveIntW(argv[2]);
		approx_minutes = (tmp > 0 ? tmp : approx_minutes);
	}

	wchar_t relpath[PATH_CCH] = {0};
	if(!resolveBgPathW(argv[1], relpath, PATH_CCH)) {
		MessageBoxW(0, L"Could not resolve background folder path.", L"Woops", MB_ICONERROR | MB_OK);
		xfree(appState.favs);
		CloseHandle(hMutex);
		LocalFree(argv);
		return 1;
	}
	printf(L"BG folder: %s\n", relpath);

	wchar_t efavfpath[PATH_CCH] = {0};
	wsprintfW(efavfpath, L"%s\\%s", relpath, INI_FILENAME);
	wcpy(g_iniPath, efavfpath, PATH_CCH);

	//very basic random seed: a little ASLR + SystemTime out of KUSER_SHARED_DATA...
	CopyMemory(&st, SystemTimePointer, sizeof(st));
	seedRng((unsigned int)((uintptr_t)&main + (uintptr_t)&ListDirectoryContentsW) + st.LowPart);

	appState.numBgs = initBGs(relpath, &appState, orgPaper, PATH_CCH);

	if(appState.numBgs == 0) {
		if (appState.bgs) {
			if (appState.bgs[0]) xfree(appState.bgs[0]);
			xfree(appState.bgs);
		}
		if(appState.favs) xfree(appState.favs);
		CloseHandle(hMutex);
		LocalFree(argv);
		return 1;
	}

	importFavs(efavfpath, appState.favs, appState.bgs, appState.numBgs);
	loadSettings(efavfpath, &settings);
	if(appState.nsfwIndex < 2){
		//only NSFW images (or nothing SFW) — nudge mode on
		printf(L"!!!!! no SFW partition, NSFW enabled !!!!!\n");
		settings.nsfw = 1;
		saveSettings(efavfpath, &settings);
	}

	g_sfwCount = (appState.nsfwIndex > 1) ? (appState.nsfwIndex - 1) : 0;
	g_nsfwCount = (appState.numBgs > appState.nsfwIndex) ? (appState.numBgs - appState.nsfwIndex) : 0;
	printf(L"Counts SFW:%d NSFW:%d (scan depth %d)\n", g_sfwCount, g_nsfwCount, SCAN_MAX_DEPTH);

	initDesktopWallpaper();
	refreshMonitors(&appState);

	HWND hwnd = InitializeHiddenWindow();
	g_hwnd = hwnd;
	{
		DEV_BROADCAST_DEVICEINTERFACE_W filter;
		ZeroMemory(&filter, sizeof(filter));
		filter.dbcc_size = sizeof(filter);
		filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
		filter.dbcc_classguid = GUID_MONITOR_INTERFACE;
		g_hDevNotify = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
	}
	UpdateTrayTip(hwnd);

	UINT timerInterval = approx_minutes * MS_PER_MIN;

	RegisterAppHotkeys();

	UINT_PTR timerId = 0;
	UINT_PTR holdTimer = 0;
	int holdDir = HOLD_NONE;
	int holdMon = -1;
	if (!settings.loop_pause) {
		timerId = SetTimer(NULL, TIMER_MAIN, timerInterval, NULL);
	}
	MSG msg = {0};

	while(running && GetMessageW(&msg, NULL, 0, 0) > 0) {
		if (msg.message == WM_TIMER && holdTimer && msg.wParam == holdTimer) {
			int stillHeld = 0;
			if(holdDir == HOLD_NEXT) stillHeld = isNextHeld();
			else if(holdDir == HOLD_PREV) stillHeld = isPrevHeld();

			if(stillHeld) {
				if(holdDir == HOLD_NEXT) {
					bumpAutoTimer(&timerId, timerInterval);
					AdvanceBackground(&appState, holdMon);
				} else if(holdDir == HOLD_PREV && holdMon >= 0 && appState.mons[holdMon].prevInd >= 0) {
					bumpAutoTimer(&timerId, timerInterval);
					PreviousBackground(&appState, holdMon);
				}
				KillTimer(NULL, holdTimer);
				holdTimer = SetTimer(NULL, TIMER_HOLD, HOLD_REPEAT_MS, NULL);
			} else {
				stopHoldCycle(&holdTimer, &holdDir, &holdMon);
			}
		} else if (msg.message == WM_TIMER && timerId && msg.wParam == timerId) {
			AdvanceNextByMode(&appState);
		} else if (msg.message == WM_HOTKEY) {
			HandleHotkey((int)msg.wParam, &appState, hwnd, efavfpath, &timerId, timerInterval, &running,
				&holdTimer, &holdDir, &holdMon);
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	stopHoldCycle(&holdTimer, &holdDir, &holdMon);
	if (timerId) KillTimer(NULL, timerId);
	if (hwnd) KillTimer(hwnd, TIMER_DISPLAY);
	UnregisterAppHotkeys();

	if (g_hDevNotify) {
		UnregisterDeviceNotification(g_hDevNotify);
		g_hDevNotify = NULL;
	}
	RemoveTrayIcon(hwnd);
	DestroyWindow(hwnd);
	g_hwnd = NULL;
	shutdownDesktopWallpaper();

	if (appState.bgs) {
		for (int i = 0; i < appState.numBgs; i++) {
			if (appState.bgs[i]) xfree(appState.bgs[i]);
		}
		xfree(appState.bgs);
	}
	if (appState.favs) xfree(appState.favs);
	CloseHandle(hMutex);
	LocalFree(argv);
	return 0;
}
