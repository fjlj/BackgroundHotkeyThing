#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define MAX_BGS 2000
#define MAX_HISTORY 20
#define SystemTimePointer ((_KSYSTEM_TIME*)0x7FFE0014)

typedef struct {
	ULONG LowPart;
	LONG High1Time;
	LONG High2Time;
} _KSYSTEM_TIME;

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

int main(int argc, char *argv[]) {
	_KSYSTEM_TIME st;
	char* bgPaths = malloc(MAX_PATH*MAX_BGS);
	char* bgs[MAX_BGS];
	char orgPaper[MAX_PATH] = {0x00};
	int toggle[10] = {0x00};
	int prev[10] = {0x00};
	int favs[10] = {0x00};
	int prevInd = 0;
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

	if(argv[1][0] == '.' || argv[1][1] != ':') {
		memset(&relpath[0],0x00,MAX_PATH);
		char* exenameBegin = strrchr(exepath,(int)'\\');
		*exenameBegin = 0x00;
		sprintf_s(relpath,MAX_PATH,"%s\\%s",exepath,argv[1]);
	}

	//do some very basic random seeding via some ASLR and system time values from KUSER_SHARED_DATA...
	memcpy(&st,SystemTimePointer,sizeof(st));
	srand ((unsigned int)((uintptr_t)&main + (uintptr_t)&ListDirectoryContents) + st.LowPart);

	//get the current BG and set it as the first in history
	SystemParametersInfo(SPI_GETDESKWALLPAPER,MAX_PATH,&orgPaper,0);
	size_t ogPathLen = strlen(orgPaper)+1;
	memcpy(bgPaths,orgPaper,ogPathLen);
	prev[0] = 0;
	bgs[0] = bgPaths;
	bgPaths += ogPathLen;

	//populate the paths and index arrays while getting the number of pngs
	int numBgs = ListDirectoryContents(relpath,bgPaths,bgs,MAX_BGS,"*.png");
	numBgs += ListDirectoryContents(relpath,bgs[numBgs-1]+strlen(bgs[numBgs-1]),&bgs[numBgs],MAX_BGS-numBgs,"*.jpg");

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


			if(GetAsyncKeyState(VK_LSHIFT) < 0 || GetAsyncKeyState(VK_RSHIFT) < 0) {

				//Super+Shift-N = go to next random image
				if(GetAsyncKeyState('N') < 0) {
					if(!toggle[1]) {
						toggle[1] = 1;
						loops = 0;
						if(++prevInd%MAX_HISTORY == 0) prevInd++;
						prev[prevInd%MAX_HISTORY] = nextBg;
						SystemParametersInfo(SPI_SETDESKWALLPAPER,0,bgs[nextBg],SPIF_SENDCHANGE);
					}
				} else {
					toggle[1] = 0;
				}

				//Super+Shift-B = go back an image
				if(GetAsyncKeyState('B') < 0 && prev != 0x00) {
					if(!toggle[2]) {
						toggle[2] = 1;
						loops = 0;
						if(--prevInd < 0) prevInd = 0;
						if(prevInd == MAX_HISTORY) prevInd--;
						int prevbg = prev[prevInd%MAX_HISTORY];
						SystemParametersInfo(SPI_SETDESKWALLPAPER,0,bgs[prevbg],SPIF_SENDCHANGE);
					}
				} else {
					toggle[2] = 0;
				}

				//Super+Shift-V = pause cycling
				if(GetAsyncKeyState('V') < 0) {
					if(!toggle[3]) {
						toggle[3] = 1;
						loop_pause ^= 1;
						if(!loop_pause) loops = 1400;
					}
				} else {
					toggle[3] = 0;
				}
			}
		}
		loops++;
		Sleep(75);
	}
	return 0;
}
