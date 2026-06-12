#pragma once

#ifndef GAME_DATABASE_H
#define GAME_DATABASE_H

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <set>
#include <map>
#include <regex>
#include <algorithm>
#include <optional>

#include "steam_utils.h"

namespace fs = std::filesystem;

namespace games {

extern std::set<uint32_t> g_patchedAppIds;

struct GameInfo {
    uint32_t    appId;
    std::string name;
    std::string developer;
    std::string saveRelativePath;
    bool        usesUserdata;
    std::string description;
    bool        installed = false;
    std::string resolvedSavePath;
    bool        patched = false;
};

struct FallbackGame {
    uint32_t appId;
    std::string name;
    std::string developer;
    std::string relativePath;
    bool usesUserdata;
    std::string description;
};

inline std::vector<FallbackGame> GetFallbackGames() {
    return {
        { 582010, "Monster Hunter: World", "Capcom", "My Games\\Monster Hunter World", false, "Saves in Documents\\My Games\\Monster Hunter World" },
        { 1446780, "Monster Hunter Rise", "Capcom", "", true, "Saves in Steam userdata" },
        { 601150, "Devil May Cry 5", "Capcom", "", true, "Saves in Steam userdata" },
        { 1196590, "Resident Evil Village", "Capcom", "", true, "Saves in Steam userdata" },
        { 2050650, "Resident Evil 4 Remake", "Capcom", "", true, "Saves in Steam userdata" },
        { 2054970, "Dragon's Dogma 2", "Capcom", "", true, "Saves in Steam userdata" },
        { 1364780, "Street Fighter 6", "Capcom", "", true, "Saves in Steam userdata" },
        { 1245620, "Elden Ring", "FromSoftware", "EldenRing", false, "Saves in AppData\\Roaming\\EldenRing" },
        { 374320, "Dark Souls III", "FromSoftware", "DarkSoulsIII", false, "Saves in AppData\\Roaming\\DarkSoulsIII" },
        { 1091500, "Cyberpunk 2077", "CD Projekt RED", "Saved Games\\CD Projekt Red\\Cyberpunk 2077", false, "Saves in User\\Saved Games" },
        { 292030, "The Witcher 3: Wild Hunt", "CD Projekt RED", "My Games\\The Witcher 3", false, "Saves in Documents\\My Games\\The Witcher 3" }
    };
}

inline std::optional<std::pair<uint32_t, std::string>> ParseManifestFile(const fs::path& path) {
    auto contentOpt = steam::ReadFileContents(path);
    if (!contentOpt) return std::nullopt;
    std::string content = *contentOpt;

    uint32_t appId = 0;
    std::regex idRegex(R"raw("appid"\s+"(\d+)")raw", std::regex_constants::icase);
    std::smatch idMatch;
    if (std::regex_search(content, idMatch, idRegex)) {
        try { appId = std::stoul(idMatch[1].str()); }
        catch (...) { return std::nullopt; }
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
    auto contentOpt = steam::ReadFileContents(remotecachePath);
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
                            std::string userProfile = steam::GetUserProfilePath();

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

inline std::vector<GameInfo> DetectInstalledGames(
    const std::string& steamPath,
    const std::string& userId)
{
    std::vector<GameInfo> detectedGames;
    std::set<uint32_t> processedAppIds;
    std::map<uint32_t, std::string> appNames;
    std::set<uint32_t> installedAppIds;

    auto libraryFolders = steam::FindSteamLibraryFolders(steamPath);
    for (const auto& folder : libraryFolders) {
        fs::path steamapps = fs::path(folder) / "steamapps";
        if (fs::exists(steamapps) && fs::is_directory(steamapps)) {
            for (const auto& entry : fs::directory_iterator(steamapps)) {
                if (entry.is_regular_file() && entry.path().extension() == ".acf") {
                    std::string filename = entry.path().filename().string();
                    if (filename.rfind("appmanifest_", 0) == 0) {
                        auto parsed = ParseManifestFile(entry.path());
                        if (parsed) {
                            appNames[parsed->first] = parsed->second;
                            installedAppIds.insert(parsed->first);
                        }
                    }
                }
            }
        }
    }

    fs::path userdataPath = fs::path(steamPath) / "userdata" / userId;
    if (fs::exists(userdataPath) && fs::is_directory(userdataPath)) {
        for (const auto& entry : fs::directory_iterator(userdataPath)) {
            if (entry.is_directory()) {
                std::string dirName = entry.path().filename().string();
                if (!dirName.empty() && std::all_of(dirName.begin(), dirName.end(), ::isdigit)) {
                    try {
                        uint32_t appId = std::stoul(dirName);
                        if (appId > 10) {
                            processedAppIds.insert(appId);
                        }
                    } catch (...) {}
                }
            }
        }
    }

    for (uint32_t appId : installedAppIds) {
        processedAppIds.insert(appId);
    }

    auto fallbacks = GetFallbackGames();
    std::map<uint32_t, FallbackGame> fallbackMap;
    for (const auto& g : fallbacks) {
        fallbackMap[g.appId] = g;
    }

    for (uint32_t appId : processedAppIds) {
        GameInfo game;
        game.appId = appId;
        game.installed = (installedAppIds.find(appId) != installedAppIds.end());

        if (appNames.find(appId) != appNames.end()) {
            game.name = appNames[appId];
        } else if (fallbackMap.find(appId) != fallbackMap.end()) {
            game.name = fallbackMap[appId].name;
            game.developer = fallbackMap[appId].developer;
        } else {
            game.name = "Steam App " + std::to_string(appId);
            game.developer = "Valve / Steam";
        }

        if (game.developer.empty()) {
            if (fallbackMap.find(appId) != fallbackMap.end()) {
                game.developer = fallbackMap[appId].developer;
            } else {
                game.developer = "Steam Application";
            }
        }

        fs::path rcPath = fs::path(steamPath) / "userdata" / userId / std::to_string(appId) / "remotecache.vdf";
        std::optional<std::string> resolvedPath;
        if (fs::exists(rcPath)) {
            resolvedPath = ResolvePathFromRemoteCache(rcPath, steamPath, userId);
        }

        if (resolvedPath && !resolvedPath->empty()) {
            game.resolvedSavePath = *resolvedPath;
            game.usesUserdata = false;
            game.description = "Saves in custom folder (auto-detected)";
        } else {
            fs::path userSavePath = fs::path(steamPath) / "userdata" / userId / std::to_string(appId) / "remote";
            if (fs::exists(userSavePath)) {
                game.resolvedSavePath = userSavePath.string();
                game.usesUserdata = true;
                game.description = "Saves in Steam userdata";
            } else if (fallbackMap.find(appId) != fallbackMap.end() && !fallbackMap[appId].usesUserdata) {
                std::string userProfile = steam::GetUserProfilePath();
                if (appId == 1245620 || appId == 374320) {
                    char* appdata = nullptr;
                    size_t len = 0;
                    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
                        game.resolvedSavePath = (fs::path(appdata) / fallbackMap[appId].relativePath).string();
                        free(appdata);
                    }
                } else if (appId == 1091500) {
                    game.resolvedSavePath = (fs::path(userProfile) / fallbackMap[appId].relativePath).string();
                } else {
                    game.resolvedSavePath = (fs::path(userProfile) / "Documents" / fallbackMap[appId].relativePath).string();
                }
                game.usesUserdata = false;
                game.description = fallbackMap[appId].description;
            } else {
                game.resolvedSavePath = userSavePath.string();
                game.usesUserdata = true;
                game.description = "Saves in Steam userdata (default)";
            }
        }

        game.patched = (g_patchedAppIds.find(appId) != g_patchedAppIds.end());

        detectedGames.push_back(std::move(game));
    }

    std::sort(detectedGames.begin(), detectedGames.end(),
              [](const GameInfo& a, const GameInfo& b) {
                  if (a.installed != b.installed) {
                      return a.installed > b.installed;
                  }
                  return a.name < b.name;
              });

    return detectedGames;
}

inline std::vector<GameInfo> GetAllSupportedGames() {
    std::vector<GameInfo> list;
    auto fallbacks = GetFallbackGames();
    for (const auto& g : fallbacks) {
        GameInfo gi;
        gi.appId = g.appId;
        gi.name = g.name;
        gi.developer = g.developer;
        gi.saveRelativePath = g.relativePath;
        gi.usesUserdata = g.usesUserdata;
        gi.description = g.description;
        list.push_back(std::move(gi));
    }
    return list;
}

}

#endif
