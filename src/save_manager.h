#pragma once

#ifndef SAVE_MANAGER_H
#define SAVE_MANAGER_H

#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <optional>

#include <Windows.h>

#include "steam_utils.h"

namespace fs = std::filesystem;

namespace saves {

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

struct BackupInfo {
    std::string gameName;
    uint32_t    appId;
    std::string path;
    std::string timestamp;
    uint64_t    sizeBytes;
};

struct PatchResult {
    bool        success;
    std::string message;
    std::string backupPath;
    std::string symlinkPath;
    std::string targetPath;
};

inline std::string GetBackupRootDir() {
    char* localAppData = nullptr;
    size_t len = 0;
    if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") == 0 && localAppData != nullptr) {
        fs::path backupRoot = fs::path(localAppData) / "SteamCloudPatcher" / "backups";
        free(localAppData);
        return backupRoot.string();
    }
    return (fs::current_path() / "backups").string();
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

inline uint64_t GetDirectorySize(const fs::path& dirPath) {
    uint64_t totalSize = 0;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(
                 dirPath, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                totalSize += entry.file_size();
            }
        }
    } catch (...) {}
    return totalSize;
}

inline PatchResult CreateBackup(
    const std::string& sourcePath,
    const std::string& gameName,
    uint32_t appId,
    const std::string& customBackupRoot = "")
{
    PatchResult result;
    result.success = false;

    if (IsProtectedSystemDirectory(sourcePath)) {
        result.message = "Source path is a protected system directory: " + sourcePath;
        return result;
    }

    if (!fs::exists(sourcePath)) {
        result.message = "Source path does not exist: " + sourcePath;
        return result;
    }

    std::string sanitizedName = gameName;
    for (char& c : sanitizedName) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
            c = '_';
        }
    }

    std::string backupDirName = std::to_string(appId) + "_" +
                                sanitizedName + "_" + GetTimestamp();

    fs::path backupRoot = customBackupRoot.empty() ? fs::path(GetBackupRootDir()) : fs::path(customBackupRoot);
    fs::path backupPath = backupRoot / backupDirName;

    try {
        fs::create_directories(backupPath);
        fs::copy(sourcePath, backupPath,
                 fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing);

        fs::path metaPath = backupPath / "_backup_meta.txt";
        std::ofstream metaFile(metaPath);
        if (metaFile.is_open()) {
            metaFile << "game=" << gameName << "\n";
            metaFile << "appId=" << appId << "\n";
            metaFile << "timestamp=" << GetTimestamp() << "\n";
            metaFile << "sourcePath=" << sourcePath << "\n";
            metaFile.close();
        }

        result.success = true;
        result.backupPath = backupPath.string();
        result.message = "Backup created successfully at: " + backupPath.string();
    } catch (const std::exception& e) {
        result.message = std::string("Failed to create backup: ") + e.what();
    }

    return result;
}

inline void SyncDirectories(const std::string& localPathStr, const std::string& cloudPathStr) {
    if (IsProtectedSystemDirectory(localPathStr) || IsProtectedSystemDirectory(cloudPathStr)) {
        return;
    }
    try {
        fs::path localPath(localPathStr);
        fs::path cloudPath(cloudPathStr);

        if (!fs::exists(localPath)) {
            fs::create_directories(localPath);
        }
        if (!fs::exists(cloudPath)) {
            fs::create_directories(cloudPath);
        }

        for (const auto& entry : fs::recursive_directory_iterator(localPath, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                fs::path relPath = fs::relative(entry.path(), localPath);
                fs::path target = cloudPath / relPath;

                bool copyNeeded = false;
                if (!fs::exists(target)) {
                    copyNeeded = true;
                } else {
                    auto localTime = fs::last_write_time(entry.path());
                    auto cloudTime = fs::last_write_time(target);
                    if (localTime > cloudTime) {
                        copyNeeded = true;
                    }
                }

                if (copyNeeded) {
                    fs::create_directories(target.parent_path());
                    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
                    try { fs::last_write_time(target, fs::last_write_time(entry.path())); } catch (...) {}
                }
            }
        }

        for (const auto& entry : fs::recursive_directory_iterator(cloudPath, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                fs::path relPath = fs::relative(entry.path(), cloudPath);
                fs::path target = localPath / relPath;

                bool copyNeeded = false;
                if (!fs::exists(target)) {
                    copyNeeded = true;
                } else {
                    auto cloudTime = fs::last_write_time(entry.path());
                    auto localTime = fs::last_write_time(target);
                    if (cloudTime > localTime) {
                        copyNeeded = true;
                    }
                }

                if (copyNeeded) {
                    fs::create_directories(target.parent_path());
                    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
                    try { fs::last_write_time(target, fs::last_write_time(entry.path())); } catch (...) {}
                }
            }
        }
    } catch (...) {}
}

inline std::vector<BackupInfo> GetBackupsList() {
    std::vector<BackupInfo> backups;
    fs::path backupRoot = GetBackupRootDir();

    if (!fs::exists(backupRoot)) {
        return backups;
    }

    for (const auto& entry : fs::directory_iterator(backupRoot)) {
        if (!entry.is_directory()) continue;

        BackupInfo info;
        info.path = entry.path().string();

        fs::path metaPath = entry.path() / "_backup_meta.txt";
        if (fs::exists(metaPath)) {
            std::ifstream metaFile(metaPath);
            std::string line;
            while (std::getline(metaFile, line)) {
                size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string key = line.substr(0, eq);
                    std::string value = line.substr(eq + 1);
                    if (key == "game") info.gameName = value;
                    else if (key == "appId") {
                        try { info.appId = static_cast<uint32_t>(std::stoul(value)); }
                        catch (...) {}
                    }
                    else if (key == "timestamp") info.timestamp = value;
                }
            }
        } else {
            std::string dirName = entry.path().filename().string();
            size_t firstUnderscore = dirName.find('_');
            if (firstUnderscore != std::string::npos) {
                try {
                    info.appId = static_cast<uint32_t>(
                        std::stoul(dirName.substr(0, firstUnderscore)));
                } catch (...) {}
                info.gameName = dirName.substr(firstUnderscore + 1);
            }
        }

        info.sizeBytes = GetDirectorySize(entry.path());

        if (info.timestamp.empty()) {
            auto ftime = entry.last_write_time();
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            auto time = std::chrono::system_clock::to_time_t(sctp);
            std::tm tm_buf{};
            localtime_s(&tm_buf, &time);
            std::ostringstream oss;
            oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
            info.timestamp = oss.str();
        }

        backups.push_back(std::move(info));
    }

    std::sort(backups.begin(), backups.end(),
              [](const BackupInfo& a, const BackupInfo& b) {
                  return a.timestamp > b.timestamp;
              });

    return backups;
}

}

#endif
