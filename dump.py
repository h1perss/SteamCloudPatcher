import ctypes
from ctypes import wintypes
import psutil
import struct
import sys

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

pid = None
for proc in psutil.process_iter(['name', 'pid']):
    if proc.info['name'].lower() == 'steam.exe':
        pid = proc.info['pid']
        break

if not pid:
    print("Steam not running")
    sys.exit(1)

hProcess = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
if not hProcess:
    print("Failed to open process")
    sys.exit(1)

hMods = (wintypes.HMODULE * 1024)()
cbNeeded = wintypes.DWORD()
if psapi.EnumProcessModules(hProcess, ctypes.byref(hMods), ctypes.sizeof(hMods), ctypes.byref(cbNeeded)):
    for i in range(cbNeeded.value // ctypes.sizeof(wintypes.HMODULE)):
        szModName = ctypes.create_string_buffer(260)
        if psapi.GetModuleFileNameExA(hProcess, hMods[i], szModName, ctypes.sizeof(szModName)):
            if b"steamclient64.dll" in szModName.value.lower():
                class MODULEINFO(ctypes.Structure):
                    _fields_ = [("lpBaseOfDll", ctypes.c_void_p),
                                ("SizeOfImage", wintypes.DWORD),
                                ("EntryPoint", ctypes.c_void_p)]
                modInfo = MODULEINFO()
                if psapi.GetModuleInformation(hProcess, hMods[i], ctypes.byref(modInfo), ctypes.sizeof(modInfo)):
                    size = modInfo.SizeOfImage
                    buf = ctypes.create_string_buffer(size)
                    bytesRead = ctypes.c_size_t()
                    if kernel32.ReadProcessMemory(hProcess, modInfo.lpBaseOfDll, buf, size, ctypes.byref(bytesRead)):
                        with open("F:\\stealthripapi\\SteamCloudPatcher\\steamclient64_dump.bin", "wb") as f:
                            f.write(buf.raw[:bytesRead.value])
                        print(f"Dumped {bytesRead.value} bytes")
                    else:
                        print("ReadProcessMemory failed")
                break
kernel32.CloseHandle(hProcess)
