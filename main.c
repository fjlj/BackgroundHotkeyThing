#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int ListDirectoryContents(const char *sDir,char* storage, char* indexes[],int psize)
{
    WIN32_FIND_DATA fdFile;
    HANDLE hFind = NULL;
    char sPath[275] = {0};
	int ind = 0;
    //Specify a file mask. *.* = We want everything!
    sprintf(sPath, "%s\\*.png", sDir);

    if((hFind = FindFirstFile(sPath, &fdFile)) == INVALID_HANDLE_VALUE)
    {
    	sprintf(sPath,"Path or Images not found at: [%s]\n",sDir);
        MessageBoxA(0,sPath,"Whoops!", 0);
        return 0;
    }
	
    do
    {
        if(strcmp(fdFile.cFileName, ".") != 0 && strcmp(fdFile.cFileName, "..") != 0)
        {
            sprintf(sPath, "%s\\%s", sDir, fdFile.cFileName);
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
	char* bgPaths = malloc(MAX_PATH*2000);
	char* bgs[2000];
	int toggle[10] = {0};
	char orgPaper[MAX_PATH] = {0x00};
	char* prev[11] = {0x00};
	int prevInd = 0;
	int loops = 0;
	
	if(argc < 2){
		MessageBoxA(0,"Usage: Icons_N_BGs.exe <path to BG images>\nNOTE: currently only PNG is searched for in a single path(non-recursive)\n","Woops",0);
		return 0;
	}
	//do some very basic random seeding via some ASLR and system time values...
	GetSystemTime(&st);
	srand ((unsigned int)((uintptr_t)&main + (uintptr_t)&ListDirectoryContents) + st.wSecond + st.wDay + st.wMilliseconds);

	//populate the paths and index arrays while getting the number of pngs
	int numBgs = ListDirectoryContents(argv[1],bgPaths,bgs,2000);
	
	if(numBgs == 0) return 0;
	
	//get the current BG and set it as the first in history
	SystemParametersInfo(SPI_GETDESKWALLPAPER,MAX_PATH,&orgPaper,0);
	prev[0] = &orgPaper[0];
	
	//get a handle to the desktop to recieve show/hide icon messages
	HWND hShellViewWin = gethShellViewWin();
	
	while(1){
		
		//Super + Z   =  toggle show/hide desktop icons
		if(GetAsyncKeyState(VK_LWIN) < 0 && GetAsyncKeyState('Z') < 0){
			if(!toggle[0]){
				SendMessage(hShellViewWin,0x0111, 0x7402, 0);
				toggle[0] = 1;
			}
		} else {
			toggle[0] = 0;
		}
		
		// every ~2 minutes update the BG or on Super-Shift+N for next or B for previous
		if(loops >= 1440 || (GetAsyncKeyState(VK_LWIN) < 0 && GetAsyncKeyState(VK_LSHIFT) < 0 && GetAsyncKeyState('N') < 0)){
			if(loops >= 1440) loops = 0;
			if(!toggle[1]){
				loops = 0;
				prev[++prevInd%10] = bgs[rand()%numBgs];
				//printf("pi:%d - Switching to: %s\n",prevInd%10,prev[prevInd%10]);
				SystemParametersInfo(SPI_SETDESKWALLPAPER,0,prev[prevInd%10],SPIF_SENDCHANGE);
				toggle[1] = 1;
			}
		} else {
			toggle[1] = 0;
		}
		
		if(GetAsyncKeyState(VK_LWIN) < 0 && GetAsyncKeyState(VK_LSHIFT) < 0 && GetAsyncKeyState('B') < 0 && prev != 0x00){
			if(!toggle[2]){
				loops = 0;
				if(--prevInd < 0) prevInd = 0;
				//printf("pi:%d - Switching to: %s\n",prevInd%10,prev[prevInd%10]);
				SystemParametersInfo(SPI_SETDESKWALLPAPER,0,prev[prevInd%10],SPIF_SENDCHANGE);
				toggle[2] = 1;
			}
		} else {
			toggle[2] = 0;
		}
		Sleep(75);
		loops++;
	}
	return 0;
}