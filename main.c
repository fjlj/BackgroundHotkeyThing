#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define MAX_HISTORY 50
#define MAX_iSTACK_SIZE 10
#define SystemTimePointer ((_KSYSTEM_TIME*)0x7FFE0014)
#define IDI_APP_ICON 101
#define TOAST_DURATION_MS 1500

#define APP_NAME "BackgroundHotkeyThing"
#define APP_CLASS "BgHotkeyMsgWindow"
#define INI_FILENAME "BackgroundHotkeyThing.ini"
#define INI_SEC_SETTINGS "Settings"
#define INI_SEC_FAVS "Favs"
#define MUTEX_NAME "Local\\BackgroundHotkeyThing_SingleInstance"
#define TIMER_MAIN 1
#define TIMER_TOAST 2
#define TIMER_HOLD 3
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

//flip this on when you want to snoop via DebugView / a debugger
//#define DEBUG

#ifdef DEBUG
	//wsprintfA + OutputDebugString - no stdio tax, still a real printf-shaped thing
	#define printf(...) do { char _dbg[1024]; wsprintfA(_dbg, __VA_ARGS__); OutputDebugStringA(_dbg); } while(0)
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
	HK_TOGGLE_NOTIF
} HotkeyID;

typedef struct {
	int onlyFavs;
	int nsfw;
	int loop_pause;
	int notifications;
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

typedef struct {
	char** bgs;
	int numBgs;
	int nsfwIndex;
	int curbg;
	//prev[] = advances only. prevInd==-1 is "home" (launch wallpaper, bgs[0])
	int prev[MAX_HISTORY];
	int prevInd;
	intStack* favs;
} AppState;

AppSettings settings = {0};
//folder counts for tray tip / menu (slot 0 is launch snapshot, not counted)
int g_sfwCount = 0;
int g_nsfwCount = 0;

//Middle Square Weyl Sequence state (see seedRng / nextRand)
//x = running square, w = Weyl accumulator, s = odd Weyl step
static unsigned long long g_ms_x = 0;
static unsigned long long g_ms_w = 0;
static unsigned long long g_ms_s = 0xb5ad4eceda1ce2a9ULL;

//tiny libc-free helpers so we dont drag stdio/stdlib into a wallpaper toy
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

//square -> add Weyl -> rotate halves out as the "middle". 31 useful bits.
//Middle Square Weyl Sequence (Widynski) - von Neumann middle-square + Weyl walk
//so it doesnt collapse into a sad little cycle. s must stay odd.
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
	//fold boot entropy into the step constant; golden-ratio-ish mix just for spice
	g_ms_s = 0xb5ad4eceda1ce2a9ULL ^ ((unsigned long long)seed * 0x9E3779B97F4A7C15ULL);
	g_ms_s |= 1ULL;
	//burn a few so we arent living on zeros
	for(int i = 0; i < 16; i++) nextRand();
}

//positive ints only (rotation minutes). garbage -> 0
int parsePositiveInt(const char* s) {
	int n = 0;
	if(!s || !*s) return 0;
	for(; *s; s++) {
		if(*s < '0' || *s > '9') return 0;
		n = n * 10 + (*s - '0');
	}
	return n;
}

char* lastPathSep(char* s) {
	char* p = NULL;
	for(; *s; s++) {
		if(*s == '\\' || *s == '/') p = s;
	}
	return p;
}

const char* lastDot(const char* s) {
	const char* p = NULL;
	for(; *s; s++) {
		if(*s == '.') p = s;
	}
	return p;
}

//drive / UNC / \\?\ / root-relative = absolute. BGs / .\BGs / ..\x = relative
int isAbsoluteWinPath(const char* p) {
	if(!p || !p[0]) return 0;
	if(p[0] == '\\' && p[1] == '\\') return 1;
	if(p[0] == '\\' || p[0] == '/') return 1;
	if(((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
		if(p[2] == '\0' || p[2] == '\\' || p[2] == '/') return 1;
	}
	return 0;
}

//absolute/UNC stay rooted; relative hangs off the *exe* dir (not cwd - shortcuts thank us)
int resolveBgPath(const char* input, char* out, DWORD outSize) {
	char combined[MAX_PATH] = {0};

	if(!input || !input[0] || !out || outSize < 2) return 0;

	if(isAbsoluteWinPath(input)) {
		DWORD n = GetFullPathNameA(input, outSize, out, NULL);
		if(n == 0 || n >= outSize) {
			lstrcpynA(out, input, (int)outSize);
		}
		return 1;
	}

	char exePath[MAX_PATH] = {0};
	if(!GetModuleFileNameA(NULL, exePath, MAX_PATH)) return 0;
	char* slash = lastPathSep(exePath);
	if(slash) *slash = '\0';
	else {
		lstrcpynA(out, input, (int)outSize);
		return 1;
	}

	wsprintfA(combined, "%s\\%s", exePath, input);
	DWORD n = GetFullPathNameA(combined, outSize, out, NULL);
	if(n == 0 || n >= outSize) {
		lstrcpynA(out, combined, (int)outSize);
	}
	printf("Resolved relative path:\n  in : %s\n  out: %s\n", input, out);
	return 1;
}

//png/jpg/jpeg/bmp, case-insensitive. no extension = kick rocks
int hasAllowedExt(const char* fileName) {
	const char* dot = lastDot(fileName);
	if(!dot || !dot[1]) return 0;
	if(lstrcmpiA(dot, ".png") == 0) return 1;
	if(lstrcmpiA(dot, ".jpg") == 0) return 1;
	if(lstrcmpiA(dot, ".jpeg") == 0) return 1;
	if(lstrcmpiA(dot, ".bmp") == 0) return 1;
	return 0;
}

//random in [lo, hi] inclusive; busted range just hands back lo
//rejection sampling so we dont bias low numbers when span doesnt divide 2^31
int randRange(int lo, int hi) {
	if(hi < lo) return lo;
	int span = hi - lo + 1;
	if(span <= 1) return lo;
	//largest multiple of span that fits in 31 bits
	unsigned int limit = (unsigned int)(0x80000000UL - (0x80000000UL % (unsigned int)span));
	unsigned int r;
	do {
		r = (unsigned int)nextRand();
	} while(r >= limit);
	return lo + (int)(r % (unsigned int)span);
}

//true if bgInd is current or sits in the last `window` history slots
int wasRecent(AppState* state, int bgInd, int window) {
	if(bgInd == state->curbg) return 1;
	if(window <= 0 || state->prevInd < 0) return 0;

	int start = state->prevInd - window + 1;
	if(start < 0) start = 0;
	for(int i = start; i <= state->prevInd; i++) {
		if(state->prev[i] == bgInd) return 1;
	}
	return 0;
}

//random pick in [lo,hi] that isnt in the recent-exclude window.
//if the folder is smaller than RECENT_EXCLUDE, window shrinks so we always have a way out.
int pickFreshBg(AppState* state, int lo, int hi) {
	int span = hi - lo + 1;
	if(span <= 1) return lo;

	int window = RECENT_EXCLUDE;
	if(window >= span) window = span - 1;
	if(window < 0) window = 0;

	//a handful of random shots first (cheap path)
	for(int attempt = 0; attempt < 48; attempt++) {
		int c = randRange(lo, hi);
		if(!wasRecent(state, c, window)) {
			printf("pickFresh: %d (window=%d)\n", c, window);
			return c;
		}
	}

	//fallback: walk the range from a random start so we dont always bias lo
	int start = randRange(lo, hi);
	for(int n = 0; n < span; n++) {
		int c = lo + ((start - lo + n) % span);
		if(!wasRecent(state, c, window)) {
			printf("pickFresh scan: %d (window=%d)\n", c, window);
			return c;
		}
	}

	//everything is "recent" somehow - just roll the dice
	printf("pickFresh: pool exhausted, allowing repeat\n");
	return randRange(lo, hi);
}

//SFW first (ind starts at 1; slot 0 is reserved for launch wallpaper), then NSFW folder
int ListDirectoryContents(const char *sDir, char*** bgs_ptr, int* capacity, int *nsfwInd) {
	WIN32_FIND_DATA fdFile;
	HANDLE hFind = NULL;
	char sPath[MAX_PATH] = {0};
	char NSFWpath[MAX_PATH] = {0};

	int ind = 1;
	//*.* = everything, we filter extensions ourselves
	wsprintfA(sPath, "%s\\*.*", sDir);
	wsprintfA(NSFWpath, "%s\\NSFW\\*.*", sDir);

	if((hFind = FindFirstFile(sPath, &fdFile)) == INVALID_HANDLE_VALUE) {
		return ind;
	}

	do {
		if(!(fdFile.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && 
		   lstrcmpA(fdFile.cFileName, ".") != 0  && 
		   lstrcmpA(fdFile.cFileName, "..") != 0 && 
		   hasAllowedExt(fdFile.cFileName)) {
		   	
			if (ind >= *capacity) {
				size_t new_capacity = *capacity * 2;
				char** new_bgs = xrealloc(*bgs_ptr, new_capacity * sizeof(char*));
				if (!new_bgs) return ind;
				*capacity = (int)new_capacity;
				*bgs_ptr = new_bgs;
			}

			wsprintfA(sPath, "%s\\%s", sDir, fdFile.cFileName);
			size_t slen = (size_t)lstrlenA(sPath)+1;
			(*bgs_ptr)[ind] = xmalloc(slen);
			CopyMemory((*bgs_ptr)[ind], sPath, slen);
			printf("File: %d:%s\n", ind, (*bgs_ptr)[ind]);
			ind++;
		}
	} while(FindNextFile(hFind, &fdFile));

	FindClose(hFind); //Always, Always, clean things up!
	*nsfwInd = ind;
	
	if((hFind = FindFirstFile(NSFWpath, &fdFile)) == INVALID_HANDLE_VALUE) {
		return ind;
	}

	do {
		if(!(fdFile.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
		   lstrcmpA(fdFile.cFileName, ".") != 0 && 
		   lstrcmpA(fdFile.cFileName, "..") != 0 && 
		   hasAllowedExt(fdFile.cFileName)) {
		   	
			if (ind >= *capacity) {
				size_t new_capacity = *capacity * 2;
				char** new_bgs = xrealloc(*bgs_ptr, new_capacity * sizeof(char*));
				if (!new_bgs) return ind;
				*capacity = (int)new_capacity;
				*bgs_ptr = new_bgs;
			}

			wsprintfA(NSFWpath, "%s\\NSFW\\%s", sDir, fdFile.cFileName);
			size_t slen = (size_t)lstrlenA(NSFWpath)+1;
			(*bgs_ptr)[ind] = xmalloc(slen);
			CopyMemory((*bgs_ptr)[ind], NSFWpath, slen);
			printf("File: %d:%s\n", ind, (*bgs_ptr)[ind]);
			ind++;
		}
	} while(FindNextFile(hFind, &fdFile));

	FindClose(hFind); //Always, Always, clean things up!

	return ind;
}

//desktop listview host - Progman first, then WorkerW scavenger hunt (icon hide/show)
HWND gethShellViewWin() {
	HWND prgMan = FindWindowA("Progman", "Program Manager");
	HWND hShellViewWin = FindWindowExA(prgMan, 0, "SHELLDLL_DefView", "");

	if(hShellViewWin == 0x00) {
		HWND hWorkerW = 0x00;
		do {
			hWorkerW = FindWindowExA(0, hWorkerW, "WorkerW", "");
			hShellViewWin = FindWindowExA(hWorkerW, 0, "SHELLDLL_DefView", "");
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

//yank one slot and close the gap (upsert helper)
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

//push to top; if full, shift down (drop oldest) and write top
//also returns the data written... (not really needed but eh why not)
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

//already in list? pull it out and re-push to top. full + brand new? FIFO drop oldest.
int upsertIntStack(intStack* stack, int data) {
	int existing = findInIntStack(stack, data);
	if(existing >= 0) {
		removeIntStackAt(stack, existing);
		printf("fav upsert: moved existing slot %d to top\n", existing);
	}
	return pushIntStack(stack, data);
}

//walk favorites newest->oldest; wrap. returns current then steps pointer down 1
int peekIntStackItr(intStack* stack){
	if(stack->top <= -1) { stack->top = -1; return 0; }
	int data = stack->inds[stack->pointer];
	if(--stack->pointer < 0)
		stack->pointer = (stack->top < 0 ? 0 : stack->top);
	return data;
}

void printFavs(intStack* favs,char **bgs){
	printf("------Current Favorites------\n");
	for(int i = 0; i <= favs->top; i++){
		printf("fav[%d]:%d = %s\n",i,favs->inds[i],bgs[favs->inds[i]]);
	}
}

//wipe [Favs] then rewrite so we dont leave ghost Fav-N slots behind
void saveFavs(char* efavfpath,intStack* favs, char **bgs){
	WritePrivateProfileStringA(INI_SEC_FAVS, NULL, NULL, efavfpath);
	if(favs->top < 0) return;

	char slotName[16] = {0};
	for(int i = 0; i <= favs->top; i++){
		wsprintfA(slotName,"Fav-%d",i);
		WritePrivateProfileStringA(
			INI_SEC_FAVS,
			slotName,
			bgs[favs->inds[i]],
			efavfpath);
	}  
}

void saveSettings(char* efavfpath, AppSettings* s) {
	char ival[8] = {0};
	wsprintfA(ival, "%d", s->onlyFavs);
	WritePrivateProfileStringA(INI_SEC_SETTINGS, "Set-0", ival, efavfpath);
	wsprintfA(ival, "%d", s->nsfw);
	WritePrivateProfileStringA(INI_SEC_SETTINGS, "Set-1", ival, efavfpath);
	wsprintfA(ival, "%d", s->loop_pause);
	WritePrivateProfileStringA(INI_SEC_SETTINGS, "Set-2", ival, efavfpath);
	wsprintfA(ival, "%d", s->notifications);
	WritePrivateProfileStringA(INI_SEC_SETTINGS, "Set-3", ival, efavfpath);
}

void loadSettings(char* efavfpath, AppSettings* s) {
	s->onlyFavs = GetPrivateProfileIntA(INI_SEC_SETTINGS, "Set-0", s->onlyFavs, efavfpath);
	s->nsfw = GetPrivateProfileIntA(INI_SEC_SETTINGS, "Set-1", s->nsfw, efavfpath);
	s->loop_pause = GetPrivateProfileIntA(INI_SEC_SETTINGS, "Set-2", s->loop_pause, efavfpath);
	s->notifications = GetPrivateProfileIntA(INI_SEC_SETTINGS, "Set-3", s->notifications, efavfpath);
}

void importFavs(char* efavfpath, intStack* favs,char* bgs[],int numBgs){
	char favPath[MAX_PATH] = {0x00};
	char slotName[16] = {0};
	for(int i = 0; i < MAX_iSTACK_SIZE; i++){
		
		wsprintfA(slotName,"Fav-%d",i);
		
		if(!GetPrivateProfileStringA(
	  		INI_SEC_FAVS,
		    slotName,
		    "",
		    favPath,
		    MAX_PATH,
		    efavfpath)
		) break;
	    
	    for(int o = 0; o < numBgs; o++){
			if(lstrcmpA(bgs[o], favPath) == 0){
				//import oldest->newest with plain push (upsert would reshuffle on load)
				pushIntStack(favs,o);
				break;
			}
		} 	
	}	    
}

//home lives outside the ring (prevInd==-1 / bgs[0]). prev[] is advances only -
//filling the ring never steals your way back to the launch wallpaper.
void historyPush(AppState* state, int bgInd) {
	if(state->prevInd < 0) {
		state->prevInd = 0;
		state->prev[0] = bgInd;
	} else if(state->prevInd < MAX_HISTORY - 1) {
		state->prev[++state->prevInd] = bgInd;
	} else {
		MoveMemory(&state->prev[0], &state->prev[1], sizeof(int) * (MAX_HISTORY - 1));
		state->prev[MAX_HISTORY - 1] = bgInd;
		state->prevInd = MAX_HISTORY - 1;
	}
	state->curbg = bgInd;
}

//nsfw partition starts at nsfwIndex. when nsfw mode is off, walk the whole fav
//ring once and skip NSFW slots instead of stalling on the first one forever.
int nextFav(AppState* state) {
	if(state->favs->top < 0) return -1;

	int attempts = (int)state->favs->top + 1;
	for(int n = 0; n < attempts; n++) {
		int favsp = (int)state->favs->pointer;
		int favSlot = peekIntStackItr(state->favs);

		if(favSlot < state->nsfwIndex || settings.nsfw > 0) {
			printf("load fav[%d] = %d - bg: %s\n", favsp, favSlot, state->bgs[favSlot]);
			SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, state->bgs[favSlot], SPIF_SENDCHANGE);
			return favSlot;
		}
		printf("skip NSFW fav[%d]=%d (nsfw mode off)\n", favsp, favSlot);
	}

	printf("No loadable favorites for current NSFW mode\n");
	return -1;
}

int initBGs(char* relpath, AppState* appState, char* orgPaper){
	//snapshot their current desktop - bgs[0], start at home (prevInd=-1)
	SystemParametersInfo(SPI_GETDESKWALLPAPER,MAX_PATH,orgPaper,0);
	
	int capacity = 1000;
	appState->bgs = xmalloc(capacity * sizeof(char*));
	if (!appState->bgs) return 0;

	size_t ogPathLen = (size_t)lstrlenA(orgPaper)+1;
	appState->bgs[0] = xmalloc(ogPathLen);
	CopyMemory(appState->bgs[0], orgPaper, ogPathLen);

	appState->prevInd = -1;
	appState->curbg = 0;

	int numBgs = ListDirectoryContents(relpath, &(appState->bgs), &capacity, &(appState->nsfwIndex));

	printf("%d Backgrounds Loaded.\nNSFW Begins at:%d\n",numBgs,appState->nsfwIndex);

	//numBgs==1 means we only have the launch snapshot, no folder images
	if(numBgs <= 1) {
		char errmsg[MAX_PATH+32];
		wsprintfA(errmsg,"Path or Images not found at: [%s]\n",relpath);
		MessageBoxA(0,errmsg,"Whoops!", 0);
		return 0;
	}
	
	return numBgs;
}

#define WM_TRAYICON (WM_APP + 1)
#define TRAY_ICON_ID 1

void AdvanceFavorite(AppState* state) {
	int nextFfavs = nextFav(state);
	if(nextFfavs != -1){
		historyPush(state, nextFfavs);
	}
}

void AdvanceBackground(AppState* state) {
	if(settings.onlyFavs){
		AdvanceFavorite(state);
		return;
	}

	int lo = 1;
	int hi = state->numBgs - 1;

	if(!settings.nsfw) {
		//SFW only (skip index 0 = launch snapshot)
		lo = 1;
		hi = state->nsfwIndex - 1;
	} else if(settings.nsfw == 2) {
		lo = state->nsfwIndex;
		hi = state->numBgs - 1;
	} else {
		//combined: everything loaded except the launch snapshot
		lo = 1;
		hi = state->numBgs - 1;
	}

	if(hi < lo) {
		printf("No backgrounds available for current NSFW mode (lo=%d hi=%d)\n", lo, hi);
		return;
	}

	int nextBg = pickFreshBg(state, lo, hi);
	historyPush(state, nextBg);
	printf("Setting:[%d]%s\n", state->curbg, state->bgs[state->curbg]);
	SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, state->bgs[state->curbg], SPIF_SENDCHANGE);
}

void PreviousBackground(AppState* state) {
	if(state->prevInd < 0) return; //already home

	if(state->prevInd == 0) {
		//step off oldest advance -> always restore launch wallpaper
		state->prevInd = -1;
		state->curbg = 0;
	} else {
		state->prevInd--;
		state->curbg = state->prev[state->prevInd];
	}
	printf("Setting:[%d]%s\n", state->curbg, state->bgs[state->curbg]);
	SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, state->bgs[state->curbg], SPIF_SENDCHANGE);
}

int keyDown(int vk) {
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

int winKeyDown(void) {
	return keyDown(VK_LWIN) || keyDown(VK_RWIN);
}

//still holding the registered combo? used for hold-to-cycle
int isNextHeld(void) {
	return winKeyDown() && keyDown(VK_SHIFT) && keyDown('N');
}
int isPrevHeld(void) {
	return winKeyDown() && keyDown(VK_SHIFT) && keyDown('B');
}

void stopHoldCycle(UINT_PTR* holdTimer, int* holdDir) {
	if(holdTimer && *holdTimer) {
		KillTimer(NULL, *holdTimer);
		*holdTimer = 0;
	}
	if(holdDir) *holdDir = HOLD_NONE;
}

void startHoldCycle(int dir, UINT_PTR* holdTimer, int* holdDir) {
	*holdDir = dir;
	if(*holdTimer) KillTimer(NULL, *holdTimer);
	//first follow-up is a hair slower so a tap doesnt double-fire
	*holdTimer = SetTimer(NULL, TIMER_HOLD, HOLD_INITIAL_MS, NULL);
}

void UpdateTrayTip(HWND hwnd) {
	NOTIFYICONDATAA nid = {0};
	nid.cbSize = sizeof(NOTIFYICONDATAA);
	nid.hWnd = hwnd;
	nid.uID = TRAY_ICON_ID;
	nid.uFlags = NIF_TIP;
	//szTip is 128 chars - plenty for a quick inventory
	wsprintfA(nid.szTip, "%s\nSFW: %d | NSFW: %d", APP_NAME, g_sfwCount, g_nsfwCount);
	Shell_NotifyIconA(NIM_MODIFY, &nid);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch(msg) {
		case WM_TRAYICON:
			if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
				POINT pt;
				GetCursorPos(&pt);
				HMENU hMenu = CreatePopupMenu();

				char infoLine[64];
				wsprintfA(infoLine, "Loaded  SFW: %d  |  NSFW: %d", g_sfwCount, g_nsfwCount);
				AppendMenuA(hMenu, MF_STRING | MF_GRAYED, 0, infoLine);
				AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);

				AppendMenuA(hMenu, MF_STRING, HK_NEXT_BG, "Next Background\tWin+Shift-N");
				AppendMenuA(hMenu, MF_STRING, HK_PREV_BG, "Previous Background\tWin+Shift-B");
				AppendMenuA(hMenu, MF_STRING | (settings.loop_pause ? MF_CHECKED : MF_UNCHECKED), HK_PAUSE, "Toggle Pause\tWin+Alt-P");
				
				char nsfwMenuText[64];
				UINT nsfwState = MF_UNCHECKED;
				if (settings.nsfw == 0) {
					lstrcpynA(nsfwMenuText, "NSFW Mode: Off\tWin+Shift-X", sizeof(nsfwMenuText));
				} else if (settings.nsfw == 1) {
					lstrcpynA(nsfwMenuText, "[-] NSFW Mode: Combined\tWin+Shift-H", sizeof(nsfwMenuText));
				} else {
					lstrcpynA(nsfwMenuText, "NSFW Mode: Only NSFW\tWin+Shift-H", sizeof(nsfwMenuText));
					nsfwState = MF_CHECKED;
				}
				AppendMenuA(hMenu, MF_STRING | nsfwState, HK_TOGGLE_NSFW, nsfwMenuText);
				
				AppendMenuA(hMenu, MF_STRING | (settings.onlyFavs ? MF_CHECKED : MF_UNCHECKED), HK_CYCLE_FAVS, "Toggle Cycle Favs\tWin+Shift-L");
				AppendMenuA(hMenu, MF_STRING | (settings.notifications ? MF_CHECKED : MF_UNCHECKED), HK_TOGGLE_NOTIF, "Toggle Notifications\tWin+Alt-N");
				AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
				AppendMenuA(hMenu, MF_STRING, HK_SAVE_FAV, "Save Favorite\tWin+Shift-A");
				AppendMenuA(hMenu, MF_STRING, HK_CLEAR_FAVS, "Clear Favorites\tWin+Shift-C");
				AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
				AppendMenuA(hMenu, MF_STRING, HK_TOGGLE_ICONS, "Toggle Desktop Icons\tWin+Shift-Z");
				AppendMenuA(hMenu, MF_STRING, HK_OPEN_EXPLORER, "Open in Explorer\tWin+Shift-O");
				AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
				AppendMenuA(hMenu, MF_STRING, HK_QUIT, "Quit\tWin+Alt-Q");
				
				SetForegroundWindow(hwnd); //menu needs this or it sticks around like a bad roommate
				int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
				PostMessage(hwnd, WM_NULL, 0, 0); //Windows being windows...
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
		case WM_TIMER:
			if (wParam == TIMER_TOAST) { 
				KillTimer(hwnd, TIMER_TOAST);
				NOTIFYICONDATAA nid = {0};
				nid.cbSize = sizeof(NOTIFYICONDATAA);
				nid.hWnd = hwnd;
				nid.uID = TRAY_ICON_ID;
				nid.uFlags = NIF_INFO;
				nid.szInfo[0] = '\0';
				Shell_NotifyIconA(NIM_MODIFY, &nid);
			}
			break;
		default:
			return DefWindowProcA(hwnd, msg, wParam, lParam);
	}
	return 0;
}

void InitTrayIcon(HWND hwnd) {
	NOTIFYICONDATAA nid = {0};
	nid.cbSize = sizeof(NOTIFYICONDATAA);
	nid.hWnd = hwnd;
	nid.uID = TRAY_ICON_ID;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_TRAYICON;
	nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
	lstrcpynA(nid.szTip, APP_NAME, sizeof(nid.szTip));
	Shell_NotifyIconA(NIM_ADD, &nid);
}

void RemoveTrayIcon(HWND hwnd) {
	NOTIFYICONDATAA nid = {0};
	nid.cbSize = sizeof(NOTIFYICONDATAA);
	nid.hWnd = hwnd;
	nid.uID = TRAY_ICON_ID;
	Shell_NotifyIconA(NIM_DELETE, &nid);
}

void ShowNotification(HWND hwnd, const char* title, const char* message, int timeoutMs) {
	if (!settings.notifications) return;

	NOTIFYICONDATAA nid = {0};
	nid.cbSize = sizeof(NOTIFYICONDATAA);
	nid.hWnd = hwnd;
	nid.uID = TRAY_ICON_ID;
	nid.uFlags = NIF_INFO;
	lstrcpynA(nid.szInfoTitle, title, sizeof(nid.szInfoTitle));
	lstrcpynA(nid.szInfo, message, sizeof(nid.szInfo));
	nid.dwInfoFlags = NIIF_NOSOUND;
	nid.uTimeout = timeoutMs;
	Shell_NotifyIconA(NIM_MODIFY, &nid);

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

void HandleHotkey(int hotkeyId, AppState* appState, HWND hwnd, char* efavfpath,
	UINT_PTR* timerId, UINT timerInterval, int* running,
	UINT_PTR* holdTimer, int* holdDir) {
	switch (hotkeyId) {
		case HK_QUIT:
			stopHoldCycle(holdTimer, holdDir);
			*running = 0;
			break;
		case HK_TOGGLE_ICONS:
			{
				HWND hShellViewWin = gethShellViewWin();
				if(hShellViewWin) SendMessage(hShellViewWin,0x0111, 0x7402, 0);
			}
			break;
		case HK_SAVE_FAV:
			upsertIntStack(appState->favs, appState->curbg);
			saveFavs(efavfpath, appState->favs, appState->bgs);
			printFavs(appState->favs, appState->bgs);
			ShowNotification(hwnd, "Favorites", "Saved current background to favorites!", TOAST_DURATION_MS);
			break;
		case HK_NEXT_BG:
			//tap = one step; hold = keep stepping via TIMER_HOLD + GetAsyncKeyState
			bumpAutoTimer(timerId, timerInterval);
			AdvanceBackground(appState);
			startHoldCycle(HOLD_NEXT, holdTimer, holdDir);
			break;
		case HK_PREV_BG:
			bumpAutoTimer(timerId, timerInterval);
			if(appState->prevInd >= 0) {
				PreviousBackground(appState);
			}
			startHoldCycle(HOLD_PREV, holdTimer, holdDir);
			break;
		case HK_PAUSE:
			settings.loop_pause ^= 1;
			if (settings.loop_pause) {
				if (*timerId) KillTimer(NULL, *timerId);
				*timerId = 0;
				ShowNotification(hwnd, APP_NAME, "Auto Rotate: Paused", TOAST_DURATION_MS);
			} else {
				if (*timerId) KillTimer(NULL, *timerId);
				*timerId = SetTimer(NULL, TIMER_MAIN, timerInterval, NULL);
				ShowNotification(hwnd, APP_NAME, "Auto Rotate: Resumed", TOAST_DURATION_MS);
			}
			saveSettings(efavfpath, &settings);
			break;
		case HK_TOGGLE_NSFW:
			settings.nsfw = ++settings.nsfw % 3;
			printf("NSFW:%d\n", settings.nsfw);
			if (settings.nsfw == 0) ShowNotification(hwnd, "NSFW Mode", "Off", TOAST_DURATION_MS);
			else if (settings.nsfw == 1) ShowNotification(hwnd, "NSFW Mode", "Combined", TOAST_DURATION_MS);
			else ShowNotification(hwnd, "NSFW Mode", "Only NSFW", TOAST_DURATION_MS);
			saveSettings(efavfpath, &settings);
			break;
		case HK_CYCLE_FAVS:
			settings.onlyFavs ^= 1;
			printf("Cycle:%s\n", (settings.onlyFavs ? "Only Favorites" : "Normal"));
			ShowNotification(hwnd, "Cycle Mode", settings.onlyFavs ? "Only Favorites" : "Normal", TOAST_DURATION_MS);
			saveSettings(efavfpath, &settings);
			break;
		case HK_CLEAR_FAVS:
			printf("Clearing Favorites\n");
			appState->favs->top = -1;
			appState->favs->pointer = 0;
			ZeroMemory(appState->favs->inds, sizeof(int) * MAX_iSTACK_SIZE);
			saveFavs(efavfpath, appState->favs, appState->bgs);
			ShowNotification(hwnd, "Favorites", "Cleared all favorites!", TOAST_DURATION_MS);
			break;
		case HK_OPEN_EXPLORER:
			{
				char args[MAX_PATH + 32] = {0};
				wsprintfA(args, "/select,\"%s\"", appState->bgs[appState->curbg]);
				ShellExecuteA(NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
			}
			break;
		case HK_TOGGLE_NOTIF:
			settings.notifications ^= 1;
			saveSettings(efavfpath, &settings);
			if (settings.notifications) {
				ShowNotification(hwnd, APP_NAME, "Notifications Enabled", TOAST_DURATION_MS);
			}
			break;
	}
}

void RegisterAppHotkeys() {
	char hkErrors[1024] = {0};

	if(!RegisterHotKey(NULL, HK_QUIT, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'Q')) lstrcatA(hkErrors, "- Win+Alt-Q (Quit)\n");
	if(!RegisterHotKey(NULL, HK_TOGGLE_ICONS, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'Z')) lstrcatA(hkErrors, "- Win+Shift-Z (Toggle Icons)\n");
	if(!RegisterHotKey(NULL, HK_SAVE_FAV, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'A')) lstrcatA(hkErrors, "- Win+Shift-A (Save Fav)\n");
	if(!RegisterHotKey(NULL, HK_NEXT_BG, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'N')) lstrcatA(hkErrors, "- Win+Shift-N (Next BG)\n");
	if(!RegisterHotKey(NULL, HK_PREV_BG, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'B')) lstrcatA(hkErrors, "- Win+Shift-B (Prev BG)\n");
	if(!RegisterHotKey(NULL, HK_PAUSE, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'P')) lstrcatA(hkErrors, "- Win+Alt-P (Pause)\n");
	if(!RegisterHotKey(NULL, HK_TOGGLE_NSFW, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'X')) lstrcatA(hkErrors, "- Win+Shift-X (Toggle NSFW)\n");
	if(!RegisterHotKey(NULL, HK_CYCLE_FAVS, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'L')) lstrcatA(hkErrors, "- Win+Shift-L (Cycle Favs)\n");
	if(!RegisterHotKey(NULL, HK_CLEAR_FAVS, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'C')) lstrcatA(hkErrors, "- Win+Shift-C (Clear Favs)\n");
	if(!RegisterHotKey(NULL, HK_OPEN_EXPLORER, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'O')) lstrcatA(hkErrors, "- Win+Shift-O (Open Explorer)\n");
	if(!RegisterHotKey(NULL, HK_TOGGLE_NOTIF, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'N')) lstrcatA(hkErrors, "- Win+Alt-N (Toggle Notifications)\n");

	if (hkErrors[0] != '\0') {
		char errorMsg[2048] = {0};
		wsprintfA(errorMsg, "The following hotkeys failed to register (they might be in use by Windows or another app):\n\n%s", hkErrors);
		MessageBoxA(0, errorMsg, "Hotkey Registration Warning", MB_ICONWARNING | MB_OK);
	}
}

void UnregisterAppHotkeys() {
	for (int i = HK_QUIT; i <= HK_TOGGLE_NOTIF; i++) {
		UnregisterHotKey(NULL, i);
	}
}

HWND InitializeHiddenWindow() {
	WNDCLASSA wc = {0};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
	wc.lpszClassName = APP_CLASS;
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA(wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
	InitTrayIcon(hwnd);
	return hwnd;
}

int main(int argc, char *argv[]) {
	//one instance only - second launch just nags and peaces out
	HANDLE hMutex = CreateMutexA(NULL, FALSE, MUTEX_NAME);
	if(!hMutex) {
		MessageBoxA(0, "Failed to create single-instance mutex.", APP_NAME, MB_ICONERROR | MB_OK);
		return 1;
	}
	if(GetLastError() == ERROR_ALREADY_EXISTS) {
		MessageBoxA(0,
			"BackgroundHotkeyThing is already running.\nCheck the tray / notification area.",
			APP_NAME, MB_ICONINFORMATION | MB_OK);
		CloseHandle(hMutex);
		return 0;
	}

	_KSYSTEM_TIME st;
	AppState appState = {0};
	appState.nsfwIndex = DEFAULT_NSFW_INDEX;
	appState.favs = xmalloc(sizeof(intStack));
	
	if (!appState.favs) {
		MessageBoxA(0, "Failed to allocate memory.", APP_NAME, MB_ICONERROR | MB_OK);
		CloseHandle(hMutex);
		return 1;
	}
	
	ZeroMemory(appState.favs->inds, sizeof(int)*MAX_iSTACK_SIZE);
	appState.favs->top = -1;
	appState.favs->pointer = 0;

	char orgPaper[MAX_PATH] = {0x00};
	int running = 1;
	int approx_minutes = 2;
	settings.loop_pause = 0;
	settings.nsfw = 0;
	settings.onlyFavs = 0;
	settings.notifications = 1;

	if(argc < 2) {
		MessageBoxA(0,"Usage: BackgroundHotkeyThing.exe <path to BG images> <rotation delay in minutes>\nNOTE: single folder(non-recursive)\n","Woops",0);
		xfree(appState.favs);
		CloseHandle(hMutex);
		return 0;
	}
	
	if(argc > 2) {
		int tmp = parsePositiveInt(argv[2]);
		approx_minutes = (tmp > 0  ? tmp : approx_minutes);
	}
	
	char relpath[MAX_PATH] = {0};
	if(!resolveBgPath(argv[1], relpath, MAX_PATH)) {
		MessageBoxA(0, "Could not resolve background folder path.", "Woops", MB_ICONERROR | MB_OK);
		xfree(appState.favs);
		CloseHandle(hMutex);
		return 1;
	}
	printf("BG folder: %s\n", relpath);
	
	char efavfpath[MAX_PATH] = {0};
	wsprintfA(efavfpath, "%s\\%s", relpath, INI_FILENAME);
	
	//very basic random seed: a little ASLR + SystemTime out of KUSER_SHARED_DATA...
	CopyMemory(&st,SystemTimePointer,sizeof(st));
	seedRng((unsigned int)((uintptr_t)&main + (uintptr_t)&ListDirectoryContents) + st.LowPart);
	
	appState.numBgs = initBGs(relpath, &appState, orgPaper);
	
	if(appState.numBgs == 0) {
		if (appState.bgs) {
			if (appState.bgs[0]) xfree(appState.bgs[0]);
			xfree(appState.bgs);
		}
		if(appState.favs) xfree(appState.favs);
		CloseHandle(hMutex);
		return 1;
	}
	
	importFavs(efavfpath, appState.favs, appState.bgs, appState.numBgs);
	loadSettings(efavfpath, &settings);
	if(appState.nsfwIndex < 2){
		printf("!!!!! NSFW images Loaded, NSFW enabled !!!!!\n");
		settings.nsfw = 1;
		saveSettings(efavfpath, &settings);
	}

	//inventory for tray (exclude bgs[0] launch snapshot from SFW count)
	g_sfwCount = (appState.nsfwIndex > 1) ? (appState.nsfwIndex - 1) : 0;
	g_nsfwCount = (appState.numBgs > appState.nsfwIndex) ? (appState.numBgs - appState.nsfwIndex) : 0;
	printf("Counts SFW:%d NSFW:%d\n", g_sfwCount, g_nsfwCount);

	HWND hwnd = InitializeHiddenWindow();
	UpdateTrayTip(hwnd);

	UINT timerInterval = approx_minutes * MS_PER_MIN;

	RegisterAppHotkeys();

	UINT_PTR timerId = 0;
	UINT_PTR holdTimer = 0;
	int holdDir = HOLD_NONE;
	if (!settings.loop_pause) {
		timerId = SetTimer(NULL, TIMER_MAIN, timerInterval, NULL);
	}
	MSG msg = {0};

	while(running && GetMessage(&msg, NULL, 0, 0) > 0) {
		if (msg.message == WM_TIMER && holdTimer && msg.wParam == holdTimer) {
			//hold-to-cycle: keep going while combo is down, otherwise peaces out
			int stillHeld = 0;
			if(holdDir == HOLD_NEXT) stillHeld = isNextHeld();
			else if(holdDir == HOLD_PREV) stillHeld = isPrevHeld();

			if(stillHeld) {
				if(holdDir == HOLD_NEXT) {
					bumpAutoTimer(&timerId, timerInterval);
					AdvanceBackground(&appState);
				} else if(holdDir == HOLD_PREV && appState.prevInd >= 0) {
					bumpAutoTimer(&timerId, timerInterval);
					PreviousBackground(&appState);
				}
				//crank up to the faster repeat rate after the initial delay
				KillTimer(NULL, holdTimer);
				holdTimer = SetTimer(NULL, TIMER_HOLD, HOLD_REPEAT_MS, NULL);
			} else {
				stopHoldCycle(&holdTimer, &holdDir);
			}
		} else if (msg.message == WM_TIMER && timerId && msg.wParam == timerId) {
			AdvanceBackground(&appState);
		} else if (msg.message == WM_HOTKEY) {
			HandleHotkey((int)msg.wParam, &appState, hwnd, efavfpath, &timerId, timerInterval, &running,
				&holdTimer, &holdDir);
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	stopHoldCycle(&holdTimer, &holdDir);
	if (timerId) KillTimer(NULL, timerId);
	UnregisterAppHotkeys();

	RemoveTrayIcon(hwnd);
	DestroyWindow(hwnd);
	
	if (appState.bgs) {
		for (int i = 0; i < appState.numBgs; i++) {
			if (appState.bgs[i]) xfree(appState.bgs[i]);
		}
		xfree(appState.bgs);
	}
	if (appState.favs) xfree(appState.favs);
	CloseHandle(hMutex);
	return 0;
}
