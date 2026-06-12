#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>

int main() {
    DWORD pid = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(hSnap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "steam.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);

    if (!pid) {
        printf("Steam not running\n");
        return 1;
    }

    HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) {
        printf("OpenProcess failed\n");
        return 1;
    }

    HMODULE hMods[1024];
    DWORD cbNeeded;
    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
            char szModName[MAX_PATH];
            if (GetModuleFileNameExA(hProcess, hMods[i], szModName, sizeof(szModName))) {
                if (strstr(szModName, "steamclient64.dll")) {
                    MODULEINFO modInfo;
                    GetModuleInformation(hProcess, hMods[i], &modInfo, sizeof(modInfo));
                    
                    DWORD size = modInfo.SizeOfImage;
                    unsigned char* buf = (unsigned char*)malloc(size);
                    SIZE_T bytesRead;
                    if (ReadProcessMemory(hProcess, modInfo.lpBaseOfDll, buf, size, &bytesRead)) {
                        FILE* f = fopen("F:\\stealthripapi\\SteamCloudPatcher\\steamclient64_dump.bin", "wb");
                        fwrite(buf, 1, bytesRead, f);
                        fclose(f);
                        printf("Dumped %zu bytes\n", bytesRead);
                    }
                    free(buf);
                    break;
                }
            }
        }
    }
    CloseHandle(hProcess);
    return 0;
}
