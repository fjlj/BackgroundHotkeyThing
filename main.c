#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define MAX_HISTORY 50
#define MAX_iSTACK_SIZE 10
#define SystemTimePointer ((_KSYSTEM_TIME*)0x7FFE0014)
#define IDI_APP_ICON 101
#define TOAST_DURATION_MS 200

#define APP_NAME "BackgroundHotkeyThing"
#define APP_CLASS "BgHotkeyMsgWindow"
#define INI_FILENAME "BackgroundHotkeyThing.ini"
#define INI_SEC_SETTINGS "Settings"
#define INI_SEC_FAVS "Favs"
#define TIMER_MAIN 1
#define TIMER_TOAST 2
#define DEFAULT_NSFW_INDEX 4000
#define MS_PER_MIN 60000

//#define DEBUG

#ifndef DEBUG
	#define printf(...) 
#endif

typedef enum {
	HK_QUIT = 1,
	HK_TOGGLE_ICONS,
	HK_SAVE_FAV,
	HK_LOAD_FAV,
	HK_SAVE_SETTINGS,
	HK_LOAD_SETTINGS,
	HK_NEXT_BG,
	HK_PREV_BG,
	HK_PAUSE,
	HK_TOGGLE_NSFW,
	HK_CYCLE_FAVS,
	HK_EXPORT_FAVS,
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

//struct to get system time from KUSER_SHARED_DATA pointer
typedef struct {
	ULONG LowPart;
	LONG High1Time;
	LONG High2Time;
} _KSYSTEM_TIME;

//integer stack struct
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
	int prev[MAX_HISTORY];
	int prevInd;
	intStack* favs;
} AppState;

//get all the paths in a folder that match a pattern
int ListDirectoryContents(const char *sDir, char*** bgs_ptr, int* capacity, const char* ext, int *nsfwInd) {
	WIN32_FIND_DATA fdFile;
	HANDLE hFind = NULL;
	char sPath[MAX_PATH] = {0};
	char NSFWpath[MAX_PATH] = {0};

	int ind = 1;
	//Specify a file mask. *.* = We want everything!
	sprintf_s(sPath,MAX_PATH, "%s\\%s", sDir,"*.*");
	sprintf_s(NSFWpath,MAX_PATH, "%s\\NSFW\\%s", sDir,"*.*");

	if((hFind = FindFirstFile(sPath, &fdFile)) == INVALID_HANDLE_VALUE) {
		return ind;
	}

	do {
		if(!(fdFile.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && 
		   strcmp(fdFile.cFileName, ".") != 0  && 
		   strcmp(fdFile.cFileName, "..") != 0 && 
		   strstr(ext,strrchr(fdFile.cFileName,'.')) != NULL) {
		   	
			if (ind >= *capacity) {
				size_t new_capacity = *capacity * 2;
				char** new_bgs = realloc(*bgs_ptr, new_capacity * sizeof(char*));
				if (!new_bgs) return ind; // Handle out of memory gracefully
				*capacity = new_capacity;
				*bgs_ptr = new_bgs;
			}

			sprintf_s(sPath,MAX_PATH, "%s\\%s", sDir, fdFile.cFileName);
			size_t slen = strlen(sPath)+1;
			(*bgs_ptr)[ind] = malloc(slen);
			memcpy((*bgs_ptr)[ind], sPath, slen);
			printf("File: %d:%s\n", ind, (*bgs_ptr)[ind]);
			ind++;
		}
	} while(FindNextFile(hFind, &fdFile)); //Find the next file.

	FindClose(hFind); //Always, Always, clean things up!
	*nsfwInd = ind;
	
	if((hFind = FindFirstFile(NSFWpath, &fdFile)) == INVALID_HANDLE_VALUE) {
		return ind;
	}

	do {
		if(!(fdFile.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
		   strcmp(fdFile.cFileName, ".") != 0 && 
		   strcmp(fdFile.cFileName, "..") != 0 && 
		   strstr(ext,strrchr(fdFile.cFileName,'.')) != NULL) {
		   	
			if (ind >= *capacity) {
				*capacity *= 2;
				char** new_bgs = realloc(*bgs_ptr, *capacity * sizeof(char*));
				if (!new_bgs) return ind;
				*bgs_ptr = new_bgs;
			}

			sprintf_s(NSFWpath,MAX_PATH, "%s\\NSFW\\%s", sDir, fdFile.cFileName);
			size_t slen = strlen(NSFWpath)+1;
			(*bgs_ptr)[ind] = malloc(slen);
			memcpy((*bgs_ptr)[ind], NSFWpath, slen);
			printf("File: %d:%s\n", ind, (*bgs_ptr)[ind]);
			ind++;
		}
	} while(FindNextFile(hFind, &fdFile)); //Find the next file.

	FindClose(hFind); //Always, Always, clean things up!

	return ind;
}

//find the desktop window for message handling (hide/show desktop icons)
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

//increment top of stack add entry to top of stack
//if we would go out of bounds.. shift the data down by 1 index and write it to the top.
//also returns the data written... (not really needed but eh why not)
int pushIntStack(intStack* stack, int data){
	
	stack->top++;
	if(stack->top >= MAX_iSTACK_SIZE){
		stack->top = MAX_iSTACK_SIZE - 1;
		memmove(stack->inds,&stack->inds[1],(sizeof(int)*(MAX_iSTACK_SIZE-1)));
		stack->inds[stack->top] = data;
		stack->pointer = stack->top;
		return data;
	}
	
	stack->inds[stack->top] = data;
	stack->pointer = stack->top;
	return data;
}

//remove top entry from stack and return its value (not technically removed
//decrements the top index.
int popIntStack(intStack* stack){
	if(stack->top <= -1){ stack->top = -1; return 0; }
	int data = stack->inds[stack->top];
	if(--stack->top < 0) stack->top = 0; 
	stack->pointer = stack->top;
	return data;
}

//moves the "cursor" for the current "selected" position of the stack
//used to iterate through loading favorites 
//rotation is last to first entry
//returns the current position and moves it down 1
int peekIntStackItr(intStack* stack){
	if(stack->top <= -1) { stack->top = -1; return 0; }
	int data = stack->inds[stack->pointer];
	if(--stack->pointer < 0)
		stack->pointer = (stack->top < 0 ? 0 : stack->top);
	return data;
}

//used for debugging (prints all current favorites)
void printFavs(intStack* favs,char **bgs){
	printf("------Current Favorites------\n");
	for(int i = 0; i <= favs->top; i++){
		printf("fav[%d]:%d = %s\n",i,favs->inds[i],bgs[favs->inds[i]]);
	}
}

void exportFavs(char* efavfpath,intStack* favs, char **bgs){
	char slotName[16] = {0};
	for(int i = 0; i <= favs->top; i++){
		sprintf_s(slotName,16,"Fav-%d",i);
		WritePrivateProfileStringA(
			INI_SEC_FAVS,
			slotName,
			bgs[favs->inds[i]],
			efavfpath);
	}  
}

void saveSettings(char* efavfpath, AppSettings* settings) {
	char ival[8] = {0};
	sprintf_s(ival, 8, "%d", settings->onlyFavs);
	WritePrivateProfileStringA(INI_SEC_SETTINGS, "Set-0", ival, efavfpath);
	sprintf_s(ival, 8, "%d", settings->nsfw);
	WritePrivateProfileStringA(INI_SEC_SETTINGS, "Set-1", ival, efavfpath);
	sprintf_s(ival, 8, "%d", settings->loop_pause);
	WritePrivateProfileStringA(INI_SEC_SETTINGS, "Set-2", ival, efavfpath);
	sprintf_s(ival, 8, "%d", settings->notifications);
	WritePrivateProfileStringA(INI_SEC_SETTINGS, "Set-3", ival, efavfpath);
}

void loadSettings(char* efavfpath, AppSettings* settings) {
	settings->onlyFavs = GetPrivateProfileIntA(INI_SEC_SETTINGS, "Set-0", settings->onlyFavs, efavfpath);
	settings->nsfw = GetPrivateProfileIntA(INI_SEC_SETTINGS, "Set-1", settings->nsfw, efavfpath);
	settings->loop_pause = GetPrivateProfileIntA(INI_SEC_SETTINGS, "Set-2", settings->loop_pause, efavfpath);
	settings->notifications = GetPrivateProfileIntA(INI_SEC_SETTINGS, "Set-3", settings->notifications, efavfpath);
}

void importFavs(char* efavfpath, intStack* favs,char* bgs[],int numBgs){
	char favPath[MAX_PATH] = {0x00};
	char slotName[16] = {0};
	for(int i = 0; i <= MAX_iSTACK_SIZE; i++){
		
		sprintf_s(slotName,16,"Fav-%d",i);
		
		if(!GetPrivateProfileStringA(
	  		INI_SEC_FAVS,
		    slotName,
		    "",
		    favPath,
		    MAX_PATH,
		    efavfpath)
		) break;
	    
	    for(int o = 0; o < numBgs; o++){
			if(strcmp(bgs[o], favPath) == 0){
				pushIntStack(favs,o);
			}
		} 	
	}	    
}

int nextFav(char** bgs, intStack* favs, int nsfw) {
	int favsp = favs->pointer;
	int favSlot = peekIntStackItr(favs);
	if(!strstr(bgs[favSlot],"NSFW") || nsfw > 0){
		printf("load fav[%d] = %d - bg: %s\n",favsp,favSlot,bgs[favSlot]);
		SystemParametersInfo(SPI_SETDESKWALLPAPER,0,bgs[favSlot],SPIF_SENDCHANGE);
		return favSlot;
	}
	printf("Not Loading NSFW favorite while NSFW mode disabled\n");
	return -1;
}

int initBGs(char* relpath, AppState* appState, char* orgPaper){
	//get the current BG and set it as the first in history
	SystemParametersInfo(SPI_GETDESKWALLPAPER,MAX_PATH,orgPaper,0);
	
	int capacity = 1000; // Start with capacity for 1000 backgrounds
	appState->bgs = malloc(capacity * sizeof(char*));
	if (!appState->bgs) return 0;

	size_t ogPathLen = strlen(orgPaper)+1;
	appState->bgs[0] = malloc(ogPathLen);
	memcpy(appState->bgs[0], orgPaper, ogPathLen);

	//populate the paths and index arrays while getting the number of pngs
	int numBgs = ListDirectoryContents(relpath, &(appState->bgs), &capacity, "*.png;*.jpg;*.bmp", &(appState->nsfwIndex));

	printf("%d Backgrounds Loaded.\nNSFW Begins at:%d\n",numBgs,appState->nsfwIndex);

	// If numBgs is 1, it means we only found the original wallpaper and no new files
	if(numBgs <= 1) {
		char errmsg[MAX_PATH+32];
		sprintf_s(errmsg,MAX_PATH+32,"Path or Images not found at: [%s]\n",relpath);
		MessageBoxA(0,errmsg,"Whoops!", 0);
		return 0;
	}
	
	return numBgs;
}

#define WM_TRAYICON (WM_APP + 1)
#define TRAY_ICON_ID 1

AppSettings settings = {0};

void AdvanceFavorite(AppState* state) {
	int nextFfavs = nextFav(state->bgs, state->favs, settings.nsfw);
	if(nextFfavs != -1){
		state->curbg = nextFfavs;
		if(++state->prevInd%MAX_HISTORY == 0) state->prevInd++;
		if(state->prevInd >= MAX_HISTORY) state->prevInd = 1;
		state->prev[state->prevInd%MAX_HISTORY] = state->curbg;
	}
}

void AdvanceBackground(AppState* state) {
	if(settings.onlyFavs){
		AdvanceFavorite(state);
	} else {
		int nextBg = 0;
		if(!settings.nsfw) {
			nextBg = (rand()%(state->nsfwIndex-1))+2;
		} else {
			nextBg = settings.nsfw == 2 ? (rand()%(state->numBgs-(state->nsfwIndex+1)))+state->nsfwIndex : (rand()%(state->numBgs-1))+1;
		}
		if(++state->prevInd%MAX_HISTORY == 0) state->prevInd++;
		if(state->prevInd >= MAX_HISTORY) state->prevInd = 1;
		state->curbg = nextBg;
		state->prev[state->prevInd%MAX_HISTORY] = state->curbg;
		printf("Setting:[%d]%s\n", state->curbg, state->bgs[state->curbg]);
		SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, state->bgs[state->curbg], SPIF_SENDCHANGE);
	}
}

void PreviousBackground(AppState* state) {
	if(state->prevInd <= 0) return;
	if(--state->prevInd < 0) state->prevInd = 0;
	if(state->prevInd == MAX_HISTORY) state->prevInd--;
	state->curbg = state->prev[state->prevInd%MAX_HISTORY];
	printf("Setting:[%d]%s\n", state->curbg, state->bgs[state->curbg]);
	SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, state->bgs[state->curbg], SPIF_SENDCHANGE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch(msg) {
		case WM_TRAYICON:
			// Handle Left and Right click on the tray icon
			if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
				POINT pt;
				GetCursorPos(&pt);
				HMENU hMenu = CreatePopupMenu();
				AppendMenuA(hMenu, MF_STRING, HK_NEXT_BG, "Next Background\tWin+Shift-N");
				AppendMenuA(hMenu, MF_STRING, HK_PREV_BG, "Previous Background\tWin+Shift-B");
				AppendMenuA(hMenu, MF_STRING | (settings.loop_pause ? MF_CHECKED : MF_UNCHECKED), HK_PAUSE, "Toggle Pause\tWin+Alt-V");
				
				char nsfwMenuText[64];
				UINT nsfwState = MF_UNCHECKED;
				if (settings.nsfw == 0) {
					strcpy_s(nsfwMenuText, sizeof(nsfwMenuText), "NSFW Mode: Off\tWin+Shift-H");
				} else if (settings.nsfw == 1) {
					strcpy_s(nsfwMenuText, sizeof(nsfwMenuText), "[-] NSFW Mode: Combined\tWin+Shift-H");
				} else {
					strcpy_s(nsfwMenuText, sizeof(nsfwMenuText), "NSFW Mode: Only NSFW\tWin+Shift-H");
					nsfwState = MF_CHECKED;
				}
				AppendMenuA(hMenu, MF_STRING | nsfwState, HK_TOGGLE_NSFW, nsfwMenuText);
				
				AppendMenuA(hMenu, MF_STRING | (settings.onlyFavs ? MF_CHECKED : MF_UNCHECKED), HK_CYCLE_FAVS, "Toggle Cycle Favs\tWin+Shift-L");
				AppendMenuA(hMenu, MF_STRING | (settings.notifications ? MF_CHECKED : MF_UNCHECKED), HK_TOGGLE_NOTIF, "Toggle Notifications\tWin+Alt-N");
				AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
				AppendMenuA(hMenu, MF_STRING, HK_SAVE_FAV, "Save Favorite\tWin+Shift-A");
				AppendMenuA(hMenu, MF_STRING, HK_CLEAR_FAVS, "Clear Favorites\tWin+Shift-C");
				AppendMenuA(hMenu, MF_STRING, HK_EXPORT_FAVS, "Export Favorites\tWin+Shift-E");
				AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
				AppendMenuA(hMenu, MF_STRING, HK_SAVE_SETTINGS, "Save Settings\tWin+Alt-S");
				AppendMenuA(hMenu, MF_STRING, HK_LOAD_SETTINGS, "Load Settings\tWin+Alt-L");
				AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
				AppendMenuA(hMenu, MF_STRING, HK_TOGGLE_ICONS, "Toggle Desktop Icons\tWin+Shift-Z");
				AppendMenuA(hMenu, MF_STRING, HK_OPEN_EXPLORER, "Open in Explorer\tWin+Shift-O");
				AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
				AppendMenuA(hMenu, MF_STRING, HK_QUIT, "Quit\tWin+Alt-Q");
				
				SetForegroundWindow(hwnd); // Required to make menu disappear if you click away
				int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
				PostMessage(hwnd, WM_NULL, 0, 0); // Windows being windows...
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
	strcpy_s(nid.szTip, sizeof(nid.szTip), APP_NAME);
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
	strcpy_s(nid.szInfoTitle, sizeof(nid.szInfoTitle), title);
	strcpy_s(nid.szInfo, sizeof(nid.szInfo), message);
	nid.dwInfoFlags = NIIF_NOSOUND;
	nid.uTimeout = timeoutMs;
	Shell_NotifyIconA(NIM_MODIFY, &nid);

	if (timeoutMs > 0) {
		SetTimer(hwnd, TIMER_TOAST, timeoutMs, NULL);
	} else {
		KillTimer(hwnd, TIMER_TOAST);
	}
}

void HandleHotkey(int hotkeyId, AppState* appState, HWND hwnd, char* efavfpath, UINT_PTR* timerId, UINT timerInterval, int* running) {
	switch (hotkeyId) {
		case HK_QUIT:
			*running = 0;
			break;
		case HK_TOGGLE_ICONS:
			{
				HWND hShellViewWin = gethShellViewWin();
				if(hShellViewWin) SendMessage(hShellViewWin,0x0111, 0x7402, 0);
			}
			break;
		case HK_SAVE_FAV:
			pushIntStack(appState->favs, appState->curbg);
			printFavs(appState->favs, appState->bgs);
			ShowNotification(hwnd, "Favorites", "Saved current background to favorites!", TOAST_DURATION_MS);
			break;
		case HK_LOAD_FAV:
			AdvanceFavorite(appState);
			break;
		case HK_SAVE_SETTINGS:
			saveSettings(efavfpath, &settings);
			break;
		case HK_LOAD_SETTINGS:
			loadSettings(efavfpath, &settings);
			if (*timerId) KillTimer(NULL, *timerId);
			*timerId = 0;
			if (!settings.loop_pause) {
				*timerId = SetTimer(NULL, TIMER_MAIN, timerInterval, NULL);
			}
			break;
		case HK_NEXT_BG:
			if (!settings.loop_pause) {
				if (*timerId) KillTimer(NULL, *timerId);
				*timerId = SetTimer(NULL, TIMER_MAIN, timerInterval, NULL);
			}
			AdvanceBackground(appState);
			break;
		case HK_PREV_BG:
			if(appState->prevInd > 0) {
				if (!settings.loop_pause) {
					if (*timerId) KillTimer(NULL, *timerId);
					*timerId = SetTimer(NULL, TIMER_MAIN, timerInterval, NULL);
				}
				PreviousBackground(appState);
			}
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
			break;
		case HK_TOGGLE_NSFW:
			settings.nsfw = ++settings.nsfw % 3;
			printf("NSFW:%d\n", settings.nsfw);
			if (settings.nsfw == 0) ShowNotification(hwnd, "NSFW Mode", "Off", TOAST_DURATION_MS);
			else if (settings.nsfw == 1) ShowNotification(hwnd, "NSFW Mode", "Combined", TOAST_DURATION_MS);
			else ShowNotification(hwnd, "NSFW Mode", "Only NSFW", TOAST_DURATION_MS);
			break;
		case HK_CYCLE_FAVS:
			settings.onlyFavs ^= 1;
			printf("Cycle:%s\n", (settings.onlyFavs ? "Only Favorites" : "Normal"));
			ShowNotification(hwnd, "Cycle Mode", settings.onlyFavs ? "Only Favorites" : "Normal", TOAST_DURATION_MS);
			break;
		case HK_EXPORT_FAVS:
			printf("Exporting %d Favorites\n",appState->favs->top);
			exportFavs(efavfpath,appState->favs,appState->bgs);
			break;
		case HK_CLEAR_FAVS:
			printf("Clearing Favorites\n");
			WritePrivateProfileStringA(
				INI_SEC_FAVS,
				NULL,
				NULL,
				efavfpath);
			appState->favs->top = -1;
			appState->favs->pointer = 0;
			memset(appState->favs->inds, 0, sizeof(int) * MAX_iSTACK_SIZE);
			ShowNotification(hwnd, "Favorites", "Cleared all favorites!", TOAST_DURATION_MS);
			break;
		case HK_OPEN_EXPLORER:
			{
				char args[MAX_PATH + 32] = {0};
				sprintf_s(args, sizeof(args), "/select,\"%s\"", appState->bgs[appState->curbg]);
				ShellExecuteA(NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
			}
			break;
		case HK_TOGGLE_NOTIF:
			settings.notifications ^= 1;
			if (settings.notifications) {
				ShowNotification(hwnd, APP_NAME, "Notifications Enabled", TOAST_DURATION_MS);
			}
			break;
	}
}

void RegisterAppHotkeys() {
	char hkErrors[1024] = {0};

	if(!RegisterHotKey(NULL, HK_QUIT, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'Q')) strcat(hkErrors, "- Win+Alt-Q (Quit)\n");
	if(!RegisterHotKey(NULL, HK_TOGGLE_ICONS, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'Z')) strcat(hkErrors, "- Win+Shift-Z (Toggle Icons)\n");
	if(!RegisterHotKey(NULL, HK_SAVE_FAV, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'A')) strcat(hkErrors, "- Win+Shift-A (Save Fav)\n");
	if(!RegisterHotKey(NULL, HK_LOAD_FAV, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'F')) strcat(hkErrors, "- Win+Shift-F (Load Fav)\n");
	if(!RegisterHotKey(NULL, HK_SAVE_SETTINGS, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'S')) strcat(hkErrors, "- Win+Alt-S (Save Settings)\n");
	if(!RegisterHotKey(NULL, HK_LOAD_SETTINGS, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'L')) strcat(hkErrors, "- Win+Alt-L (Load Settings)\n");
	if(!RegisterHotKey(NULL, HK_NEXT_BG, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'N')) strcat(hkErrors, "- Win+Shift-N (Next BG)\n");
	if(!RegisterHotKey(NULL, HK_PREV_BG, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'B')) strcat(hkErrors, "- Win+Shift-B (Prev BG)\n");
	if(!RegisterHotKey(NULL, HK_PAUSE, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'V')) strcat(hkErrors, "- Win+Alt-V (Pause)\n");
	if(!RegisterHotKey(NULL, HK_TOGGLE_NSFW, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'H')) strcat(hkErrors, "- Win+Shift-H (Toggle NSFW)\n");
	if(!RegisterHotKey(NULL, HK_CYCLE_FAVS, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'L')) strcat(hkErrors, "- Win+Shift-L (Cycle Favs)\n");
	if(!RegisterHotKey(NULL, HK_EXPORT_FAVS, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'E')) strcat(hkErrors, "- Win+Shift-E (Export Favs)\n");
	if(!RegisterHotKey(NULL, HK_CLEAR_FAVS, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'C')) strcat(hkErrors, "- Win+Shift-C (Clear Favs)\n");
	if(!RegisterHotKey(NULL, HK_OPEN_EXPLORER, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'O')) strcat(hkErrors, "- Win+Shift-O (Open Explorer)\n");
	if(!RegisterHotKey(NULL, HK_TOGGLE_NOTIF, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'N')) strcat(hkErrors, "- Win+Alt-N (Toggle Notifications)\n");

	if (hkErrors[0] != '\0') {
		char errorMsg[2048] = {0};
		sprintf_s(errorMsg, 2048, "The following hotkeys failed to register (they might be in use by Windows or another app):\n\n%s", hkErrors);
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
	HWND hwnd = CreateWindowA(wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL); // Create hidden window
	InitTrayIcon(hwnd);
	return hwnd;
}

int main(int argc, char *argv[]) {
	_KSYSTEM_TIME st;
	AppState appState = {0};
	appState.nsfwIndex = DEFAULT_NSFW_INDEX;
	appState.prev[0] = 0;
	appState.favs = malloc(sizeof(intStack));
	
	if (!appState.favs) {
		MessageBoxA(0, "Failed to allocate memory.", APP_NAME, MB_ICONERROR | MB_OK);
		return 1;
	}
	
	memset(appState.favs->inds, 0, sizeof(int)*MAX_iSTACK_SIZE);
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
		MessageBoxA(0,"Usage: BackgroundHotkeyThing.exe <path to BG images> <rotation delay in seconds>\nNOTE: single folder(non-recursive)\n","Woops",0);
		return 0;
	}
	
	if(argc > 2) {
		int tmp = atoi(argv[2]);
		approx_minutes = (tmp > 0  ? tmp : approx_minutes);
	}
	
	char relpath[MAX_PATH] = {0};
	memcpy(relpath,argv[1],strlen(argv[1]));
	char exepath[MAX_PATH] = {0};
	memcpy(exepath,argv[0],strlen(argv[0]));

	//detect weather its an absolute or relative path and set accordingly
	if(argv[1][0] == '.' || argv[1][1] != ':') {
		memset(&relpath[0],0x00,MAX_PATH);
		char* exenameBegin = strrchr(exepath,(int)'\\');
		*exenameBegin = 0x00;
		sprintf_s(relpath,MAX_PATH,"%s\\%s",exepath,argv[1]);
	}
	
	//set savepath
	char efavfpath[MAX_PATH] = {0};
	sprintf_s(efavfpath,MAX_PATH,"%s\\%s",relpath,INI_FILENAME);
	
	//do some very basic random seeding via some ASLR and system time values from KUSER_SHARED_DATA...
	memcpy(&st,SystemTimePointer,sizeof(st));
	srand((unsigned int)((uintptr_t)&main + (uintptr_t)&ListDirectoryContents) + st.LowPart);
	
	appState.numBgs = initBGs(relpath, &appState, orgPaper);
	
	if(appState.numBgs == 0) {
		if (appState.bgs) {
			if (appState.bgs[0]) free(appState.bgs[0]);
			free(appState.bgs);
		}
		if(appState.favs) free(appState.favs);
		return 1;
	}
	
	importFavs(efavfpath, appState.favs, appState.bgs, appState.numBgs);
	loadSettings(efavfpath, &settings);
	if(appState.nsfwIndex < 2){
		printf("!!!!! NSFW images Loaded, NSFW enabled !!!!!\n");
		settings.nsfw = 1;
	}

	HWND hwnd = InitializeHiddenWindow();

	UINT timerInterval = approx_minutes * MS_PER_MIN;

	RegisterAppHotkeys();

	UINT_PTR timerId = 0;
	if (!settings.loop_pause) {
		timerId = SetTimer(NULL, TIMER_MAIN, timerInterval, NULL);
	}
	MSG msg = {0};

	while(running && GetMessage(&msg, NULL, 0, 0) > 0) {
		if (msg.message == WM_TIMER && msg.wParam == timerId) {
			AdvanceBackground(&appState);
		} else if (msg.message == WM_HOTKEY) {
			HandleHotkey((int)msg.wParam, &appState, hwnd, efavfpath, &timerId, timerInterval, &running);
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	if (timerId) KillTimer(NULL, timerId);
	UnregisterAppHotkeys();

	RemoveTrayIcon(hwnd);
	DestroyWindow(hwnd);
	
	if (appState.bgs) {
		for (int i = 0; i < appState.numBgs; i++) {
			if (appState.bgs[i]) free(appState.bgs[i]);
		}
		free(appState.bgs);
	}
	if (appState.favs) free(appState.favs);
	return 0;
}
