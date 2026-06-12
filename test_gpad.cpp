#include <Windows.h>
#include <iostream>

inline FARPROC GetProcAddressDirect(HMODULE hModule, const char* funcName) {
    if (!hModule) return nullptr;
    BYTE* base = (BYTE*)hModule;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    IMAGE_DATA_DIRECTORY exportDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDir.Size == 0) return nullptr;
    IMAGE_EXPORT_DIRECTORY* exports = (IMAGE_EXPORT_DIRECTORY*)(base + exportDir.VirtualAddress);
    
    DWORD* names = (DWORD*)(base + exports->AddressOfNames);
    WORD* ordinals = (WORD*)(base + exports->AddressOfNameOrdinals);
    DWORD* functions = (DWORD*)(base + exports->AddressOfFunctions);
    
    for (DWORD i = 0; i < exports->NumberOfNames; i++) {
        const char* name = (const char*)(base + names[i]);
        if (strcmp(name, funcName) == 0) {
            WORD ord = ordinals[i];
            DWORD funcRva = functions[ord];
            if (funcRva >= exportDir.VirtualAddress && funcRva < exportDir.VirtualAddress + exportDir.Size) {
                std::cout << "Forwarded export: " << (const char*)(base + funcRva) << std::endl;
            }
            return (FARPROC)(base + funcRva);
        }
    }
    return nullptr;
}

int main() {
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) {
        std::cout << "Failed to get kernel32.dll handle" << std::endl;
        return 1;
    }
    FARPROC gpaDirect = GetProcAddressDirect(hKernel32, "GetProcAddress");
    FARPROC gpaReal = GetProcAddress(hKernel32, "GetProcAddress");
    std::cout << "Direct: " << (void*)gpaDirect << ", Real: " << (void*)gpaReal << std::endl;
    if (gpaDirect == gpaReal) {
        std::cout << "MATCH!" << std::endl;
    } else {
        std::cout << "MISMATCH!" << std::endl;
    }
    return 0;
}
