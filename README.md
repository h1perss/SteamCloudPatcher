# Steam Cloud Fixer & Local Backup Manager

A lightweight, robust proxy DLL (`version.dll`) designed to permanently fix Steam Cloud synchronization errors (specifically "Access Denied" errors for games without cloud entitlements) while securely redirecting and backing up your local game saves to a custom directory or OneDrive.

## 🌟 Features

*   **Green Tick Guarantee**: Completely bypasses the annoying yellow exclamation mark ("Cloud Sync Error") in the Steam UI.
*   **AutoCloud Blinder**: Intercepts Steam's internal file system scanners to prevent unauthorized sync attempts.
*   **Memory Patching**: Dynamically patches `steamclient64.dll` at runtime to skip Cloud Rewrite Error evaluations.
*   **Intelligent Auto-Detect**: No manual configuration required! The patcher monitors Steam's internal logs and instantly patches any game that triggers an "Access Denied" error, applying the fix completely automatically.
*   **Multi-Account / Profile Support**: Dynamically detects the active Steam account (`ActiveUser` registry key) to ensure correct userdata paths are used when switching accounts.
*   **AutoCloud Quarantine Protection**: Safely prevents Steam from quarantining (moving/deleting) configuration files during sync conflicts or switching accounts.
*   **Local Save Redirection**: Safely backs up your actual save files to a unified location (e.g., OneDrive) without relying on Steam's servers.
*   **Seamless Integration**: Acts as a proxy for the legitimate Windows `version.dll`, meaning it loads automatically when Steam starts without requiring an external injector.

## 🛠️ How It Works (Technical Details)

When Steam attempts to sync a game to the Steam Cloud, it checks the server for entitlements. If the server denies access, Steam enters a persistent error state. This project solves the problem through a multi-layered approach:

### 1. The AutoCloud Blinder (API Hooking)
Using **MinHook**, the DLL intercepts calls to `FindFirstFileW` and `FindFirstFileExW`. When Steam's AutoCloud system attempts to scan standard save directories (`AppData\Local`, `AppData\LocalLow`, `Documents`, etc.), our hook intervenes and returns `ERROR_FILE_NOT_FOUND`. 
By blinding Steam to the existence of these local files, Steam concludes that there is nothing to upload. Consequently, it skips the upload phase entirely, avoiding the server-side "Access Denied" response.

### 2. The Cloud Rewrite Memory Patch
For edge cases where the Cloud state evaluation still triggers an error (such as existing cached entries in `remotecache.vdf`), the DLL scans the memory of `steamclient64.dll` for a specific execution branch.
It finds the signature responsible for evaluating `k_ERemoteStorageSyncStateError` and applies a targeted 1-byte patch (changing a conditional short-jump `JE` to an unconditional `JMP`). This forces Steam to silently skip the error handling logic, instantly reverting the UI to the "In-Sync" (Green Tick) status.

### 3. Dynamic User ID & Account Switching
The DLL queries `SOFTWARE\Valve\Steam\ActiveProcess` -> `ActiveUser` dynamically to identify the logged-in user ID. This ensures that when switching profiles/accounts on Steam, the patcher immediately resolves the correct paths under `userdata\<AccountID>` for configuration and cache clearing.

### 4. Refined AutoCloud Quarantine Bypass
To prevent Steam from quarantining files during sync conflicts or account-switching, the DLL hooks `MoveFileW`, `MoveFileExW`, `MoveFileA`, and `MoveFileExA`.
Unlike generic hooks, the DLL dynamically parses the AppID from the quarantine path (e.g. `\userdata\<UserID>\<AppID>\ac\win`) and checks it against tracked games. Move operations are only blocked if the path belongs to a tracked (patched) game. This completely prevents any interference with Steam client settings (AppID 7) or legitimate games (like CS2), ensuring settings switch smoothly and are never lost on account changes.

### 5. Local Cache Management
The patcher proactively clears the `remotecache.vdf` for managed games to prevent Steam from remembering past file states, ensuring a clean slate on every launch.

## 🚀 Installation

1. Download the latest `version.dll` from the Releases page.
2. Completely close Steam (Ensure `steam.exe` and `steamwebhelper.exe` are not running in the background).
3. Navigate to your Steam installation directory (usually `C:\Program Files (x86)\Steam\`).
4. Paste the downloaded `version.dll` into the folder.
5. Launch Steam! The patcher will automatically initialize in the background and seamlessly clear old cache files for patched games.

## ⚙️ Configuration

The patcher uses a `config.json` file (typically generated in `%LOCALAPPDATA%\SteamCloudPatcher\`) to manage which games are tracked for local backup redirection.

## ⚠️ Disclaimer

This project modifies Steam client memory at runtime. While it only touches Cloud Sync UI and evaluation states, use it at your own risk. This project does not bypass DRM or game ownership checks.

## 📝 Credits

*   Uses [MinHook](https://github.com/TsudaKageyu/minhook) for API hooking.
*   Inspired by legacy Cloud fixes like STFixer.
