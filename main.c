#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define MAX_BGS 2000

int ListDirectoryContents(const char *sDir,char* storage, char* indexes[],int psize, const char* ext)
{
    WIN32_FIND_DATA fdFile;
    HANDLE hFind = NULL;
    char sPath[MAX_PATH] = {0};
	int ind = 0;
    //Specify a file mask. *.* = We want everything!
    sprintf_s(sPath,MAX_PATH, "%s\\%s", sDir,ext);

    if((hFind = FindFirstFile(sPath, &fdFile)) == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
	
    do
    {
        if(strcmp(fdFile.cFileName, ".") != 0 && strcmp(fdFile.cFileName, "..") != 0)
        {
            sprintf_s(sPath,MAX_PATH, "%s\\%s", sDir, fdFile.cFileName);
            size_t slen = strlen(sPath)+1;
            memcpy(storage,sPath,slen);
            indexes[ind++] = storage;
            storage += slen;
            //printf("File: %d:%s\n", ind-1,indexes[ind-1]);
        }
    }
    while(FindNextFile(hFind, &fdFile) && ind < psize-1); //Find the next file.

    FindClose(hFind); //Always, Always, clean things up!

    return ind;
}

HWND gethShellViewWin(){
	HWND prgMan = FindWindowA("Progman", "Program Manager");
	HWND hShellViewWin = FindWindowExA(prgMan, 0, "SHELLDLL_DefView", "");
	
	if(hShellViewWin == 0x00){
		HWND hWorkerW = 0x00;
		do
                {
                    hWorkerW = FindWindowExA(0, hWorkerW, "WorkerW", "");
                    hShellViewWin = FindWindowExA(hWorkerW, 0, "SHELLDLL_DefView", "");
                } while (hShellViewWin == 0x00 && hWorkerW != 0x00);
	}
	return hShellViewWin;
}

int main(int argc, char *argv[]) {
	SYSTEMTIME st, lt;
	char* bgPaths = malloc(MAX_PATH*MAX_BGS);
	char* bgs[MAX_BGS];
	int toggle[10] = {0};
	char orgPaper[MAX_PATH] = {0x00};
	int prevInd = 0;
	int loops = 0;
	int loop_pause = 0;
	int MAX_HISTORY = 20;
	int running = 1;
	if(argc < 2){
		MessageBoxA(0,"Usage: Icons_N_BGs.exe <path to BG images>\nNOTE: currently only PNG/JPG is searched for in a single path(non-recursive)\n","Woops",0);
		return 0;
	}
	
	if(argc > 2){
		MAX_HISTORY = atoi(argv[2]) - 1;
	}

	char **prev = malloc(MAX_HISTORY+1*sizeof(char*));

	char relpath[MAX_PATH] = {0};
	char exepath[MAX_PATH] = {0};
	memcpy(exepath,argv[0],strlen(argv[0]));
	
	if(relpath[0] == '.' || relpath[1] != ':'){
		char* exenameBegin = strrchr(exepath,(int)'\\');
		*exenameBegin = 0x00;
		sprintf_s(relpath,MAX_PATH,"%s\\%s",exepath,argv[1]);
	}

	//do some very basic random seeding via some ASLR and system time values...
	GetSystemTime(&st);
	srand ((unsigned int)((uintptr_t)&main + (uintptr_t)&ListDirectoryContents) + st.wSecond + st.wDay + st.wMilliseconds);

	//populate the paths and index arrays while getting the number of pngs
	int numBgs = ListDirectoryContents(relpath,bgPaths,bgs,MAX_BGS,"*.png");
	numBgs += ListDirectoryContents(relpath,&bgPaths[numBgs],&bgs[numBgs],MAX_BGS-numBgs,"*.jpg");
	
	if(numBgs == 0 ){
		char errmsg[MAX_PATH+32];
		sprintf_s(errmsg,MAX_PATH+32,"Path or Images not found at: [%s]\n",relpath);
        MessageBoxA(0,errmsg,"Whoops!", 0);
        return 0;
	}
	
	//get the current BG and set it as the first in history
	SystemParametersInfo(SPI_GETDESKWALLPAPER,MAX_PATH,&orgPaper,0);
	prev[0] = &orgPaper[0];
	
	//get a handle to the desktop to recieve show/hide icon messages
	HWND hShellViewWin = gethShellViewWin();
	
	while(running){
		
		// every ~2 minutes update the BG or on Super-Shift+N for next or B for previous
		if(loops >= 1440 && !loop_pause || (GetAsyncKeyState(VK_LWIN) < 0 && GetAsyncKeyState(VK_LSHIFT) < 0 && GetAsyncKeyState('N') < 0)){
			if(loops >= 1440) loops = 0;
			if(!toggle[0]){
				loops = 0;
				if(++prevInd  >= INT_MAX) prevInd = 0;
				prev[prevInd%MAX_HISTORY] = bgs[rand()%numBgs];
				//printf("pi:%d - Switching to: %s\n",prevInd%MAX_HISTORY,prev[prevInd%MAX_HISTORY]);
				SystemParametersInfo(SPI_SETDESKWALLPAPER,0,prev[prevInd%MAX_HISTORY],SPIF_SENDCHANGE);
				toggle[0] = 1;
			}
		} else {
			toggle[0] = 0;
		}
		
		if(GetAsyncKeyState(VK_LWIN) < 0){
			
			//Super-Z   = toggle show/hide desktop icons
			if(GetAsyncKeyState('Z') < 0){
				if(!toggle[1]){
					SendMessage(hShellViewWin,0x0111, 0x7402, 0);
					toggle[1] = 1;
				}
			} else {
				toggle[1] = 0;
			}
			//Super-Q  = quit
			if(GetAsyncKeyState('Q') < 0){
				running = 0;
			}
		
			if(GetAsyncKeyState(VK_LSHIFT) < 0 || GetAsyncKeyState(VK_RSHIFT) < 0) {
				//Super+Shift-B = go back an image
				if(!toggle[2] && GetAsyncKeyState('B') < 0 && prev != 0x00){
					if(--prevInd < 0) prevInd = 0;
					toggle[2] = 1;
					//printf("pi:%d - Switching to: %s\n",prevInd%MAX_HISTORY,prev[prevInd%MAX_HISTORY]);
					SystemParametersInfo(SPI_SETDESKWALLPAPER,0,prev[prevInd%MAX_HISTORY],SPIF_SENDCHANGE);
				} else {
					toggle[2] = 0;
				}
				//Super+Shift-V = pause cycling
				if(!toggle[3] && GetAsyncKeyState('V') < 0){
					loop_pause ^= 1;
					if(!loop_pause) loops = 1400;
					toggle[3] = 1;
				} else {
					toggle[3] = 0;
				}
			}
		} else {
			memset(toggle,0,sizeof(toggle));
		}
		loops++;
		Sleep(75);
	}
	return 0;
}