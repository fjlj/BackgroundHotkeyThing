#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define MAX_BGS 10000
#define MAX_HISTORY 50
#define MAX_iSTACK_SIZE 10
#define SystemTimePointer ((_KSYSTEM_TIME*)0x7FFE0014)

//#define DEBUG

#ifndef DEBUG
	#define printf(...) 
#endif

typedef enum {
	keys_WIN = 1,
	keys_SHIFT = 2,
	keys_ALT = 4,
	keys_S = 8,
	keys_F = 16,
	keys_Q = 32,
	keys_B = 64,
	keys_V = 128,
	keys_N = 256,
	keys_H = 512,
	keys_L = 1024,
	keys_Z = 2048,
	keys_E = 4096,
	keys_C = 8192
} Keys;

typedef enum {
	onlyFavs   = 0,
	nsfw       = 1,
	loop_pause = 2
} Settings;

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

//get all the paths in a folder that match a pattern
int ListDirectoryContents(const char *sDir,char* storage, char* indexes[], int psize, const char* ext, int *nsfwInd) {
	WIN32_FIND_DATA fdFile;
	HANDLE hFind = NULL;
	char sPath[MAX_PATH] = {0};
	char NSFWpath[MAX_PATH] = {0};

	int ind = 1;
	//Specify a file mask. *.* = We want everything!
	sprintf_s(sPath,MAX_PATH, "%s\\%s", sDir,"*.*");
	sprintf_s(NSFWpath,MAX_PATH, "%s\\NSFW\\%s", sDir,"*.*");

	if((hFind = FindFirstFile(sPath, &fdFile)) == INVALID_HANDLE_VALUE) {
		return 0;
	}

	do {
		if(!(fdFile.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && 
		   strcmp(fdFile.cFileName, ".") != 0  && 
		   strcmp(fdFile.cFileName, "..") != 0 && 
		   strstr(ext,strrchr(fdFile.cFileName,'.')) != NULL) {
		   	
			sprintf_s(sPath,MAX_PATH, "%s\\%s\x00", sDir, fdFile.cFileName);
			size_t slen = strlen(sPath)+1;
			memcpy(storage,sPath,slen);
			indexes[ind++] = storage;
			storage += slen;
			printf("File: %d:%s\n", ind-1,indexes[ind-1]);
		}
	} while(FindNextFile(hFind, &fdFile) && ind < psize-1); //Find the next file.

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
		   	
			sprintf_s(NSFWpath,MAX_PATH, "%s\\NSFW\\%s\x00", sDir, fdFile.cFileName);
			size_t slen = strlen(NSFWpath)+1;
			memcpy(storage,NSFWpath,slen);
			indexes[ind++] = storage;
			storage += slen;
			printf("File: %d:%s\n", ind-1,indexes[ind-1]);
		}
	} while(FindNextFile(hFind, &fdFile) && ind < psize-1); //Find the next file.

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
			"Favs",
			slotName,
			bgs[favs->inds[i]],
			efavfpath);
	}  
}

void saveSettings(char* efavfpath, int* settings[]){
	char slotName[16] = {0};
	for(int i = 0; i < 3; ++i){
		char ival[8] = {0};
		itoa((*settings)[i],ival,10);
		sprintf_s(slotName,16,"Set-%d",i);
		WritePrivateProfileStringA(
			"Settings",
			slotName,
			ival,
			efavfpath);
	}  
}

void loadSettings(char* efavfpath, int* settings[]){
	char slotName[16] = {0};
	for(int i = 0; i < 3; ++i){
		char ival[8] = {0};
		sprintf_s(slotName,16,"Set-%d",i);
		if(!GetPrivateProfileStringA(
	  		"Settings",
		    slotName,
		    "",
		    ival,
		    7,
		    efavfpath)
		) break;
	    (*settings)[i] = (int)atoi(ival);		
	}  
}

void importFavs(char* efavfpath, intStack* favs,char* bgs[],int numBgs){
	char favPath[MAX_PATH] = {0x00};
	char slotName[16] = {0};
	for(int i = 0; i <= MAX_iSTACK_SIZE; i++){
		
		sprintf_s(slotName,16,"Fav-%d",i);
		
		if(!GetPrivateProfileStringA(
	  		"Favs",
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

int checkHotkey(int key_s, int* debounce){
	if(*debounce) return 0;
	int pressed = 0;
	int alt = 0;
	int shift = 0;
	int win = 0;
	 
		win   |= (GetAsyncKeyState(VK_LWIN) & 0x8000  || GetAsyncKeyState(VK_RWIN) & 0x8000) ? 1 : 0;
		alt   |= (GetAsyncKeyState(VK_RMENU) & 0x8000 || GetAsyncKeyState(VK_LMENU) & 0x8000) ? 1 : 0;
		shift |= (GetAsyncKeyState(VK_LSHIFT) & 0x8000 || GetAsyncKeyState(VK_RSHIFT) & 0x8000 ) ? 1 : 0; 
	
		
	if(win && !shift && !alt) {
		printf("WIN\n");
		if(key_s & keys_Q)
			pressed |= GetAsyncKeyState('Q') < 0 ? 1 : 0;
		if(key_s & keys_Z)
			pressed |= GetAsyncKeyState('Z') < 0 ? 1 : 0;
		if(key_s & keys_S)
			pressed |= GetAsyncKeyState('S') < 0 ? 1 : 0;
		if(key_s & keys_F)
			pressed |= GetAsyncKeyState('F') < 0 ? 1 : 0;	
	}
	
	if(key_s & keys_ALT && win && alt && !shift) {
		printf("WIN+ALT\n");
		if(key_s & keys_S)
			pressed |= GetAsyncKeyState('S') < 0 ? 1 : 0;
		if(key_s & keys_L)
			pressed |= GetAsyncKeyState('L') < 0 ? 1 : 0;
	}
		
    if(key_s & keys_SHIFT && win && shift && !alt) {
    	printf("WIN + SHIFT\n");
		if(key_s & keys_N)
			pressed |= GetAsyncKeyState('N') < 0 ? 1 : 0;
		if(key_s & keys_B)
			pressed |= GetAsyncKeyState('B') < 0 ? 1 : 0;
		if(key_s & keys_V)
			pressed |= GetAsyncKeyState('V') < 0 ? 1 : 0;
		if(key_s & keys_H)
			pressed |= GetAsyncKeyState('H') < 0 ? 1 : 0; 
		if(key_s & keys_L)
			pressed |= GetAsyncKeyState('L') < 0 ? 1 : 0;
		if(key_s & keys_E)
			pressed |= GetAsyncKeyState('E') < 0 ? 1 : 0;
		if(key_s & keys_C)
			pressed |= GetAsyncKeyState('C') < 0 ? 1 : 0;
	}
	*debounce = pressed*4;
	
	return pressed;
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

int initBGs(char* relpath, char** bgs, char* bgPaths, char* orgPaper, int* nsfwIndex){

	//get the current BG and set it as the first in history
	SystemParametersInfo(SPI_GETDESKWALLPAPER,MAX_PATH,orgPaper,0);
	size_t ogPathLen = strlen(orgPaper)+1;
	memcpy(bgPaths,orgPaper,ogPathLen);
	
	bgs[0] = bgPaths;
	bgPaths += ogPathLen;

	//populate the paths and index arrays while getting the number of pngs
	int numBgs = ListDirectoryContents(relpath,bgPaths,&bgs[1],MAX_BGS,"*.png;*.jpg;*.bmp",nsfwIndex);

	printf("%d Backgrounds Loaded.\nNSFW Begins at:%d\n",numBgs,*nsfwIndex);

	if(numBgs == 0 ) {
		char errmsg[MAX_PATH+32];
		sprintf_s(errmsg,MAX_PATH+32,"Path or Images not found at: [%s]\n",relpath);
		MessageBoxA(0,errmsg,"Whoops!", 0);
		return 0;
	}
	
	return numBgs;
}

int main(int argc, char *argv[]) {
	_KSYSTEM_TIME st;
	char* bgPaths = malloc((MAX_PATH*MAX_BGS)+MAX_BGS+1);
	char** bgs = malloc(MAX_BGS*sizeof(char*));
	int* settings = malloc(3*sizeof(int));
	char orgPaper[MAX_PATH] = {0x00};
	int nsfwIndex = 4000;
	int running = 1;
	int approx_minutes = 2;
	int prevInd = 0;
	int curbg = 0;
	int loops = 0;
	settings[loop_pause] = 0;
	settings[nsfw] = 0;
	settings[onlyFavs] = 0;
	int prev[MAX_HISTORY] = {0x00};
	prev[0] = 0;
	intStack* favs = malloc(sizeof(intStack));
	memset(favs->inds,0,sizeof(int)*MAX_iSTACK_SIZE);
	favs->top = -1;
	favs->pointer = 0;

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
	sprintf_s(efavfpath,MAX_PATH,"%s\\BackgroundHotkeyThing.ini",relpath);
	
	//do some very basic random seeding via some ASLR and system time values from KUSER_SHARED_DATA...
	memcpy(&st,SystemTimePointer,sizeof(st));
	srand((unsigned int)((uintptr_t)&main + (uintptr_t)&ListDirectoryContents) + st.LowPart);
	
	int numBgs = initBGs(relpath,bgs,bgPaths,orgPaper,&nsfwIndex);
	
	importFavs(efavfpath,favs,bgs,numBgs);
	loadSettings(efavfpath,&settings);
	if(nsfwIndex < 2){
		printf("!!!!! Only NSFW images Loaded, NSFW enabled !!!!!\n");
		settings[nsfw] = 1;
	}
	//get a handle to the desktop to recieve show/hide icon messages
	HWND hShellViewWin = gethShellViewWin();

	//60 seconds in a min, 1000ms in a second = 60000ms/min
	//sleep is 75ms (estimate instructions at 10ms) = 85ms
	//60000 / 85 = 705
	//round to 700 because OCD
	int approxMinToms = approx_minutes * 700;
	int nextBg = 0;
	int something_pressed = 0;

	while(running) {
		if(something_pressed > 0) 
			something_pressed--;
			
		if(!settings[nsfw]) {
			nextBg = (rand()%(nsfwIndex-1))+2;
		} else {
			nextBg = settings[nsfw] == 2 ? (rand()%(numBgs-(nsfwIndex+1)))+nsfwIndex : (rand()%(numBgs-1))+1;
		}

		// every ~2 minutes update the BG or on Super-Shift+N for next or B for previous
		if(loops >= approxMinToms && !settings[loop_pause]) {
			loops = 0;
			if(settings[onlyFavs]){
				int nextFfavs = nextFav(bgs,favs,settings[nsfw]);
				curbg = (nextFfavs == -1 ? curbg : nextFfavs);
			} else {
				if(++prevInd%MAX_HISTORY == 0) prevInd++;
				curbg = nextBg;
				prev[prevInd%MAX_HISTORY] = curbg;
				printf("Setting:[%d]%s\n",curbg,bgs[curbg]);
				SystemParametersInfo(SPI_SETDESKWALLPAPER,0,bgs[curbg],SPIF_SENDCHANGE);
			}
		}

		//Super-Q  = quit
		if(checkHotkey(keys_WIN | keys_Q, &something_pressed)) {
			running = 0;
		}

		//Super-Z   = toggle show/hide desktop icons
		if(checkHotkey(keys_WIN | keys_Z, &something_pressed)) {
				SendMessage(hShellViewWin,0x0111, 0x7402, 0);
		}
		
		//Super-S   = Save current BG to favs. (rotating 10 slots first in first out).
		if(checkHotkey(keys_WIN | keys_S, &something_pressed)) {
				pushIntStack(favs,curbg);
				printFavs(favs,bgs);
				//printf("set bg[%d] = %d - bg: %s\n",favs->top,favs->inds[favs->top],bgs[favs->inds[favs->top]]);
		}
		
		//Super-F    = Load saved favs (roatating pointer from last in, does not pop favs from list)
		if(checkHotkey(keys_WIN | keys_F, &something_pressed)) {
			int nextFfavs = nextFav(bgs,favs,settings[nsfw]);
			curbg = (nextFfavs == -1 ? curbg : nextFfavs);
		}
		
		//Super+ALT-S Save Settings
		if(checkHotkey(keys_WIN | keys_ALT | keys_S, &something_pressed)) {
				saveSettings(efavfpath,&settings);
		}
		
		//Super+ALT-L Re-load Saved Settings
		if(checkHotkey(keys_WIN | keys_ALT | keys_L, &something_pressed)) {
				loadSettings(efavfpath,&settings);
		}

		//Super+Shift-N = go to next random image
		if(checkHotkey(keys_WIN | keys_SHIFT | keys_N, &something_pressed)) {
				loops = 0;
				if(settings[onlyFavs]){
					int nextFfavs = nextFav(bgs,favs,settings[nsfw]);
					curbg = (nextFfavs == -1 ? curbg : nextFfavs);
				} else {
					if(++prevInd%MAX_HISTORY == 0) prevInd++;
					if(prevInd >= MAX_HISTORY) prevInd = 1;
					curbg = nextBg;
					prev[prevInd%MAX_HISTORY] = curbg;
					printf("Setting:[%d]%s\n",curbg,bgs[curbg]);
					SystemParametersInfo(SPI_SETDESKWALLPAPER,0,bgs[curbg],SPIF_SENDCHANGE);
				}
		} 

		//Super+Shift-B = go back an image
		if(checkHotkey(keys_WIN | keys_SHIFT | keys_B, &something_pressed) && prev != 0x00) {
				loops = 0;
				if(--prevInd < 0) prevInd = 0;
				if(prevInd == MAX_HISTORY) prevInd--;
				int prevbg = prev[prevInd%MAX_HISTORY];
				curbg = prevbg;
				//printFavs(favs,bgs);
				printf("Setting:[%d]%s\n",curbg,bgs[curbg]);
				SystemParametersInfo(SPI_SETDESKWALLPAPER,0,bgs[curbg],SPIF_SENDCHANGE);
		}

		//Super+Shift-V = pause timed cycling
		if(checkHotkey(keys_WIN | keys_SHIFT | keys_V, &something_pressed)) {
				settings[loop_pause] ^= 1;
				if(!settings[loop_pause]) loops = 1400;
		}
		
		//Super+Shift-H = toggle NSFW
		if(checkHotkey(keys_WIN | keys_SHIFT | keys_H, &something_pressed)) {
				settings[nsfw] = ++settings[nsfw] % 3;
				printf("NSFW:%d\n",settings[nsfw]);
		}
		
		//Super+Shift-L = only cycle favorites
		if(checkHotkey(keys_WIN | keys_SHIFT | keys_L, &something_pressed)) {
				settings[onlyFavs] ^= 1;
				printf("Cycle:%s\n",(settings[onlyFavs] ? "Only Favorites" : "Normal"));
		}
		
		//Super+Shift-E = Export Favorites
		if(checkHotkey(keys_WIN | keys_SHIFT | keys_E, &something_pressed)) {
				printf("Exporting %d Favorites\n",favs->top);
				exportFavs(efavfpath,favs,bgs);
		}
		
		//Super+Shift-C = Clear Favorites
		if(checkHotkey(keys_WIN | keys_SHIFT | keys_C, &something_pressed)) {
				printf("Clearing Favorites\n",favs->top);
				WritePrivateProfileStringA(
					"Favs",
					0,
					0,
					efavfpath);
		}
		loops++;
		Sleep(75);
	}
	return 0;
}
