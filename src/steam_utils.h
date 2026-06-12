#pragma once

#ifndef STEAM_UTILS_H
#define STEAM_UTILS_H

#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <regex>
#include <iostream>
#include <thread>
#include <chrono>

#include <Windows.h>

namespace fs = std::filesystem;

namespace steam {

inline std::optional<std::string> ReadRegistryString(
    HKEY hKeyRoot,
    const std::string& subKey,
    const std::string& valueName)
{
    HKEY hKey = nullptr;
    const REGSAM accessFlags[] = {
        KEY_READ | KEY_WOW64_64KEY,
        KEY_READ | KEY_WOW64_32KEY,
        KEY_READ
    };

    for (auto access : accessFlags) {
        LONG result = RegOpenKeyExA(hKeyRoot, subKey.c_str(), 0, access, &hKey);
        if (result == ERROR_SUCCESS) {
            DWORD dataType = 0;
            DWORD dataSize = 0;
            result = RegQueryValueExA(hKey, valueName.c_str(), nullptr,
                                      &dataType, nullptr, &dataSize);

            if (result == ERROR_SUCCESS && (dataType == REG_SZ || dataType == REG_EXPAND_SZ) && dataSize > 0) {
                std::string value(dataSize, '\0');
                result = RegQueryValueExA(hKey, valueName.c_str(), nullptr,
                                          &dataType, reinterpret_cast<LPBYTE>(value.data()), &dataSize);
                RegCloseKey(hKey);

                if (result == ERROR_SUCCESS) {
                    while (!value.empty() && value.back() == '\0') {
                        value.pop_back();
                    }
                    return value;
                }
            } else {
                RegCloseKey(hKey);
            }
        }
    }
    return std::nullopt;
}

inline std::optional<DWORD> ReadRegistryDword(
    HKEY hKeyRoot,
    const std::string& subKey,
    const std::string& valueName)
{
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExA(hKeyRoot, subKey.c_str(), 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        result = RegOpenKeyExA(hKeyRoot, subKey.c_str(), 0,
                               KEY_READ | KEY_WOW64_32KEY, &hKey);
        if (result != ERROR_SUCCESS) return std::nullopt;
    }

    DWORD value = 0;
    DWORD dataSize = sizeof(DWORD);
    DWORD dataType = 0;
    result = RegQueryValueExA(hKey, valueName.c_str(), nullptr,
                              &dataType, reinterpret_cast<LPBYTE>(&value), &dataSize);
    RegCloseKey(hKey);

    if (result == ERROR_SUCCESS && dataType == REG_DWORD) {
        return value;
    }
    return std::nullopt;
}

inline std::optional<std::string> DetectSteamPath() {
    struct RegistryLocation {
        HKEY root;
        std::string subKey;
        std::string valueName;
    };

    const std::vector<RegistryLocation> locations = {
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam",           "InstallPath" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath" },
        { HKEY_CURRENT_USER,  "SOFTWARE\\Valve\\Steam",           "SteamPath" },
    };

    for (const auto& loc : locations) {
        auto path = ReadRegistryString(loc.root, loc.subKey, loc.valueName);
        if (path.has_value() && !path->empty()) {
            std::string normalized = *path;
            std::replace(normalized.begin(), normalized.end(), '/', '\\');
            if (fs::exists(normalized)) {
                return normalized;
            }
        }
    }

    const std::vector<std::string> commonPaths = {
        "C:\\Program Files (x86)\\Steam",
        "C:\\Program Files\\Steam",
        "D:\\Steam",
        "D:\\Program Files (x86)\\Steam",
        "E:\\Steam",
    };

    for (const auto& p : commonPaths) {
        if (fs::exists(p) && fs::exists(fs::path(p) / "steam.exe")) {
            return p;
        }
    }
    return std::nullopt;
}

inline std::vector<std::pair<std::string, std::string>> ParseVdfKeyValues(
    const std::string& content)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    std::regex kvRegex(R"raw("([^"]+)"\s+"([^"]*)")raw");
    std::sregex_iterator it(content.begin(), content.end(), kvRegex);
    std::sregex_iterator end;

    for (; it != end; ++it) {
        pairs.emplace_back((*it)[1].str(), (*it)[2].str());
    }
    return pairs;
}

inline std::optional<std::string> ReadFileContents(const fs::path& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

inline std::vector<std::string> FindSteamLibraryFolders(const std::string& steamPath) {
    std::vector<std::string> folders;
    folders.push_back(steamPath);

    fs::path vdfPath = fs::path(steamPath) / "steamapps" / "libraryfolders.vdf";
    auto content = ReadFileContents(vdfPath);

    if (!content.has_value()) {
        vdfPath = fs::path(steamPath) / "config" / "libraryfolders.vdf";
        content = ReadFileContents(vdfPath);
    }

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

inline std::optional<std::string> GetSteamUserId(const std::string& steamPath) {
    auto regUser = ReadRegistryDword(HKEY_CURRENT_USER,
                                     "SOFTWARE\\Valve\\Steam\\ActiveProcess",
                                     "ActiveUser");
    if (regUser.has_value() && *regUser != 0) {
        return std::to_string(*regUser);
    }

    fs::path loginUsersPath = fs::path(steamPath) / "config" / "loginusers.vdf";
    auto content = ReadFileContents(loginUsersPath);

    if (content.has_value()) {
        const uint64_t steamIdBase = 76561197960265728ULL;
        std::regex idRegex(R"raw("(\d{17})")raw");
        std::sregex_iterator it(content->begin(), content->end(), idRegex);
        std::sregex_iterator end;

        std::string mostRecentId64;
        std::string currentId64;

        std::istringstream stream(*content);
        std::string line;
        bool inUserBlock = false;

        while (std::getline(stream, line)) {
            std::smatch match;
            if (std::regex_search(line, match, idRegex)) {
                currentId64 = match[1].str();
                inUserBlock = true;
            }

            if (inUserBlock && line.find("\"MostRecent\"") != std::string::npos &&
                line.find("\"1\"") != std::string::npos) {
                mostRecentId64 = currentId64;
            }

            if (inUserBlock && line.find('}') != std::string::npos) {
                inUserBlock = false;
            }
        }

        std::string targetId64 = mostRecentId64.empty() ? currentId64 : mostRecentId64;

        if (!targetId64.empty()) {
            try {
                uint64_t id64 = std::stoull(targetId64);
                uint64_t id32 = id64 - steamIdBase;
                return std::to_string(id32);
            } catch (...) {}
        }
    }

    fs::path userdataPath = fs::path(steamPath) / "userdata";
    if (fs::exists(userdataPath) && fs::is_directory(userdataPath)) {
        for (const auto& entry : fs::directory_iterator(userdataPath)) {
            if (entry.is_directory()) {
                std::string dirName = entry.path().filename().string();
                if (!dirName.empty() && dirName != "0" &&
                    std::all_of(dirName.begin(), dirName.end(), ::isdigit)) {
                    return dirName;
                }
            }
        }
    }
    return std::nullopt;
}

inline std::string ExpandEnvironmentPath(const std::string& path) {
    if (path.find('%') == std::string::npos) {
        return path;
    }

    char expanded[MAX_PATH * 2];
    DWORD result = ExpandEnvironmentStringsA(path.c_str(), expanded, sizeof(expanded));
    if (result > 0 && result < sizeof(expanded)) {
        return std::string(expanded);
    }
    return path;
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

inline bool IsGameInstalled(const std::string& steamPath, uint32_t appId) {
    auto folders = FindSteamLibraryFolders(steamPath);

    for (const auto& folder : folders) {
        fs::path manifestPath = fs::path(folder) / "steamapps" /
                                ("appmanifest_" + std::to_string(appId) + ".acf");
        if (fs::exists(manifestPath)) {
            return true;
        }
    }
    return false;
}

inline void TouchDirectoryFiles(const std::string& dirPath) {
    try {
        if (!fs::exists(dirPath)) return;
        auto now = fs::file_time_type::clock::now();
        if (fs::is_directory(dirPath)) {
            for (const auto& entry : fs::recursive_directory_iterator(dirPath, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file()) {
                    try {
                        fs::last_write_time(entry.path(), now);
                    } catch (...) {}
                }
            }
        } else if (fs::is_regular_file(dirPath)) {
            fs::last_write_time(dirPath, now);
        }
    } catch (...) {}
}

inline bool RestartSteam(const std::string& steamPath) {
    fs::path steamExe = fs::path(steamPath) / "steam.exe";
    if (!fs::exists(steamExe)) return false;

    ShellExecuteA(nullptr, "open", steamExe.string().c_str(), "-shutdown", nullptr, SW_HIDE);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    HINSTANCE res = ShellExecuteA(nullptr, "open", steamExe.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(res) > 32;
}

}

#endif
