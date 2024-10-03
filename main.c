#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define MAX_BGS 2000
#define MAX_HISTORY 20
#define MAX_iSTACK_SIZE 10
#define SystemTimePointer ((_KSYSTEM_TIME*)0x7FFE0014)

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
int ListDirectoryContents(const char *sDir,char* storage, char* indexes[],int psize, const char* ext) {
	WIN32_FIND_DATA fdFile;
	HANDLE hFind = NULL;
	char sPath[MAX_PATH] = {0};
	int ind = 1;
	//Specify a file mask. *.* = We want everything!
	sprintf_s(sPath,MAX_PATH, "%s\\%s", sDir,ext);

	if((hFind = FindFirstFile(sPath, &fdFile)) == INVALID_HANDLE_VALUE) {
		return 0;
	}

	do {
		if(strcmp(fdFile.cFileName, ".") != 0 && strcmp(fdFile.cFileName, "..") != 0) {
			sprintf_s(sPath,MAX_PATH, "%s\\%s", sDir, fdFile.cFileName);
			size_t slen = strlen(sPath)+1;
			memcpy(storage,sPath,slen);
			indexes[ind++] = storage;
			storage += slen;
			//printf("File: %d:%s\n", ind-1,indexes[ind-1]);
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
	for(int i = 0; i <= favs->top; i++){
		printf("fav[%d]:%d = %s\n",i,favs->inds[i],bgs[favs->inds[i]]);
	}
}

int main(int argc, char *argv[]) {
	_KSYSTEM_TIME st;
	char* bgPaths = malloc(MAX_PATH*MAX_BGS);
	char** bgs = malloc(MAX_BGS*sizeof(char*));
	char orgPaper[MAX_PATH] = {0x00};
	int toggle[10];
	memset(toggle,0,sizeof(toggle));
	int prev[10];
	memset(prev,0,sizeof(prev));
	intStack* favs = malloc(sizeof(intStack));
	memset(favs->inds,0,sizeof(int)*MAX_iSTACK_SIZE);
	favs->top = -1;
	favs->pointer = 0;
	int prevInd = 0;
	int curbg = 0;
	int loops = 0;
	int loop_pause = 0;
	int running = 1;
	int approx_minutes = 2;

	if(argc < 2) {
		MessageBoxA(0,"Usage: BackgroundHotkeyThing.exe <path to BG images>\nNOTE: currently only PNG/JPG is searched for in a single path(non-recursive)\n","Woops",0);
		return 0;
	}
	if(argc > 2) {
		int tmp = atoi(argv[2]);
		approx_minutes = (tmp > 0  ? tmp : approx_minutes);
		//char tmps[32] = {0};
		//sprintf_s(tmps,32,"Approx Wait set to %d minutes.",tmp);
		//MessageBoxA(0,tmps,"debug",0);
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

	//do some very basic random seeding via some ASLR and system time values from KUSER_SHARED_DATA...
	memcpy(&st,SystemTimePointer,sizeof(st));
	srand((unsigned int)((uintptr_t)&main + (uintptr_t)&ListDirectoryContents) + st.LowPart);

	//get the current BG and set it as the first in history
	SystemParametersInfo(SPI_GETDESKWALLPAPER,MAX_PATH,&orgPaper,0);
	size_t ogPathLen = strlen(orgPaper)+1;
	memcpy(bgPaths,orgPaper,ogPathLen);
	prev[0] = 0;
	bgs[0] = bgPaths;
	bgPaths += ogPathLen;

	//populate the paths and index arrays while getting the number of pngs
	int numBgs = ListDirectoryContents(relpath,bgPaths,&bgs[1],MAX_BGS,"*.png");
	numBgs += ListDirectoryContents(relpath,(bgs[numBgs-1]+strlen(bgs[numBgs-1])+1),&bgs[numBgs-1],MAX_BGS-numBgs,"*.jpg") - 1;

	if(numBgs == 0 ) {
		char errmsg[MAX_PATH+32];
		sprintf_s(errmsg,MAX_PATH+32,"Path or Images not found at: [%s]\n",relpath);
		MessageBoxA(0,errmsg,"Whoops!", 0);
		return 0;
	}

	//get a handle to the desktop to recieve show/hide icon messages
	HWND hShellViewWin = gethShellViewWin();

	//60 seconds in a min, 1000ms in a second = 60000ms/min
	//sleep is 75ms (estimate instructions at 10ms) = 85ms
	//60000 / 85 = 705
	int approxMinToms = approx_minutes * 705;
	int nextBg = 0;

	while(running) {
		nextBg = (rand()%(numBgs-1))+1;

		// every ~2 minutes update the BG or on Super-Shift+N for next or B for previous
		if(loops >= approxMinToms && !loop_pause) {
			loops = 0;
			if(++prevInd%MAX_HISTORY == 0) prevInd++;
			prev[prevInd%MAX_HISTORY] = nextBg;
			SystemParametersInfo(SPI_SETDESKWALLPAPER,0,bgs[nextBg],SPIF_SENDCHANGE);
		}

		if(GetAsyncKeyState(VK_LWIN) < 0) {
			//Super-Q  = quit
			if(GetAsyncKeyState('Q') < 0) {
				running = 0;
			}

			//Super-Z   = toggle show/hide desktop icons
			if(GetAsyncKeyState('Z') < 0) {
				if(!toggle[0]) {
					SendMessage(hShellViewWin,0x0111, 0x7402, 0);
					toggle[0] = 1;
				}
			} else {
				toggle[0] = 0;
			}
			
			//Super-S   = Save current BG to favs. (rotating 10 slots first in first out).
			if(GetAsyncKeyState('S') < 0) {
				if(!toggle[1]) {
					pushIntStack(favs,curbg);
					//printf("set bg[%d] = %d - bg: %s\n",favs->top,favs->inds[favs->top],bgs[favs->inds[favs->top]]);
					toggle[1] = 1;
				}
			} else {
				toggle[1] = 0;
			}
			
			//Super-F    = Load saved favs (roatating pointer from last in, does not pop favs from list)
			if(GetAsyncKeyState('F') < 0) {
				if(!toggle[2]) {
					int favsp = favs->pointer;
					int favSlot = peekIntStackItr(favs);
					//printf("load bg[%d] = %d - bg: %s\n",favsp,favSlot,bgs[favSlot]);
					SystemParametersInfo(SPI_SETDESKWALLPAPER,0,bgs[favSlot],SPIF_SENDCHANGE);
					toggle[2] = 1;
				}
			} else {
				toggle[2] = 0;
			}

			if(GetAsyncKeyState(VK_LSHIFT) < 0 || GetAsyncKeyState(VK_RSHIFT) < 0) {

				//Super+Shift-N = go to next random image
				if(GetAsyncKeyState('N') < 0) {
					if(!toggle[3]) {
						toggle[3] = 1;
						loops = 0;
						if(++prevInd%MAX_HISTORY == 0) prevInd++;
						if(prevInd >= MAX_HISTORY) prevInd = 1;
						prev[prevInd%MAX_HISTORY] = nextBg;
						curbg = nextBg;
						printFavs(favs,bgs);
						SystemParametersInfo(SPI_SETDESKWALLPAPER,0,bgs[curbg],SPIF_SENDCHANGE);
					}
				} else {
					toggle[3] = 0;
				}

				//Super+Shift-B = go back an image
				if(GetAsyncKeyState('B') < 0 && prev != 0x00) {
					if(!toggle[4]) {
						toggle[4] = 1;
						loops = 0;
						if(--prevInd < 0) prevInd = 0;
						if(prevInd == MAX_HISTORY) prevInd--;
						int prevbg = prev[prevInd%MAX_HISTORY];
						curbg = prevbg;
						printFavs(favs,bgs);
						SystemParametersInfo(SPI_SETDESKWALLPAPER,0,bgs[prevbg],SPIF_SENDCHANGE);
					}
				} else {
					toggle[4] = 0;
				}

				//Super+Shift-V = pause timed cycling
				if(GetAsyncKeyState('V') < 0) {
					if(!toggle[5]) {
						toggle[5] = 1;
						loop_pause ^= 1;
						if(!loop_pause) loops = 1400;
					}
				} else {
					toggle[5] = 0;
				}
			}
		}
		loops++;
		Sleep(75);
	}
	return 0;
}
