#include <Windows.h>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <set>
#include <map>
#include <fstream>
#include <sstream>
#include <regex>
#include <optional>
#include <iomanip>
#include <mutex>
#include <algorithm>
#include <psapi.h>
#include <nlohmann/json.hpp>
#include "MinHook.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

#pragma comment(linker, "/export:GetFileVersionInfoA=C:\\Windows\\System32\\version.GetFileVersionInfoA")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=C:\\Windows\\System32\\version.GetFileVersionInfoByHandle")
#pragma comment(linker, "/export:GetFileVersionInfoExA=C:\\Windows\\System32\\version.GetFileVersionInfoExA")
#pragma comment(linker, "/export:GetFileVersionInfoExW=C:\\Windows\\System32\\version.GetFileVersionInfoExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=C:\\Windows\\System32\\version.GetFileVersionInfoSizeA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExA=C:\\Windows\\System32\\version.GetFileVersionInfoSizeExA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExW=C:\\Windows\\System32\\version.GetFileVersionInfoSizeExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=C:\\Windows\\System32\\version.GetFileVersionInfoSizeW")
#pragma comment(linker, "/export:GetFileVersionInfoW=C:\\Windows\\System32\\version.GetFileVersionInfoW")
#pragma comment(linker, "/export:VerFindFileA=C:\\Windows\\System32\\version.VerFindFileA")
#pragma comment(linker, "/export:VerFindFileW=C:\\Windows\\System32\\version.VerFindFileW")
#pragma comment(linker, "/export:VerInstallFileA=C:\\Windows\\System32\\version.VerInstallFileA")
#pragma comment(linker, "/export:VerInstallFileW=C:\\Windows\\System32\\version.VerInstallFileW")
#pragma comment(linker, "/export:VerLanguageNameA=C:\\Windows\\System32\\version.VerLanguageNameA")
#pragma comment(linker, "/export:VerLanguageNameW=C:\\Windows\\System32\\version.VerLanguageNameW")
#pragma comment(linker, "/export:VerQueryValueA=C:\\Windows\\System32\\version.VerQueryValueA")
#pragma comment(linker, "/export:VerQueryValueW=C:\\Windows\\System32\\version.VerQueryValueW")

static std::atomic<bool> g_dllRunning{true};
void LogDebug(const std::string& msg);
static std::string g_steamPath;
static std::string g_steamUserId;
static bool g_autoPatch = true;
static std::string g_autoProvider = "onedrive";
static std::string g_autoCustomPath = "";
static bool g_enableBackup = true;
static std::string g_backupPath = "";
static std::string g_patcherPath;
static HANDLE g_patcherProcess = nullptr;
static uint32_t g_lastLaunchSyncedAppId = 0;
static std::mutex g_configMutex;
static std::set<uint32_t> g_patchedAppIds;

inline std::optional<std::string> ReadRegistryString(HKEY hKeyRoot, const std::string& subKey, const std::string& valueName) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(hKeyRoot, subKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dataType = 0;
        DWORD dataSize = 0;
        if (RegQueryValueExA(hKey, valueName.c_str(), nullptr, &dataType, nullptr, &dataSize) == ERROR_SUCCESS) {
            std::string value(dataSize, '\0');
            if (RegQueryValueExA(hKey, valueName.c_str(), nullptr, &dataType, reinterpret_cast<LPBYTE>(value.data()), &dataSize) == ERROR_SUCCESS) {
                RegCloseKey(hKey);
                while (!value.empty() && value.back() == '\0') {
                    value.pop_back();
                }
                return value;
            }
        }
        RegCloseKey(hKey);
    }
    return std::nullopt;
}

inline std::optional<DWORD> ReadRegistryDword(HKEY hKeyRoot, const std::string& subKey, const std::string& valueName) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(hKeyRoot, subKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 0;
        DWORD dataSize = sizeof(DWORD);
        DWORD dataType = 0;
        if (RegQueryValueExA(hKey, valueName.c_str(), nullptr, &dataType, reinterpret_cast<LPBYTE>(&value), &dataSize) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return value;
        }
        RegCloseKey(hKey);
    }
    return std::nullopt;
}

inline std::optional<std::string> ReadFileContents(const fs::path& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

inline std::string GetUserProfilePath() {
    char* profile = nullptr;
    size_t len = 0;
    if (_dupenv_s(&profile, &len, "USERPROFILE") == 0 && profile != nullptr) {
        std::string result(profile);
        free(profile);
        return result;
    }
    return "C:\\Users\\Default";
}

inline void TouchDirectoryFiles(const std::string& dirPath) {
    try {
        if (!fs::exists(dirPath)) return;
        auto now = fs::file_time_type::clock::now();
        if (fs::is_directory(dirPath)) {
            for (const auto& entry : fs::recursive_directory_iterator(dirPath, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file()) {
                    try { fs::last_write_time(entry.path(), now); } catch (...) {}
                }
            }
        } else if (fs::is_regular_file(dirPath)) {
            fs::last_write_time(dirPath, now);
        }
    } catch (...) {}
}

inline std::vector<std::pair<std::string, std::string>> ParseVdfKeyValues(const std::string& content) {
    std::vector<std::pair<std::string, std::string>> pairs;
    std::regex kvRegex(R"raw("([^"]+)"\s+"([^"]*)")raw");
    std::sregex_iterator it(content.begin(), content.end(), kvRegex);
    std::sregex_iterator end;
    for (; it != end; ++it) {
        pairs.emplace_back((*it)[1].str(), (*it)[2].str());
    }
    return pairs;
}

inline std::vector<std::string> FindSteamLibraryFolders(const std::string& steamPath) {
    std::vector<std::string> folders;
    folders.push_back(steamPath);
    fs::path vdfPath = fs::path(steamPath) / "steamapps" / "libraryfolders.vdf";
    auto content = ReadFileContents(vdfPath);
    if (content.has_value()) {
        auto kvPairs = ParseVdfKeyValues(*content);
        for (const auto& [key, value] : kvPairs) {
            if (key == "path" && !value.empty()) {
                std::string normalized = value;
                std::string unescaped;
                for (size_t i = 0; i < normalized.size(); ++i) {
                    if (normalized[i] == '\\' && i + 1 < normalized.size() && normalized[i + 1] == '\\') {
                        unescaped += '\\';
                        ++i;
                    } else {
                        unescaped += normalized[i];
                    }
                }
                if (fs::exists(unescaped) && unescaped != steamPath) {
                    folders.push_back(unescaped);
                }
            }
        }
    }
    return folders;
}

inline std::optional<std::pair<uint32_t, std::string>> ParseManifestFile(const fs::path& path) {
    auto contentOpt = ReadFileContents(path);
    if (!contentOpt) return std::nullopt;
    std::string content = *contentOpt;

    uint32_t appId = 0;
    std::regex idRegex(R"raw("appid"\s+"(\d+)")raw", std::regex_constants::icase);
    std::smatch idMatch;
    if (std::regex_search(content, idMatch, idRegex)) {
        try { appId = std::stoul(idMatch[1].str()); } catch (...) { return std::nullopt; }
    } else {
        return std::nullopt;
    }

    std::string name;
    std::regex nameRegex(R"raw("name"\s+"([^"]+)")raw", std::regex_constants::icase);
    std::smatch nameMatch;
    if (std::regex_search(content, nameMatch, nameRegex)) {
        name = nameMatch[1].str();
    } else {
        name = "Steam App " + std::to_string(appId);
    }
    return std::make_pair(appId, name);
}

inline bool IsProtectedSystemDirectory(const std::string& pathStr) {
    if (pathStr.empty()) return true;
    try {
        fs::path p = fs::weakly_canonical(fs::path(pathStr));
        std::string pStr = p.string();
        std::vector<std::string> protectedPaths;
        char* userProfile = nullptr;
        size_t len = 0;
        if (_dupenv_s(&userProfile, &len, "USERPROFILE") == 0 && userProfile != nullptr) {
            std::string up(userProfile);
            free(userProfile);
            protectedPaths.push_back(up);
            protectedPaths.push_back(up + "\\Documents");
            protectedPaths.push_back(up + "\\Saved Games");
            protectedPaths.push_back(up + "\\Desktop");
            protectedPaths.push_back(up + "\\AppData\\LocalLow");
        }
        char* localAppData = nullptr;
        if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") == 0 && localAppData != nullptr) {
            protectedPaths.push_back(localAppData);
            free(localAppData);
        }
        char* appData = nullptr;
        if (_dupenv_s(&appData, &len, "APPDATA") == 0 && appData != nullptr) {
            protectedPaths.push_back(appData);
            free(appData);
        }
        for (const auto& path : protectedPaths) {
            if (path.empty()) continue;
            try {
                fs::path cp = fs::weakly_canonical(fs::path(path));
                if (_stricmp(pStr.c_str(), cp.string().c_str()) == 0) {
                    return true;
                }
            } catch (...) {}
        }
    } catch (...) {}
    return false;
}

inline std::optional<std::string> ResolvePathFromRemoteCache(
    const fs::path& remotecachePath,
    const std::string& steamPath,
    const std::string& userId)
{
    auto contentOpt = ReadFileContents(remotecachePath);
    if (!contentOpt) return std::nullopt;
    std::string content = *contentOpt;

    size_t pos = 0;
    while (true) {
        size_t openBrace = content.find('{', pos);
        if (openBrace == std::string::npos) break;

        size_t closeBrace = content.find('}', openBrace);
        if (closeBrace == std::string::npos) break;

        size_t rootKey = content.find("\"root\"", openBrace);
        if (rootKey != std::string::npos && rootKey < closeBrace) {
            size_t valStart = content.find('"', rootKey + 6);
            if (valStart != std::string::npos && valStart < closeBrace) {
                size_t valEnd = content.find('"', valStart + 1);
                if (valEnd != std::string::npos && valEnd < closeBrace) {
                    std::string rootStr = content.substr(valStart + 1, valEnd - valStart - 1);

                    size_t blockStart = content.rfind('"', openBrace - 1);
                    if (blockStart != std::string::npos) {
                        size_t blockKeyStart = content.rfind('"', blockStart - 1);
                        if (blockKeyStart != std::string::npos) {
                            std::string relPathStr = content.substr(blockKeyStart + 1, blockStart - blockKeyStart - 1);

                            std::string basePath;
                            std::string userProfile = GetUserProfilePath();

                            if (rootStr == "0") {
                                basePath = (fs::path(steamPath) / "userdata" / userId / remotecachePath.parent_path().filename().string() / "remote").string();
                            } else if (rootStr == "1") {
                                basePath = steamPath;
                            } else if (rootStr == "2" || _stricmp(rootStr.c_str(), "WinMyDocuments") == 0) {
                                basePath = userProfile + "\\Documents";
                            } else if (rootStr == "3" || _stricmp(rootStr.c_str(), "WinAppDataLocal") == 0) {
                                char* appdata = nullptr;
                                size_t len = 0;
                                if (_dupenv_s(&appdata, &len, "LOCALAPPDATA") == 0 && appdata != nullptr) {
                                    basePath = appdata;
                                    free(appdata);
                                } else {
                                    basePath = userProfile + "\\AppData\\Local";
                                }
                            } else if (rootStr == "4" || _stricmp(rootStr.c_str(), "WinAppDataRoaming") == 0) {
                                char* appdata = nullptr;
                                size_t len = 0;
                                if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
                                    basePath = appdata;
                                    free(appdata);
                                } else {
                                    basePath = userProfile + "\\AppData\\Roaming";
                                }
                            } else if (rootStr == "12" || _stricmp(rootStr.c_str(), "WinAppDataLocalLow") == 0) {
                                basePath = userProfile + "\\AppData\\LocalLow";
                            } else if (rootStr == "15" || _stricmp(rootStr.c_str(), "WinSavedGames") == 0) {
                                basePath = userProfile + "\\Saved Games";
                            } else {
                                return std::nullopt;
                            }

                            if (basePath.empty()) return std::nullopt;

                            fs::path relPath(relPathStr);
                            fs::path relFolder = relPath.parent_path();
                            fs::path fullSavePath = fs::path(basePath) / relFolder;
                            std::string fullSavePathStr = fullSavePath.string();
                            if (IsProtectedSystemDirectory(fullSavePathStr)) {
                                return std::nullopt;
                            }
                            return fullSavePathStr;
                        }
                    }
                }
            }
        }
        pos = closeBrace + 1;
    }
    return std::nullopt;
}

struct GameInfo {
    uint32_t appId;
    std::string name;
    std::string resolvedSavePath;
    bool installed;
};

inline std::vector<GameInfo> DetectGames(const std::string& steamPath, const std::string& userId) {
    std::vector<GameInfo> list;
    std::set<uint32_t> appIds;
    std::map<uint32_t, std::string> names;
    std::set<uint32_t> installed;

    auto libs = FindSteamLibraryFolders(steamPath);
    for (const auto& folder : libs) {
        fs::path steamapps = fs::path(folder) / "steamapps";
        if (fs::exists(steamapps) && fs::is_directory(steamapps)) {
            for (const auto& entry : fs::directory_iterator(steamapps)) {
                if (entry.is_regular_file() && entry.path().extension() == ".acf") {
                    std::string fn = entry.path().filename().string();
                    if (fn.rfind("appmanifest_", 0) == 0) {
                        auto p = ParseManifestFile(entry.path());
                        if (p) {
                            names[p->first] = p->second;
                            installed.insert(p->first);
                            appIds.insert(p->first);
                        }
                    }
                }
            }
        }
    }

    fs::path userdata = fs::path(steamPath) / "userdata" / userId;
    if (fs::exists(userdata) && fs::is_directory(userdata)) {
        for (const auto& entry : fs::directory_iterator(userdata)) {
            if (entry.is_directory()) {
                std::string dn = entry.path().filename().string();
                if (!dn.empty() && std::all_of(dn.begin(), dn.end(), ::isdigit)) {
                    try {
                        uint32_t aid = std::stoul(dn);
                        if (aid > 10) appIds.insert(aid);
                    } catch (...) {}
                }
            }
        }
    }

    for (uint32_t aid : appIds) {
        GameInfo gi;
        gi.appId = aid;
        gi.installed = (installed.find(aid) != installed.end());
        gi.name = (names.find(aid) != names.end()) ? names[aid] : "Steam App " + std::to_string(aid);

        fs::path rcPath = fs::path(steamPath) / "userdata" / userId / std::to_string(aid) / "remotecache.vdf";
        std::optional<std::string> resolved;
        if (fs::exists(rcPath)) {
            resolved = ResolvePathFromRemoteCache(rcPath, steamPath, userId);
        }

        if (resolved && !resolved->empty()) {
            gi.resolvedSavePath = *resolved;
        } else {
            fs::path usp = fs::path(steamPath) / "userdata" / userId / std::to_string(aid) / "remote";
            gi.resolvedSavePath = usp.string();
        }
        list.push_back(std::move(gi));
    }
    return list;
}

inline std::string GetProviderSavePath(const std::string& providerId, const std::string& customPath, const std::string& gameName, uint32_t appId) {
    std::string basePath;
    std::string up = GetUserProfilePath();
    
    if (providerId == "local" || providerId == "custom") {
        basePath = customPath;
        if (basePath.empty()) {
            basePath = up + "\\Documents\\SteamCloudSaves";
        }
    } else {
        if (providerId == "onedrive") {
            char* envPath = nullptr;
            size_t len = 0;
            if (_dupenv_s(&envPath, &len, "OneDrive") == 0 && envPath != nullptr) {
                basePath = envPath;
                free(envPath);
            } else {
                basePath = up + "\\Documents\\SteamCloudSaves";
            }
        } else if (providerId == "gdrive") {
            basePath = up + "\\Google Drive";
        } else if (providerId == "dropbox") {
            basePath = up + "\\Dropbox";
        }
    }

    if (basePath.empty()) return "";

    std::string sanitized = gameName;
    for (char& c : sanitized) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ') {
            c = '_';
        }
    }
    return (fs::path(basePath) / "SteamCloudPatcher" / (std::to_string(appId) + "_" + sanitized)).string();
}

inline void SyncDirectories(const std::string& lpStr, const std::string& cpStr) {
    if (IsProtectedSystemDirectory(lpStr) || IsProtectedSystemDirectory(cpStr)) {
        return;
    }
    try {
        fs::path lp(lpStr);
        fs::path cp(cpStr);
        if (!fs::exists(lp)) fs::create_directories(lp);
        if (!fs::exists(cp)) fs::create_directories(cp);

        for (const auto& entry : fs::recursive_directory_iterator(lp, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                fs::path rp = fs::relative(entry.path(), lp);
                fs::path target = cp / rp;
                bool copy = false;
                if (!fs::exists(target)) copy = true;
                else if (fs::last_write_time(entry.path()) > fs::last_write_time(target)) copy = true;

                if (copy) {
                    fs::create_directories(target.parent_path());
                    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
                    try { fs::last_write_time(target, fs::last_write_time(entry.path())); } catch (...) {}
                }
            }
        }

        for (const auto& entry : fs::recursive_directory_iterator(cp, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                fs::path rp = fs::relative(entry.path(), cp);
                fs::path target = lp / rp;
                bool copy = false;
                if (!fs::exists(target)) copy = true;
                else if (fs::last_write_time(entry.path()) > fs::last_write_time(target)) copy = true;

                if (copy) {
                    fs::create_directories(target.parent_path());
                    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
                    try { fs::last_write_time(target, fs::last_write_time(entry.path())); } catch (...) {}
                }
            }
        }
    } catch (...) {}
}

inline std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &time);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d_%H%M%S");
    return oss.str();
}

inline void CreateBackup(const std::string& sourcePath, const std::string& gameName, uint32_t appId, const std::string& customBackupPath) {
    if (IsProtectedSystemDirectory(sourcePath)) return;
    try {
        if (!fs::exists(sourcePath)) return;
        std::string targetDir = customBackupPath;
        if (targetDir.empty()) {
            char* localAppData = nullptr;
            size_t len = 0;
            if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") == 0 && localAppData != nullptr) {
                targetDir = (fs::path(localAppData) / "SteamCloudPatcher" / "backups").string();
                free(localAppData);
            } else {
                targetDir = "C:\\SteamCloudPatcherBackups";
            }
        }
        std::string sanitized = gameName;
        for (char& c : sanitized) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
                c = '_';
            }
        }
        std::string folderName = std::to_string(appId) + "_" + sanitized + "_" + GetTimestamp();
        fs::path dest = fs::path(targetDir) / folderName;
        fs::create_directories(dest);
        fs::copy(sourcePath, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        std::ofstream meta(dest / "_backup_meta.txt");
        if (meta.is_open()) {
            meta << "game=" << gameName << "\n";
            meta << "appId=" << appId << "\n";
            meta << "timestamp=" << GetTimestamp() << "\n";
            meta << "sourcePath=" << sourcePath << "\n";
            meta.close();
        }
    } catch (...) {}
}

inline void LoadConfig(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_configMutex);
    fs::path configPath = fs::path(dir) / "config.json";
    if (fs::exists(configPath)) {
        try {
            auto content = ReadFileContents(configPath);
            if (content) {
                auto j = json::parse(*content);
                g_autoPatch = j.value("autoPatch", false);
                g_autoProvider = j.value("provider", "");
                g_autoCustomPath = j.value("customPath", "");
                g_enableBackup = j.value("enableBackup", false);
                g_backupPath = j.value("backupPath", "");
                g_patcherPath = j.value("patcherPath", "");
                g_patchedAppIds.clear();
                if (j.contains("patchedGames") && j["patchedGames"].is_array()) {
                    for (const auto& item : j["patchedGames"]) {
                        if (item.is_number()) g_patchedAppIds.insert(item.get<uint32_t>());
                    }
                }
            }
        } catch (...) {}
    }
}

inline void SaveConfig(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_configMutex);
    fs::path configPath = fs::path(dir) / "config.json";
    try {
        json j = {
            {"autoPatch", g_autoPatch},
            {"provider", g_autoProvider},
            {"customPath", g_autoCustomPath},
            {"enableBackup", g_enableBackup},
            {"backupPath", g_backupPath},
            {"patcherPath", g_patcherPath},
            {"patchedGames", json::array()}
        };
        for (uint32_t aid : g_patchedAppIds) j["patchedGames"].push_back(aid);
        std::ofstream file(configPath);
        if (file.is_open()) file << j.dump(2);
    } catch (...) {}
}

inline bool IsInGreenLumaAppList(uint32_t appId, const std::string& steamPath) {
    fs::path appListDir = fs::path(steamPath) / "AppList";
    if (fs::exists(appListDir) && fs::is_directory(appListDir)) {
        for (const auto& entry : fs::directory_iterator(appListDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                try {
                    std::ifstream txt(entry.path());
                    std::string line;
                    if (std::getline(txt, line)) {
                        line.erase(0, line.find_first_not_of(" \t\r\n"));
                        line.erase(line.find_last_not_of(" \t\r\n") + 1);
                        if (!line.empty() && std::all_of(line.begin(), line.end(), ::isdigit)) {
                            uint32_t aid = std::stoul(line);
                            if (aid == appId) return true;
                        }
                    }
                } catch (...) {}
            }
        }
    }
    return false;
}

inline bool IsManifestSpoofed(uint32_t appId, const std::string& steamPath, const std::string& userId) {
    if (userId.empty() || userId == "0") return false;
    uint64_t accountId = 0;
    try {
        accountId = std::stoull(userId);
    } catch (...) {
        return false;
    }
    uint64_t userSteamID64 = 0x0110000100000000ULL | accountId;
    
    auto libs = FindSteamLibraryFolders(steamPath);
    for (const auto& folder : libs) {
        fs::path manifestPath = fs::path(folder) / "steamapps" / ("appmanifest_" + std::to_string(appId) + ".acf");
        if (fs::exists(manifestPath)) {
            auto contentOpt = ReadFileContents(manifestPath);
            if (contentOpt) {
                std::regex ownerRegex(R"raw("LastOwner"\s+"(\d+)")raw", std::regex_constants::icase);
                std::smatch match;
                if (std::regex_search(*contentOpt, match, ownerRegex)) {
                    try {
                        uint64_t lastOwner = std::stoull(match[1].str());
                        if (lastOwner != 0 && lastOwner != userSteamID64) {
                            return true;
                        }
                    } catch (...) {}
                } else {
                    return true;
                }
            }
        }
    }
    return false;
}

inline bool IsSteamToolsSpoofed(uint32_t appId, const std::string& steamPath) {
    std::string appIdStr = std::to_string(appId);
    fs::path configDir = fs::path(steamPath) / "config";
    std::vector<fs::path> dirsToCheck = { configDir / "stplugin", configDir / "lua", configDir / "stplugin" / "lua" };
    
    for (const auto& dir : dirsToCheck) {
        if (fs::exists(dir) && fs::is_directory(dir)) {
            try {
                for (const auto& entry : fs::directory_iterator(dir)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        if (filename.find(appIdStr) != std::string::npos) {
                            return true;
                        }
                        if (entry.path().extension() == ".lua" || entry.path().extension() == ".txt" || entry.path().extension() == ".json") {
                            std::ifstream file(entry.path());
                            std::string line;
                            while (std::getline(file, line)) {
                                if (line.find(appIdStr) != std::string::npos) {
                                    return true;
                                }
                            }
                        }
                    }
                }
            } catch (...) {}
        }
    }
    return false;
}

inline bool IsGameSpoofed(uint32_t appId) {
    if (g_steamPath.empty()) return false;
    if (IsInGreenLumaAppList(appId, g_steamPath)) return true;
    if (IsManifestSpoofed(appId, g_steamPath, g_steamUserId)) return true;
    if (IsSteamToolsSpoofed(appId, g_steamPath)) return true;
    return false;
}

bool IsGameTracked(uint32_t appId) {
    std::lock_guard<std::mutex> lock(g_configMutex);
    if (g_patchedAppIds.find(appId) != g_patchedAppIds.end()) return true;
    if (IsGameSpoofed(appId)) return true;
    return false;
}

void TriggerLaunchPatch(uint32_t appId) {
    g_lastLaunchSyncedAppId = appId;
    if (g_steamPath.empty() || g_autoProvider.empty()) return;
    auto list = DetectGames(g_steamPath, g_steamUserId);
    for (const auto& g : list) {
        if (g.appId == appId && !g.resolvedSavePath.empty()) {
            std::string target = GetProviderSavePath(g_autoProvider, g_autoCustomPath, g.name, g.appId);
            if (!target.empty()) {
                if (g_enableBackup) {
                    CreateBackup(g.resolvedSavePath, g.name, g.appId, g_backupPath);
                }
                SyncDirectories(g.resolvedSavePath, target);
            }
            std::string universalTarget = GetProviderSavePath("local", "", g.name, g.appId);
            if (!universalTarget.empty() && universalTarget != target) {
                SyncDirectories(g.resolvedSavePath, universalTarget);
            }
            TouchDirectoryFiles(g.resolvedSavePath);
            if (g_patchedAppIds.find(appId) == g_patchedAppIds.end()) {
                g_patchedAppIds.insert(appId);
                SaveConfig(g_steamPath);
            }
            break;
        }
    }
}

void TriggerExitPatch(uint32_t appId) {
    if (g_lastLaunchSyncedAppId == appId) {
        g_lastLaunchSyncedAppId = 0;
    }
    if (g_steamPath.empty() || g_autoProvider.empty()) return;
    auto list = DetectGames(g_steamPath, g_steamUserId);
    for (const auto& g : list) {
        if (g.appId == appId && !g.resolvedSavePath.empty()) {
            std::string target = GetProviderSavePath(g_autoProvider, g_autoCustomPath, g.name, g.appId);
            if (!target.empty()) {
                if (g_enableBackup) {
                    CreateBackup(g.resolvedSavePath, g.name, g.appId, g_backupPath);
                }
                SyncDirectories(g.resolvedSavePath, target);
            }
            std::string universalTarget = GetProviderSavePath("local", "", g.name, g.appId);
            if (!universalTarget.empty() && universalTarget != target) {
                SyncDirectories(g.resolvedSavePath, universalTarget);
            }
            std::string sPath = g_steamPath;
            std::string uId = g_steamUserId;
            std::string rPath = g.resolvedSavePath;
            std::thread([sPath, uId, appId, rPath]() {
                for (int i = 0; i < 5; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    TouchDirectoryFiles(rPath);
                }
            }).detach();
            break;
        }
    }
}

void MonitorThread() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam", 0, KEY_NOTIFY | KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    DWORD lastAppId = 0;
    while (g_dllRunning) {
        if (RegNotifyChangeKeyValue(hKey, TRUE, REG_NOTIFY_CHANGE_LAST_SET, nullptr, FALSE) != ERROR_SUCCESS) {
            break;
        }

        auto appVal = ReadRegistryDword(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam", "RunningAppID");
        if (appVal.has_value()) {
            DWORD currentAppId = *appVal;
            if (currentAppId != lastAppId) {
                if (currentAppId != 0) {
                    if (currentAppId != g_lastLaunchSyncedAppId && IsGameTracked(currentAppId)) {
                        TriggerLaunchPatch(currentAppId);
                    }
                } else if (lastAppId != 0) {
                    if (IsGameTracked(lastAppId)) TriggerExitPatch(lastAppId);
                }
                lastAppId = currentAppId;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    RegCloseKey(hKey);
}

void LaunchPatcher(const std::string& patcherPath) {
    try {
        if (patcherPath.empty() || !fs::exists(patcherPath)) return;
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        std::string dir = fs::path(patcherPath).parent_path().string();
        char cmd[MAX_PATH * 2];
        strcpy_s(cmd, patcherPath.c_str());
        strcat_s(cmd, " --silent");
        if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
            g_patcherProcess = pi.hProcess;
            CloseHandle(pi.hThread);
        }
    } catch (...) {
        LogDebug("LaunchPatcher: Exception caught!");
    }
}

void TerminatePatcher() {
    if (g_patcherProcess != nullptr) {
        TerminateProcess(g_patcherProcess, 0);
        CloseHandle(g_patcherProcess);
        g_patcherProcess = nullptr;
    }
}

typedef BOOL(WINAPI* CreateProcessW_t)(
    LPCWSTR lpApplicationName,
    LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation
);

static CreateProcessW_t OriginalCreateProcessW = CreateProcessW;

BOOL WINAPI HookedCreateProcessW(
    LPCWSTR lpApplicationName,
    LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation
) {
    auto appVal = ReadRegistryDword(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam", "RunningAppID");
    if (appVal.has_value() && *appVal != 0) {
        if (IsGameTracked(*appVal)) {
            TriggerLaunchPatch(*appVal);
        }
    }

    return OriginalCreateProcessW(
        lpApplicationName,
        lpCommandLine,
        lpProcessAttributes,
        lpThreadAttributes,
        bInheritHandles,
        dwCreationFlags,
        lpEnvironment,
        lpCurrentDirectory,
        lpStartupInfo,
        lpProcessInformation
    );
}

inline size_t FindSectionCaseInsensitive(const std::string& content, const std::string& sectionName, size_t startPos = 0) {
    std::string lowerContent = content;
    std::transform(lowerContent.begin(), lowerContent.end(), lowerContent.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::string lowerSec = sectionName;
    std::transform(lowerSec.begin(), lowerSec.end(), lowerSec.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    
    size_t pos = startPos;
    while (true) {
        pos = lowerContent.find("\"" + lowerSec + "\"", pos);
        if (pos == std::string::npos) break;
        
        size_t bracePos = content.find('{', pos);
        if (bracePos != std::string::npos) {
            bool onlyWhitespace = true;
            for (size_t i = pos + lowerSec.length() + 2; i < bracePos; ++i) {
                if (!std::isspace(static_cast<unsigned char>(content[i]))) {
                    onlyWhitespace = false;
                    break;
                }
            }
            if (onlyWhitespace) return pos;
        }
        pos += lowerSec.length() + 2;
    }
    return std::string::npos;
}

inline size_t FindMatchingBrace(const std::string& content, size_t openBracePos) {
    int depth = 1;
    size_t pos = openBracePos + 1;
    while (pos < content.length()) {
        if (content[pos] == '{') {
            depth++;
        } else if (content[pos] == '}') {
            depth--;
            if (depth == 0) {
                return pos;
            }
        }
        pos++;
    }
    return std::string::npos;
}

inline void SetKeyValueInRange(std::string& content, size_t startPos, size_t endPos, const std::string& key, const std::string& val, int indentLevel) {
    std::string rangeStr = content.substr(startPos, endPos - startPos);
    std::regex kvRegex("\"" + key + "\"\\s+\"([^\"]*)\"", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(rangeStr, match, kvRegex)) {
        size_t absMatchPos = startPos + match.position(0);
        std::string replacement = "\"" + key + "\"\t\t\"" + val + "\"";
        content.replace(absMatchPos, match.length(0), replacement);
    } else {
        std::string indent(indentLevel, '\t');
        std::string insertion = "\n" + indent + "\"" + key + "\"\t\t\"" + val + "\"";
        content.insert(startPos, insertion);
    }
}

inline bool PatchVdfContent(std::string& content, const std::set<uint32_t>& trackedAppIds) {
    if (trackedAppIds.empty()) return false;
    
    size_t appsPos = FindSectionCaseInsensitive(content, "apps");
    if (appsPos == std::string::npos) return false;
    
    size_t appsBracePos = content.find('{', appsPos);
    if (appsBracePos == std::string::npos) return false;
    
    bool changed = false;
    for (uint32_t appId : trackedAppIds) {
        size_t appsEndBracePos = FindMatchingBrace(content, appsBracePos);
        if (appsEndBracePos == std::string::npos) break;
        
        std::string appsBlock = content.substr(appsBracePos + 1, appsEndBracePos - appsBracePos - 1);
        std::string appIdStr = std::to_string(appId);
        size_t appPosInApps = FindSectionCaseInsensitive(appsBlock, appIdStr);
        
        if (appPosInApps != std::string::npos) {
            size_t appPos = appsBracePos + 1 + appPosInApps;
            size_t appBracePos = content.find('{', appPos);
            if (appBracePos != std::string::npos) {
                size_t appEndBracePos = FindMatchingBrace(content, appBracePos);
                if (appEndBracePos != std::string::npos) {
                    SetKeyValueInRange(content, appBracePos + 1, appEndBracePos, "cloudenabled", "1", 6);
                    appEndBracePos = FindMatchingBrace(content, appBracePos);
                    if (appEndBracePos != std::string::npos) {
                        SetKeyValueInRange(content, appBracePos + 1, appEndBracePos, "CloudEnabled", "1", 6);
                    }
                    changed = true;
                }
            }
        } else {
            std::string indent = "\t\t\t\t\t";
            std::string newSection = "\n" + indent + "\"" + appIdStr + "\"\n" + indent + "{\n" + indent + "\t\"cloudenabled\"\t\t\"1\"\n" + indent + "\t\"CloudEnabled\"\t\t\"1\"\n" + indent + "}";
            content.insert(appsBracePos + 1, newSection);
            changed = true;
        }
    }
    return changed;
}

inline void PatchConfigIfNeeded(const fs::path& configPath) {
    try {
        if (!fs::exists(configPath)) return;
        auto contentOpt = ReadFileContents(configPath);
        if (!contentOpt) return;
        
        std::string content = *contentOpt;
        std::set<uint32_t> appIdsToDisable;
        
        for (uint32_t aid : g_patchedAppIds) {
            appIdsToDisable.insert(aid);
        }
        
        if (g_autoPatch && !g_steamPath.empty()) {
            auto list = DetectGames(g_steamPath, g_steamUserId);
            for (const auto& g : list) {
                if (g.installed && IsGameSpoofed(g.appId)) {
                    appIdsToDisable.insert(g.appId);
                }
            }
        }
        
        fs::path appListDir = fs::path(g_steamPath) / "AppList";
        if (fs::exists(appListDir) && fs::is_directory(appListDir)) {
            for (const auto& entry : fs::directory_iterator(appListDir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                    try {
                        std::ifstream txt(entry.path());
                        std::string line;
                        if (std::getline(txt, line)) {
                            line.erase(0, line.find_first_not_of(" \t\r\n"));
                            line.erase(line.find_last_not_of(" \t\r\n") + 1);
                            if (!line.empty() && std::all_of(line.begin(), line.end(), ::isdigit)) {
                                uint32_t aid = std::stoul(line);
                                if (aid > 0) appIdsToDisable.insert(aid);
                            }
                        }
                    } catch (...) {}
                }
            }
        }
        
        if (PatchVdfContent(content, appIdsToDisable)) {
            SetFileAttributesW(configPath.wstring().c_str(), FILE_ATTRIBUTE_NORMAL);
            std::ofstream out(configPath, std::ios::binary);
            if (out.is_open()) {
                out << content;
            }
        }
    } catch (...) {}
}

void DisableSteamCloudUIAndErrors() {
    if (g_steamPath.empty()) return;
    std::string uId = g_steamUserId;
    if (uId.empty() || uId == "0") {
        fs::path userdataPath = fs::path(g_steamPath) / "userdata";
        if (fs::exists(userdataPath) && fs::is_directory(userdataPath)) {
            for (const auto& entry : fs::directory_iterator(userdataPath)) {
                if (entry.is_directory()) {
                    std::string dn = entry.path().filename().string();
                    if (!dn.empty() && dn != "0" && std::all_of(dn.begin(), dn.end(), ::isdigit)) {
                        uId = dn;
                        break;
                    }
                }
            }
        }
    }
    if (uId.empty() || uId == "0") return;
    
    fs::path sharedConfigPath = fs::path(g_steamPath) / "userdata" / uId / "7" / "remote" / "sharedconfig.vdf";
    fs::path localConfigPath = fs::path(g_steamPath) / "userdata" / uId / "config" / "localconfig.vdf";
    
    PatchConfigIfNeeded(sharedConfigPath);
    PatchConfigIfNeeded(localConfigPath);
}

typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL(WINAPI* CloseHandle_t)(HANDLE);
typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
typedef HMODULE(WINAPI* LoadLibraryA_t)(LPCSTR);
typedef HMODULE(WINAPI* LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);

static CreateFileW_t OriginalCreateFileW = CreateFileW;
static CreateFileA_t OriginalCreateFileA = CreateFileA;
static CloseHandle_t OriginalCloseHandle = CloseHandle;
static LoadLibraryW_t OriginalLoadLibraryW = LoadLibraryW;
static LoadLibraryExW_t OriginalLoadLibraryExW = LoadLibraryExW;
static LoadLibraryA_t OriginalLoadLibraryA = LoadLibraryA;
static LoadLibraryExA_t OriginalLoadLibraryExA = LoadLibraryExA;

static std::mutex g_handleMutex;
static std::set<HANDLE> g_trackedConfigHandles;
static std::map<HANDLE, std::wstring> g_handlePathMap;

inline bool IsSteamConfigPathW(const std::wstring& path, bool& isSharedConfig, bool& isLocalConfig) {
    isSharedConfig = false;
    isLocalConfig = false;
    std::wstring lowerPath = path;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), [](wchar_t c) { return (wchar_t)std::tolower(static_cast<int>(c)); });
    if (lowerPath.find(L"sharedconfig.vdf") != std::wstring::npos) {
        isSharedConfig = true;
        return true;
    }
    if (lowerPath.find(L"localconfig.vdf") != std::wstring::npos) {
        isLocalConfig = true;
        return true;
    }
    return false;
}

inline bool IsSteamConfigPathA(const std::string& path, bool& isSharedConfig, bool& isLocalConfig) {
    isSharedConfig = false;
    isLocalConfig = false;
    std::string lowerPath = path;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (lowerPath.find("sharedconfig.vdf") != std::string::npos) {
        isSharedConfig = true;
        return true;
    }
    if (lowerPath.find("localconfig.vdf") != std::string::npos) {
        isLocalConfig = true;
        return true;
    }
    return false;
}

HANDLE WINAPI HookedCreateFileW(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
) {
    if (lpFileName) {
        std::wstring path(lpFileName);
        bool isShared = false;
        bool isLocal = false;
        if (IsSteamConfigPathW(path, isShared, isLocal)) {
            if ((dwDesiredAccess & GENERIC_WRITE) == 0) {
                PatchConfigIfNeeded(path);
            }
            
            HANDLE hFile = OriginalCreateFileW(
                lpFileName,
                dwDesiredAccess,
                dwShareMode,
                lpSecurityAttributes,
                dwCreationDisposition,
                dwFlagsAndAttributes,
                hTemplateFile
            );
            
            if (hFile != INVALID_HANDLE_VALUE && (dwDesiredAccess & GENERIC_WRITE)) {
                std::lock_guard<std::mutex> lock(g_handleMutex);
                g_trackedConfigHandles.insert(hFile);
                g_handlePathMap[hFile] = path;
            }
            return hFile;
        }
    }
    return OriginalCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

HANDLE WINAPI HookedCreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile
) {
    if (lpFileName) {
        std::string path(lpFileName);
        bool isShared = false;
        bool isLocal = false;
        if (IsSteamConfigPathA(path, isShared, isLocal)) {
            if ((dwDesiredAccess & GENERIC_WRITE) == 0) {
                PatchConfigIfNeeded(path);
            }
            
            HANDLE hFile = OriginalCreateFileA(
                lpFileName,
                dwDesiredAccess,
                dwShareMode,
                lpSecurityAttributes,
                dwCreationDisposition,
                dwFlagsAndAttributes,
                hTemplateFile
            );
            
            if (hFile != INVALID_HANDLE_VALUE && (dwDesiredAccess & GENERIC_WRITE)) {
                std::lock_guard<std::mutex> lock(g_handleMutex);
                g_trackedConfigHandles.insert(hFile);
                std::wstring wpath(path.begin(), path.end());
                g_handlePathMap[hFile] = wpath;
            }
            return hFile;
        }
    }
    return OriginalCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

BOOL WINAPI HookedCloseHandle(HANDLE hObject) {
    bool isConfigHandle = false;
    std::wstring configPath;
    {
        std::lock_guard<std::mutex> lock(g_handleMutex);
        auto it = g_trackedConfigHandles.find(hObject);
        if (it != g_trackedConfigHandles.end()) {
            isConfigHandle = true;
            g_trackedConfigHandles.erase(it);
            configPath = g_handlePathMap[hObject];
            g_handlePathMap.erase(hObject);
        }
    }

    BOOL result = OriginalCloseHandle(hObject);
    if (isConfigHandle && !configPath.empty()) {
        PatchConfigIfNeeded(configPath);
    }
    return result;
}

void HookAllModulesIAT(const char* targetDllName, const char* functionName, PROC hookFunction, PROC* originalFunction) {
    HANDLE hProcess = GetCurrentProcess();
    HMODULE hMods[1024];
    DWORD cbNeeded;
    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        int numModules = cbNeeded / sizeof(HMODULE);
        for (int i = 0; i < numModules; i++) {
            char szModName[MAX_PATH];
            if (GetModuleFileNameExA(hProcess, hMods[i], szModName, sizeof(szModName))) {
                std::string modName = fs::path(szModName).filename().string();
                std::transform(modName.begin(), modName.end(), modName.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (modName == "kernelbase.dll" || modName == "kernel32.dll" || modName == "ntdll.dll" || modName == "version.dll") {
                    continue;
                }
                
                PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hMods[i];
                if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) continue;
                PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)hMods[i] + dosHeader->e_lfanew);
                if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) continue;
                IMAGE_DATA_DIRECTORY importDirectory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
                if (importDirectory.Size == 0) continue;
                PIMAGE_IMPORT_DESCRIPTOR importDescriptor = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hMods[i] + importDirectory.VirtualAddress);

                while (importDescriptor->Name) {
                    const char* dllName = (const char*)((BYTE*)hMods[i] + importDescriptor->Name);
                    if (_stricmp(dllName, targetDllName) == 0) {
                        PIMAGE_THUNK_DATA thunk = (PIMAGE_THUNK_DATA)((BYTE*)hMods[i] + importDescriptor->FirstThunk);
                        PIMAGE_THUNK_DATA originalThunk = (PIMAGE_THUNK_DATA)((BYTE*)hMods[i] + importDescriptor->OriginalFirstThunk);
                        
                        while (thunk->u1.Function) {
                            PROC* fnAddress = (PROC*)&thunk->u1.Function;
                            if (*fnAddress == hookFunction) {
                                thunk++;
                                if (originalThunk) originalThunk++;
                                continue;
                            }
                            
                            PROC targetFuncAddr = (PROC)GetProcAddress(GetModuleHandleA(targetDllName), functionName);
                            if (targetFuncAddr && *fnAddress == targetFuncAddr) {
                                if (*originalFunction == nullptr || *originalFunction == targetFuncAddr) {
                                    *originalFunction = *fnAddress;
                                }
                                DWORD oldProtect;
                                if (VirtualProtect(fnAddress, sizeof(PROC), PAGE_READWRITE, &oldProtect)) {
                                    *fnAddress = hookFunction;
                                    VirtualProtect(fnAddress, sizeof(PROC), oldProtect, &oldProtect);
                                }
                            }
                            thunk++;
                            if (originalThunk) originalThunk++;
                        }
                    }
                    importDescriptor++;
                }
            }
        }
    }
}

void ApplyAllHooks();

HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE hMod = OriginalLoadLibraryW(lpLibFileName);
    if (hMod) ApplyAllHooks();
    return hMod;
}

HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hMod = OriginalLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (hMod) ApplyAllHooks();
    return hMod;
}

HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE hMod = OriginalLoadLibraryA(lpLibFileName);
    if (hMod) ApplyAllHooks();
    return hMod;
}

HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hMod = OriginalLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (hMod) ApplyAllHooks();
    return hMod;
}

typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;
typedef uint32_t uint32;

enum ERemoteStorageSyncState
{
    k_ERemoteSyncStateDisabled = 0,
    k_ERemoteSyncStateUnknown = 1,
    k_ERemoteSyncStateSynchronized = 2,
    k_ERemoteSyncStateSyncInProgress = 3,
    k_ERemoteSyncStatePendingChangesInCloud = 4,
    k_ERemoteSyncStatePendingChangesLocally = 5,
    k_ERemoteSyncStatePendingChangesInCloudAndLocally = 6,
    k_ERemoteSyncStateConflictingChanges = 7,
};

#define VTIDX_IsCloudEnabledForApp 19
#define VTIDX_EvaluateRemoteStorageSyncState 63
#define VTIDX_GetRemoteStorageSyncState 64
#define VTIDX_HaveLatestFilesLocally 65
#define VTIDX_GetConflictingFileTimestamps 66
#define VTIDX_ResolveSyncConflict 67
#define VTIDX_SynchronizeApp 68
#define VTIDX_IsAppSyncInProgress 69
#define VTIDX_RunAutoCloudOnAppLaunch 70
#define VTIDX_RunAutoCloudOnAppExit 71
static constexpr int VTIDX_GetIClientRemoteStorage      = 23;
inline void LogDebug(const std::string& msg) {
    try {
        static fs::path logPath;
        static bool pathResolved = false;
        if (!pathResolved) {
            char* localAppData = nullptr;
            size_t len = 0;
            if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") == 0 && localAppData != nullptr) {
                logPath = fs::path(localAppData) / "SteamCloudPatcher" / "patcher_dll.log";
                free(localAppData);
            } else {
                logPath = "C:\\SteamCloudPatcher\\patcher_dll.log";
            }
            fs::create_directories(logPath.parent_path());
            pathResolved = true;
        }
        std::ofstream logFile(logPath, std::ios::app);
        if (logFile.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            std::tm tm_buf{};
            localtime_s(&tm_buf, &time);
            logFile << std::put_time(&tm_buf, "[%Y-%m-%d %H:%M:%S] ") << msg << "\n";
            logFile.close();
        }
    } catch (...) {}
}
static PROC HookVTableMethodSEH(void* interfacePtr, int methodIndex, PROC hookFunc, DWORD* pExceptionCode) {
    *pExceptionCode = 0;
    __try {
        void** vtable = *(void***)interfacePtr;
        if (!vtable) return nullptr;
        PROC original = (PROC)vtable[methodIndex];
        if (original == hookFunc) return original;
        
        DWORD oldProtect;
        if (VirtualProtect(&vtable[methodIndex], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            vtable[methodIndex] = (void*)hookFunc;
            VirtualProtect(&vtable[methodIndex], sizeof(void*), oldProtect, &oldProtect);
        }
        return original;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *pExceptionCode = GetExceptionCode();
        return nullptr;
    }
}

inline PROC HookVTableMethod(void* interfacePtr, int methodIndex, PROC hookFunc) {
    if (!interfacePtr) return nullptr;
    
    DWORD exCode = 0;
    PROC result = HookVTableMethodSEH(interfacePtr, methodIndex, hookFunc, &exCode);
    
    if (exCode != 0) {
        char buf[64];
        sprintf_s(buf, "HookVTableMethod: CRASH at vtable index %d, ex=0x%08X", methodIndex, exCode);
        LogDebug(buf);
        return nullptr;
    }
    
    if (result) {
        LogDebug("HookVTableMethod: Hooked vtable index " + std::to_string(methodIndex));
    }
    return result;
}
typedef bool (*IsCloudEnabledForApp_t)(void* self, uint32 nAppId);
static IsCloudEnabledForApp_t OriginalIsCloudEnabledForApp = nullptr;

typedef ERemoteStorageSyncState (*GetRemoteStorageSyncState_t)(void* self, uint32 nAppId);
static GetRemoteStorageSyncState_t OriginalGetRemoteStorageSyncState = nullptr;

typedef bool (*HaveLatestFilesLocally_t)(void* self, uint32 nAppId);
static HaveLatestFilesLocally_t OriginalHaveLatestFilesLocally = nullptr;

typedef bool (*IsAppSyncInProgress_t)(void* self, uint32 nAppId);
static IsAppSyncInProgress_t OriginalIsAppSyncInProgress = nullptr;

typedef void (*RunAutoCloudOnAppLaunch_t)(void* self, uint32 nAppId);
static RunAutoCloudOnAppLaunch_t OriginalRunAutoCloudOnAppLaunch = nullptr;

typedef void (*RunAutoCloudOnAppExit_t)(void* self, uint32 nAppId);
static RunAutoCloudOnAppExit_t OriginalRunAutoCloudOnAppExit = nullptr;

typedef bool (*GetConflictingFileTimestamps_t)(void* self, uint32 nAppId, uint32* pnTimestampLocal, uint32* pnTimestampRemote);
static GetConflictingFileTimestamps_t OriginalGetConflictingFileTimestamps = nullptr;

typedef bool (*ResolveSyncConflict_t)(void* self, uint32 nAppId, bool bAcceptLocalFiles);
static ResolveSyncConflict_t OriginalResolveSyncConflict = nullptr;

static PROC g_originalEvaluateRemoteStorageSyncState = nullptr;
static PROC g_originalSynchronizeApp = nullptr;
static std::atomic<bool> g_remoteStorageHooked{false};
bool HookedIsCloudEnabledForApp(void* self, uint32 nAppId) {
    bool tracked = IsGameTracked(nAppId);
    LogDebug("IsCloudEnabledForApp called for AppID " + std::to_string(nAppId) + ", tracked=" + (tracked ? "true" : "false"));
    if (tracked) {
        return true;
    }
    if (OriginalIsCloudEnabledForApp) {
        return OriginalIsCloudEnabledForApp(self, nAppId);
    }
    return true;
}

ERemoteStorageSyncState HookedGetRemoteStorageSyncState(void* self, uint32 nAppId) {
    bool tracked = IsGameTracked(nAppId);
    LogDebug("GetRemoteStorageSyncState called for AppID " + std::to_string(nAppId) + ", tracked=" + (tracked ? "true" : "false"));
    if (tracked) {
        return k_ERemoteSyncStateSynchronized;
    }
    if (OriginalGetRemoteStorageSyncState) {
        return OriginalGetRemoteStorageSyncState(self, nAppId);
    }
    return k_ERemoteSyncStateSynchronized;
}

bool HookedHaveLatestFilesLocally(void* self, uint32 nAppId) {
    bool tracked = IsGameTracked(nAppId);
    LogDebug("HaveLatestFilesLocally called for AppID " + std::to_string(nAppId) + ", tracked=" + (tracked ? "true" : "false"));
    if (tracked) {
        return true;
    }
    if (OriginalHaveLatestFilesLocally) {
        return OriginalHaveLatestFilesLocally(self, nAppId);
    }
    return true;
}

bool HookedIsAppSyncInProgress(void* self, uint32 nAppId) {
    bool tracked = IsGameTracked(nAppId);
    LogDebug("IsAppSyncInProgress called for AppID " + std::to_string(nAppId) + ", tracked=" + (tracked ? "true" : "false"));
    if (tracked) {
        return false;
    }
    if (OriginalIsAppSyncInProgress) {
        return OriginalIsAppSyncInProgress(self, nAppId);
    }
    return false;
}

void HookedRunAutoCloudOnAppLaunch(void* self, uint32 nAppId) {
    bool tracked = IsGameTracked(nAppId);
    LogDebug("RunAutoCloudOnAppLaunch called for AppID " + std::to_string(nAppId) + ", tracked=" + (tracked ? "true" : "false"));
    if (tracked) {
        TriggerLaunchPatch(nAppId);
        return;
    }
    if (OriginalRunAutoCloudOnAppLaunch) {
        OriginalRunAutoCloudOnAppLaunch(self, nAppId);
    }
}

void HookedRunAutoCloudOnAppExit(void* self, uint32 nAppId) {
    bool tracked = IsGameTracked(nAppId);
    LogDebug("RunAutoCloudOnAppExit called for AppID " + std::to_string(nAppId) + ", tracked=" + (tracked ? "true" : "false"));
    if (tracked) {
        TriggerExitPatch(nAppId);
        return;
    }
    if (OriginalRunAutoCloudOnAppExit) {
        OriginalRunAutoCloudOnAppExit(self, nAppId);
    }
}

void HookedEvaluateRemoteStorageSyncState(void* self, uint32 nAppId, bool bUnk) {
    bool tracked = IsGameTracked(nAppId);
    LogDebug("EvaluateRemoteStorageSyncState called for AppID " + std::to_string(nAppId) + ", tracked=" + (tracked ? "true" : "false"));
    if (tracked) {
        return;
    }
    if (g_originalEvaluateRemoteStorageSyncState) {
        ((void(*)(void*, uint32, bool))g_originalEvaluateRemoteStorageSyncState)(self, nAppId, bUnk);
    }
}

bool HookedSynchronizeApp(void* self, uint32 nAppId, bool bSyncClient, bool bSyncServer) {
    bool tracked = IsGameTracked(nAppId);
    LogDebug("SynchronizeApp called for AppID " + std::to_string(nAppId) + ", tracked=" + (tracked ? "true" : "false"));
    if (tracked) {
        TriggerLaunchPatch(nAppId);
        return true;
    }
    if (g_originalSynchronizeApp) {
        return ((bool(*)(void*, uint32, bool, bool))g_originalSynchronizeApp)(self, nAppId, bSyncClient, bSyncServer);
    }
    return true;
}

bool HookedGetConflictingFileTimestamps(void* self, uint32 nAppId, uint32* pnTimestampLocal, uint32* pnTimestampRemote) {
    bool tracked = IsGameTracked(nAppId);
    LogDebug("GetConflictingFileTimestamps called for AppID " + std::to_string(nAppId) + ", tracked=" + (tracked ? "true" : "false"));
    if (tracked) {
        if (pnTimestampLocal) *pnTimestampLocal = 0;
        if (pnTimestampRemote) *pnTimestampRemote = 0;
        return false;
    }
    if (OriginalGetConflictingFileTimestamps) {
        return OriginalGetConflictingFileTimestamps(self, nAppId, pnTimestampLocal, pnTimestampRemote);
    }
    return false;
}

bool HookedResolveSyncConflict(void* self, uint32 nAppId, bool bAcceptLocalFiles) {
    bool tracked = IsGameTracked(nAppId);
    LogDebug("ResolveSyncConflict called for AppID " + std::to_string(nAppId) + ", tracked=" + (tracked ? "true" : "false"));
    if (tracked) {
        return true;
    }
    if (OriginalResolveSyncConflict) {
        return OriginalResolveSyncConflict(self, nAppId, bAcceptLocalFiles);
    }
    return true;
}
static void* g_lastClientRemoteStorage = nullptr;
static bool HookIClientRemoteStorageSEH(void* remoteStorage, DWORD* pExceptionCode) {
    *pExceptionCode = 0;
    __try {
        void** vtable = *(void***)remoteStorage;
        (void)vtable;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *pExceptionCode = GetExceptionCode();
        return false;
    }
}

void HookIClientRemoteStorage(void* remoteStorage) {
    if (!remoteStorage) return;
    if (remoteStorage == g_lastClientRemoteStorage && g_remoteStorageHooked.load()) return;
    DWORD exCode = 0;
    if (!HookIClientRemoteStorageSEH(remoteStorage, &exCode)) {
        char buf[64];
        sprintf_s(buf, "HookIClientRemoteStorage: Invalid pointer, ex=0x%08X", exCode);
        LogDebug(buf);
        return;
    }
    
    g_lastClientRemoteStorage = remoteStorage;
    
    LogDebug("HookIClientRemoteStorage: Hooking IClientRemoteStorage at " + ([](void* p) {
        char buf[32]; sprintf_s(buf, "0x%p", p); return std::string(buf);
    })(remoteStorage));
    OriginalIsCloudEnabledForApp = (IsCloudEnabledForApp_t)HookVTableMethod(remoteStorage, VTIDX_IsCloudEnabledForApp, (PROC)HookedIsCloudEnabledForApp);
    g_originalEvaluateRemoteStorageSyncState = HookVTableMethod(remoteStorage, VTIDX_EvaluateRemoteStorageSyncState, (PROC)HookedEvaluateRemoteStorageSyncState);
    OriginalGetRemoteStorageSyncState = (GetRemoteStorageSyncState_t)HookVTableMethod(remoteStorage, VTIDX_GetRemoteStorageSyncState, (PROC)HookedGetRemoteStorageSyncState);
    OriginalHaveLatestFilesLocally = (HaveLatestFilesLocally_t)HookVTableMethod(remoteStorage, VTIDX_HaveLatestFilesLocally, (PROC)HookedHaveLatestFilesLocally);
    OriginalGetConflictingFileTimestamps = (GetConflictingFileTimestamps_t)HookVTableMethod(remoteStorage, VTIDX_GetConflictingFileTimestamps, (PROC)HookedGetConflictingFileTimestamps);
    OriginalResolveSyncConflict = (ResolveSyncConflict_t)HookVTableMethod(remoteStorage, VTIDX_ResolveSyncConflict, (PROC)HookedResolveSyncConflict);
    g_originalSynchronizeApp = HookVTableMethod(remoteStorage, VTIDX_SynchronizeApp, (PROC)HookedSynchronizeApp);
    OriginalIsAppSyncInProgress = (IsAppSyncInProgress_t)HookVTableMethod(remoteStorage, VTIDX_IsAppSyncInProgress, (PROC)HookedIsAppSyncInProgress);
    OriginalRunAutoCloudOnAppLaunch = (RunAutoCloudOnAppLaunch_t)HookVTableMethod(remoteStorage, VTIDX_RunAutoCloudOnAppLaunch, (PROC)HookedRunAutoCloudOnAppLaunch);
    OriginalRunAutoCloudOnAppExit = (RunAutoCloudOnAppExit_t)HookVTableMethod(remoteStorage, VTIDX_RunAutoCloudOnAppExit, (PROC)HookedRunAutoCloudOnAppExit);
    
    g_remoteStorageHooked.store(true);
    LogDebug("HookIClientRemoteStorage: ALL hooks applied successfully!");
}

typedef void* (*GetIClientRemoteStorage_t)(void* self, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion);
static GetIClientRemoteStorage_t OriginalGetIClientRemoteStorage = nullptr;

void* HookedGetIClientRemoteStorage(void* self, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion) {
    LogDebug("GetIClientRemoteStorage called with version: " + std::string(pchVersion ? pchVersion : "(null)"));
    void* result = nullptr;
    if (OriginalGetIClientRemoteStorage) {
        result = OriginalGetIClientRemoteStorage(self, hSteamUser, hSteamPipe, pchVersion);
    }
    if (result) {
        LogDebug("GetIClientRemoteStorage returned interface, hooking it now...");
        HookIClientRemoteStorage(result);
    } else {
        LogDebug("GetIClientRemoteStorage returned NULL!");
    }
    return result;
}

static void* g_lastClientEngine = nullptr;
static bool HookIClientEngineSEH(void* clientEngine, int vtableIndex, PROC hookFunc, PROC* pOriginal, DWORD* pExceptionCode) {
    *pExceptionCode = 0;
    __try {
        void** vtable = *(void***)clientEngine;
        if (!vtable) return false;
        *pOriginal = (PROC)vtable[vtableIndex];
        if (*pOriginal == hookFunc) return true;
        DWORD oldProtect;
        if (VirtualProtect(&vtable[vtableIndex], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            vtable[vtableIndex] = (void*)hookFunc;
            VirtualProtect(&vtable[vtableIndex], sizeof(void*), oldProtect, &oldProtect);
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *pExceptionCode = GetExceptionCode();
        return false;
    }
}

static void SafeProactiveGetIClientRemoteStorage(void* clientEngine) {
    if (!OriginalGetIClientRemoteStorage) return;

    const char* versions[] = {
        "CLIENTREMOTESTORAGE_INTERFACE_VERSION001",
        "CLIENTREMOTESTORAGE_INTERFACE_VERSION002",
        "CLIENTREMOTESTORAGE_INTERFACE_VERSION003",
        nullptr
    };

    for (int pipe = 1; pipe <= 10; pipe++) {
        for (int user = 1; user <= 10; user++) {
            for (int i = 0; versions[i]; i++) {
                void* remoteStorage = OriginalGetIClientRemoteStorage(clientEngine, user, pipe, versions[i]);
                if (remoteStorage) {
                    char buf[128];
                    sprintf_s(buf, "SafeProactiveGetIClientRemoteStorage: Got IClientRemoteStorage for %s with pipe=%d user=%d", versions[i], pipe, user);
                    LogDebug(buf);
                    HookIClientRemoteStorage(remoteStorage);
                    return;
                }
            }
        }
    }
    LogDebug("SafeProactiveGetIClientRemoteStorage: Failed to brute force pipe and user.");
}

void HookIClientEngine(void* clientEngine) {
    if (!clientEngine || clientEngine == g_lastClientEngine) return;
    g_lastClientEngine = clientEngine;
    LogDebug("HookIClientEngine: Hooking GetIClientRemoteStorage at vtable index " + std::to_string(VTIDX_GetIClientRemoteStorage));
    
    DWORD exCode = 0;
    PROC original = nullptr;
    if (HookIClientEngineSEH(clientEngine, VTIDX_GetIClientRemoteStorage, (PROC)HookedGetIClientRemoteStorage, &original, &exCode)) {
        OriginalGetIClientRemoteStorage = (GetIClientRemoteStorage_t)original;
        LogDebug("HookIClientEngine: Success! OriginalGetIClientRemoteStorage = " + ([](void* p) {
            char buf[32]; sprintf_s(buf, "0x%p", p); return std::string(buf);
        })((void*)OriginalGetIClientRemoteStorage));
        if (OriginalGetIClientRemoteStorage && !g_remoteStorageHooked.load()) {
            LogDebug("HookIClientEngine: Proactively calling GetIClientRemoteStorage...");
            SafeProactiveGetIClientRemoteStorage(clientEngine);
            if (g_remoteStorageHooked.load()) {
                LogDebug("HookIClientEngine: Proactive call succeeded!");
            } else {
                LogDebug("HookIClientEngine: Proactive call did not hook interface.");
            }
        }
    } else {
        char buf[64];
        sprintf_s(buf, "HookIClientEngine: CRASH, ex=0x%08X", exCode);
        LogDebug(buf);
    }
}
typedef void* (*CreateInterface_t)(const char* pName, int* pReturnCode);
static CreateInterface_t OriginalCreateInterface = nullptr;

static HMODULE GetSteamClientModule() {
    HMODULE h = GetModuleHandleA("steamclient64.dll");
    if (!h) h = GetModuleHandleA("steamclient.dll");
    return h;
}
static void* TryCallCreateInterface(CreateInterface_t fn, const char* name, DWORD* pExceptionCode) {
    *pExceptionCode = 0;
    __try {
        int returnCode = 0;
        return fn(name, &returnCode);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *pExceptionCode = GetExceptionCode();
        return nullptr;
    }
}

#include <psapi.h>

static bool g_cloudMemoryPatched = false;

void ApplyCloudFixMemoryPatch() {
    if (g_cloudMemoryPatched) return;
    
    HMODULE hSteamClient = GetSteamClientModule();
    if (!hSteamClient) return;

    MODULEINFO modInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hSteamClient, &modInfo, sizeof(modInfo))) return;

    uint8_t* base = (uint8_t*)modInfo.lpBaseOfDll;
    DWORD size = modInfo.SizeOfImage;

    for (DWORD i = 0; i < size - 17; i++) {
        if (base[i] == 0x85 && base[i+1] == 0xC0 &&
            base[i+2] == 0x0F && base[i+3] == 0x85) 
        {
            if (base[i+8] == 0x45 && base[i+9] == 0x85 && base[i+10] == 0xFF &&
                base[i+11] == 0x0F && base[i+12] == 0x84) 
            {
                DWORD oldProtect;
                if (VirtualProtect(base + i + 11, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    base[i+11] = 0x90;
                    base[i+12] = 0xE9;
                    VirtualProtect(base + i + 11, 2, oldProtect, &oldProtect);
                    LogDebug("ApplyCloudFixMemoryPatch: Successfully applied old Cloud Rewrite Skip patch!");
                    g_cloudMemoryPatched = true;
                    return;
                }
            }
            else if (base[i+8] == 0x40 && base[i+9] == 0x84 && base[i+10] == 0xFF &&
                     base[i+11] == 0x74) 
            {
                DWORD oldProtect;
                if (VirtualProtect(base + i + 11, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    base[i+11] = 0xEB;
                    VirtualProtect(base + i + 11, 1, oldProtect, &oldProtect);
                    LogDebug("ApplyCloudFixMemoryPatch: Successfully applied NEW Cloud Rewrite Skip patch!");
                    g_cloudMemoryPatched = true;
                    return;
                }
            }
            else if (base[i+8] == 0x40 && base[i+9] == 0x84 && base[i+10] == 0xFF &&
                     base[i+11] == 0xEB) 
            {
                LogDebug("ApplyCloudFixMemoryPatch: NEW Patch already applied.");
                g_cloudMemoryPatched = true;
                return;
            }
        }
    }
    LogDebug("ApplyCloudFixMemoryPatch: Pattern not found.");
}

void TryDirectRemoteStorageHook() {
    ApplyCloudFixMemoryPatch();
    if (g_remoteStorageHooked.load()) return;
    if (!OriginalCreateInterface) {
        HMODULE hSteamclient = GetSteamClientModule();
        if (hSteamclient) {
            FARPROC fn = ::GetProcAddress(hSteamclient, "CreateInterface");
            if (fn) {
                OriginalCreateInterface = (CreateInterface_t)fn;
                LogDebug("TryDirectRemoteStorageHook: Found CreateInterface in steamclient!");
            } else {
                return;
            }
        } else {
            return;
        }
    }

    LogDebug("TryDirectRemoteStorageHook: Attempting to get IClientRemoteStorage...");
    void* engine = OriginalCreateInterface("CLIENTENGINE_INTERFACE_VERSION005", nullptr);
    if (engine) {
        HookIClientEngine(engine);
    }
    
    if (g_lastClientEngine && !g_remoteStorageHooked.load()) {
        SafeProactiveGetIClientRemoteStorage(g_lastClientEngine);
    }
    
    if (!g_remoteStorageHooked.load()) {
        LogDebug("TryDirectRemoteStorageHook: Still no IClientRemoteStorage found.");
    }
}

void* HookedCreateInterface(const char* pName, int* pReturnCode) {
    if (!OriginalCreateInterface) {
        HMODULE hSteamclient = GetSteamClientModule();
        if (hSteamclient) {
            OriginalCreateInterface = (CreateInterface_t)GetProcAddress(hSteamclient, "CreateInterface");
        }
    }
    if (!OriginalCreateInterface) return nullptr;
    
    void* result = OriginalCreateInterface(pName, pReturnCode);
    if (result && pName) {
        std::string interfaceName(pName);
        LogDebug("CreateInterface called for: " + interfaceName + " -> " + ([](void* p) {
            char buf[32]; sprintf_s(buf, "0x%p", p); return std::string(buf);
        })(result));
        
        if (interfaceName.rfind("CLIENTREMOTESTORAGE_INTERFACE_VERSION", 0) == 0) {
            HookIClientRemoteStorage(result);
        } else if (interfaceName.rfind("CLIENTENGINE_INTERFACE_VERSION", 0) == 0) {
            HookIClientEngine(result);
        }
    }
    return result;
}

typedef FARPROC(WINAPI* GetProcAddress_t)(HMODULE, LPCSTR);
static GetProcAddress_t OriginalGetProcAddress = ::GetProcAddress;

FARPROC WINAPI HookedGetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    if (OriginalGetProcAddress) {
        FARPROC result = OriginalGetProcAddress(hModule, lpProcName);
        if (!lpProcName || ((ULONG_PTR)lpProcName < 0x10000)) {
            return result;
        }

        if (result) {
            if (strcmp(lpProcName, "CreateInterface") == 0) {
                char szModName[MAX_PATH];
                if (GetModuleFileNameA(hModule, szModName, sizeof(szModName))) {
                    std::string modName = fs::path(szModName).filename().string();
                    std::transform(modName.begin(), modName.end(), modName.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    LogDebug("GetProcAddress: CreateInterface from " + modName);
                    if (modName == "steamclient.dll" || modName == "steamclient64.dll") {
                        LogDebug(">>> INTERCEPTED CreateInterface from " + modName + " <<<");
                        OriginalCreateInterface = (CreateInterface_t)result;
                        return (FARPROC)HookedCreateInterface;
                    }
                }
            }
        }

        return result;
    }
    return ::GetProcAddress(hModule, lpProcName);
}
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

typedef HANDLE(WINAPI* FindFirstFileW_t)(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);
typedef HANDLE(WINAPI* FindFirstFileExW_t)(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags);

static FindFirstFileW_t OriginalFindFirstFileW = nullptr;
static FindFirstFileExW_t OriginalFindFirstFileExW = nullptr;

typedef BOOL(WINAPI* WriteFile_t)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static WriteFile_t OriginalWriteFile = nullptr;

thread_local uint32_t t_currentAppId = 0;

BOOL WINAPI HookedWriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped) {
    if (lpBuffer && nNumberOfBytesToWrite > 15) {
        const char* buf = (const char*)lpBuffer;
        // Optimization: Steam logs usually start with '[' (e.g. "[2026-06-12...")
        if (buf[0] == '[') {
            const char* appIdStr = strstr(buf, "[AppID ");
            if (appIdStr) {
                t_currentAppId = (uint32_t)atoi(appIdStr + 7);
                
                // Auto-Detect "Access Denied" errors and automatically add to patched list!
                if (t_currentAppId != 0 && strstr(buf, "Upload Access Denied")) {
                    bool needsAdd = false;
                    {
                        std::lock_guard<std::mutex> lock(g_configMutex);
                        if (g_patchedAppIds.find(t_currentAppId) == g_patchedAppIds.end()) {
                            g_patchedAppIds.insert(t_currentAppId);
                            needsAdd = true;
                        }
                    }
                    if (needsAdd) {
                        SaveConfig(g_steamPath);
                        LogDebug("AUTO-DETECTED CLOUD ERROR! Automatically patched AppID " + std::to_string(t_currentAppId));
                        
                        // Delete the remotecache.vdf asynchronously so it fixes on the very next launch
                        std::thread([](uint32_t appId, std::string steamPath, std::string steamUserId) {
                            std::this_thread::sleep_for(std::chrono::seconds(5)); // Wait for Steam to finish current sync
                            fs::path cacheFile = fs::path(steamPath) / "userdata" / steamUserId / std::to_string(appId) / "remotecache.vdf";
                            std::error_code ec;
                            fs::remove(cacheFile, ec);
                        }, t_currentAppId, g_steamPath, g_steamUserId).detach();
                    }
                }
            }
        }
    }
    return OriginalWriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
}

bool ShouldHideFromSteamCloud(LPCWSTR lpFileName) {
    if (!lpFileName) return false;
    
    // IF THIS THREAD IS NOT LOGGING ABOUT A PATCHED OR SPOOFED APPID, ALLOW EVERYTHING (Fixes CS2 and legit games losing saves)
    if (t_currentAppId == 0 || !IsGameTracked(t_currentAppId)) {
        return false;
    }

    wchar_t absPath[MAX_PATH];
    if (GetFullPathNameW(lpFileName, MAX_PATH, absPath, nullptr) == 0) {
        wcscpy_s(absPath, MAX_PATH, lpFileName);
    }

    std::wstring path = absPath;
    std::transform(path.begin(), path.end(), path.begin(), ::towlower);
    
    // EXCEPTION: Always allow Steam's own core directories so Steam doesn't break
    // We allow \steam\ and \steamapps\ explicitly, but we will override this exception for save folders below.
    bool isSteamDir = (path.find(L"\\steam\\") != std::wstring::npos) || 
                      (path.find(L"\\steamapps\\") != std::wstring::npos);
                      
    // EXCEPTION: Always allow Steam userdata (where Steam caches cloud states)
    if (path.find(L"\\steam\\userdata") != std::wstring::npos || path.find(L"\\program files (x86)\\steam\\userdata") != std::wstring::npos) {
        return false;
    }
    
    // UNIVERSAL SAVE LOCATION BLINDER
    // We blind all standard Windows user directories where games are forced to save
    bool isUserLoc = (path.find(L"\\appdata\\") != std::wstring::npos) ||
                     (path.find(L"\\documents\\") != std::wstring::npos) ||
                     (path.find(L"\\saved games\\") != std::wstring::npos) ||
                     (path.find(L"\\my games\\") != std::wstring::npos) ||
                     (path.find(L"\\programdata\\") != std::wstring::npos) ||
                     (path.find(L"\\users\\public\\") != std::wstring::npos);
                     
    // We aggressively blind ANY path that specifically targets a "save" folder, even if it's inside steamapps!
    // This catches games that save inside their own installation folders.
    bool isSaveKeyword = (path.find(L"\\saves\\") != std::wstring::npos) ||
                         (path.find(L"\\save\\") != std::wstring::npos) ||
                         (path.find(L"\\savegames\\") != std::wstring::npos) ||
                         (path.find(L"\\save_games\\") != std::wstring::npos) ||
                         (path.find(L"\\saved\\") != std::wstring::npos);
                         
    if (isUserLoc || isSaveKeyword) {
        // If it's a known user loc or save keyword, we blind it to protect against AutoCloud!
        LogDebug("Blinded Universal AutoCloud scan: " + std::string(path.begin(), path.end()));
        return true;
    }
    
    // If it's inside Steam and didn't trigger the save keywords above, it's safe (e.g. game verification, updates)
    if (isSteamDir) {
        return false;
    }
    
    // Also universally block if Steam is explicitly scanning for common save extensions anywhere
    if (path.length() >= 4) {
        std::wstring ext = path.substr(path.length() - 4);
        if (ext == L".sav" || ext == L".bak" || ext == L".dat") {
            LogDebug("Blinded AutoCloud extension scan: " + std::string(path.begin(), path.end()));
            return true;
        }
    }
    
    return false;
}

HANDLE WINAPI HookedFindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    if (ShouldHideFromSteamCloud(lpFileName)) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return OriginalFindFirstFileW(lpFileName, lpFindFileData);
}

HANDLE WINAPI HookedFindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags) {
    if (ShouldHideFromSteamCloud(lpFileName)) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return OriginalFindFirstFileExW(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
}

void ApplyAllHooks() {
    static std::mutex s_hookMutex;
    static DWORD s_lastApplyTime = 0;
    DWORD now = GetTickCount();
    if (now - s_lastApplyTime < 500) return;
    
    std::unique_lock<std::mutex> lock(s_hookMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    
    s_lastApplyTime = now;
    static int s_applyCount = 0;
    s_applyCount++;
    if (s_applyCount <= 3 || s_applyCount % 10 == 0) {
        LogDebug("ApplyAllHooks: Round #" + std::to_string(s_applyCount));
    }

    HookAllModulesIAT("kernel32.dll", "GetProcAddress", (PROC)HookedGetProcAddress, (PROC*)&OriginalGetProcAddress);
    HookAllModulesIAT("kernelbase.dll", "GetProcAddress", (PROC)HookedGetProcAddress, (PROC*)&OriginalGetProcAddress);
    HookAllModulesIAT("api-ms-win-core-libraryloader-l1-1-0.dll", "GetProcAddress", (PROC)HookedGetProcAddress, (PROC*)&OriginalGetProcAddress);
    HookAllModulesIAT("api-ms-win-core-libraryloader-l1-2-0.dll", "GetProcAddress", (PROC)HookedGetProcAddress, (PROC*)&OriginalGetProcAddress);
    MH_CreateHookApiEx(L"kernel32", "FindFirstFileW", &HookedFindFirstFileW, (LPVOID*)&OriginalFindFirstFileW, nullptr);
    MH_CreateHookApiEx(L"kernel32", "FindFirstFileExW", &HookedFindFirstFileExW, (LPVOID*)&OriginalFindFirstFileExW, nullptr);
    
    HookAllModulesIAT("steamclient.dll", "CreateInterface", (PROC)HookedCreateInterface, (PROC*)&OriginalCreateInterface);
    HookAllModulesIAT("steamclient64.dll", "CreateInterface", (PROC)HookedCreateInterface, (PROC*)&OriginalCreateInterface);

    HookAllModulesIAT("kernel32.dll", "CreateProcessW", (PROC)HookedCreateProcessW, (PROC*)&OriginalCreateProcessW);
    HookAllModulesIAT("kernel32.dll", "CreateFileW", (PROC)HookedCreateFileW, (PROC*)&OriginalCreateFileW);
    HookAllModulesIAT("kernel32.dll", "CreateFileA", (PROC)HookedCreateFileA, (PROC*)&OriginalCreateFileA);
    HookAllModulesIAT("kernel32.dll", "CloseHandle", (PROC)HookedCloseHandle, (PROC*)&OriginalCloseHandle);
    HookAllModulesIAT("kernel32.dll", "WriteFile", (PROC)HookedWriteFile, (PROC*)&OriginalWriteFile);
    HookAllModulesIAT("kernel32.dll", "LoadLibraryW", (PROC)HookedLoadLibraryW, (PROC*)&OriginalLoadLibraryW);
    HookAllModulesIAT("kernel32.dll", "LoadLibraryExW", (PROC)HookedLoadLibraryExW, (PROC*)&OriginalLoadLibraryExW);
    HookAllModulesIAT("kernel32.dll", "LoadLibraryA", (PROC)HookedLoadLibraryA, (PROC*)&OriginalLoadLibraryA);
    HookAllModulesIAT("kernel32.dll", "LoadLibraryExA", (PROC)HookedLoadLibraryExA, (PROC*)&OriginalLoadLibraryExA);
}

void MainLoop() {
    try {
        LogDebug("==================================================");
        LogDebug("MainLoop: version.dll attached to process.");
        LogDebug("Build: " __DATE__ " " __TIME__);
        
        if (MH_Initialize() != MH_OK) {
            LogDebug("MainLoop: MH_Initialize failed!");
        }

        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        fs::path steamDir = fs::path(exePath).parent_path();
        g_steamPath = steamDir.string();
        LogDebug("Steam path: " + g_steamPath);

        LoadConfig(g_steamPath);
        LogDebug("Config loaded. autoPatch=" + std::string(g_autoPatch ? "true" : "false") + 
                 ", provider=" + g_autoProvider + 
                 ", patchedGames=" + std::to_string(g_patchedAppIds.size()));

        if (!g_patcherPath.empty()) {
            LaunchPatcher(g_patcherPath);
        }

        ApplyAllHooks();
        
        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
            LogDebug("MainLoop: MH_EnableHook failed!");
        }

        auto userIdOpt = ReadRegistryDword(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam\\ActiveProcess", "ActiveUser");
        if (userIdOpt.has_value() && *userIdOpt != 0) {
            g_steamUserId = std::to_string(*userIdOpt);
        } else {
            fs::path userdataPath = steamDir / "userdata";
            if (fs::exists(userdataPath) && fs::is_directory(userdataPath)) {
                for (const auto& entry : fs::directory_iterator(userdataPath)) {
                    if (entry.is_directory()) {
                        std::string dn = entry.path().filename().string();
                        if (!dn.empty() && dn != "0" && std::all_of(dn.begin(), dn.end(), ::isdigit)) {
                            g_steamUserId = dn;
                            break;
                        }
                    }
                }
            }
        }
        LogDebug("Steam user ID: " + g_steamUserId);
        
        // Auto-delete remotecache.vdf for patched games to avoid manual user action
        if (!g_steamPath.empty() && !g_steamUserId.empty() && !g_patchedAppIds.empty()) {
            for (uint32_t appId : g_patchedAppIds) {
                fs::path cacheFile = fs::path(g_steamPath) / "userdata" / g_steamUserId / std::to_string(appId) / "remotecache.vdf";
                if (fs::exists(cacheFile)) {
                    std::error_code ec;
                    fs::remove(cacheFile, ec);
                    if (!ec) {
                        LogDebug("Auto-cleared remotecache.vdf for AppID " + std::to_string(appId));
                    }
                }
            }
        }
        std::thread cloudDisablerThread([]() {
            try {
                while (g_dllRunning) {
                    DisableSteamCloudUIAndErrors();
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                }
            } catch (...) {}
        });
        cloudDisablerThread.detach();
        std::thread t([]() {
            try {
                MonitorThread();
            } catch (...) {}
        });
        t.detach();
        std::thread fallbackThread([]() {
            try {
                std::this_thread::sleep_for(std::chrono::seconds(8));
                if (!g_remoteStorageHooked.load()) {
                    LogDebug("Fallback: IClientRemoteStorage not yet hooked, trying direct CreateInterface...");
                    TryDirectRemoteStorageHook();
                }
                for (int retry = 0; retry < 10 && g_dllRunning; retry++) {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    if (g_remoteStorageHooked.load()) {
                        LogDebug("Fallback: IClientRemoteStorage is hooked. Done.");
                        return;
                    }
                    LogDebug("Fallback: Retry #" + std::to_string(retry + 1) + " to hook IClientRemoteStorage...");
                    TryDirectRemoteStorageHook();
                }
            } catch (...) {
                LogDebug("Fallback thread exception!");
            }
        });
        fallbackThread.detach();
        
    } catch (const std::exception& e) {
        LogDebug(std::string("MainLoop exception: ") + e.what());
    } catch (...) {
        LogDebug("MainLoop unknown exception!");
    }
}

DWORD WINAPI MainLoopThread(LPVOID lpParam) {
    MainLoop();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainLoopThread, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        g_dllRunning = false;
        TerminatePatcher();
        break;
    }
    return TRUE;
}
