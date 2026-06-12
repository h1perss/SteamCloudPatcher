#pragma once

#ifndef CLOUD_PROVIDER_H
#define CLOUD_PROVIDER_H

#include <string>
#include <vector>
#include <filesystem>
#include <optional>

#include <Windows.h>

#include "steam_utils.h"

namespace fs = std::filesystem;

namespace cloud {

struct CloudProvider {
    std::string id;
    std::string name;
    std::string path;
    bool        detected;
    std::string icon;
};

inline CloudProvider DetectGoogleDrive() {
    CloudProvider provider;
    provider.id = "gdrive";
    provider.name = "Google Drive";
    provider.detected = false;
    provider.icon = "☁️";

    auto driveFs = steam::ReadRegistryString(
        HKEY_CURRENT_USER,
        "SOFTWARE\\Google\\DriveFS",
        "MountPoint");
    if (driveFs.has_value() && !driveFs->empty() && fs::exists(*driveFs)) {
        provider.path = *driveFs;
        provider.detected = true;
        provider.name = "Google Drive (Workspace)";
        return provider;
    }

    auto driveRoot = steam::ReadRegistryString(
        HKEY_CURRENT_USER,
        "SOFTWARE\\Google\\DriveFS\\Share",
        "BasePath");
    if (driveRoot.has_value() && !driveRoot->empty() && fs::exists(*driveRoot)) {
        provider.path = *driveRoot;
        provider.detected = true;
        return provider;
    }

    std::string userProfile = steam::GetUserProfilePath();
    std::vector<std::string> commonPaths = {
        userProfile + "\\Google Drive",
        userProfile + "\\My Drive",
        userProfile + "\\GoogleDrive",
        "G:\\My Drive",
        "G:\\Shared drives",
    };

    for (const auto& p : commonPaths) {
        if (fs::exists(p)) {
            provider.path = p;
            provider.detected = true;
            return provider;
        }
    }

    auto driveInstall = steam::ReadRegistryString(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Google\\Drive",
        "InstallLocation");
    if (driveInstall.has_value() && !driveInstall->empty()) {
        provider.path = userProfile + "\\Google Drive";
        provider.detected = false;
        provider.name = "Google Drive (install detected, path unverified)";
        return provider;
    }

    provider.path = userProfile + "\\Google Drive";
    return provider;
}

inline CloudProvider DetectOneDrive() {
    CloudProvider provider;
    provider.id = "onedrive";
    provider.name = "OneDrive";
    provider.detected = false;
    provider.icon = "📁";

    char* oneDrivePath = nullptr;
    size_t len = 0;
    if (_dupenv_s(&oneDrivePath, &len, "OneDrive") == 0 && oneDrivePath != nullptr) {
        std::string envPath(oneDrivePath);
        free(oneDrivePath);
        if (!envPath.empty() && fs::exists(envPath)) {
            provider.path = envPath;
            provider.detected = true;
            return provider;
        }
    }

    if (_dupenv_s(&oneDrivePath, &len, "OneDriveConsumer") == 0 && oneDrivePath != nullptr) {
        std::string envPath(oneDrivePath);
        free(oneDrivePath);
        if (!envPath.empty() && fs::exists(envPath)) {
            provider.path = envPath;
            provider.detected = true;
            provider.name = "OneDrive (Personal)";
            return provider;
        }
    }

    if (_dupenv_s(&oneDrivePath, &len, "OneDriveCommercial") == 0 && oneDrivePath != nullptr) {
        std::string envPath(oneDrivePath);
        free(oneDrivePath);
        if (!envPath.empty() && fs::exists(envPath)) {
            provider.path = envPath;
            provider.detected = true;
            provider.name = "OneDrive (Business)";
            return provider;
        }
    }

    auto regPath = steam::ReadRegistryString(
        HKEY_CURRENT_USER,
        "SOFTWARE\\Microsoft\\OneDrive",
        "UserFolder");
    if (regPath.has_value() && !regPath->empty() && fs::exists(*regPath)) {
        provider.path = *regPath;
        provider.detected = true;
        return provider;
    }

    std::string userProfile = steam::GetUserProfilePath();
    std::vector<std::string> commonPaths = {
        userProfile + "\\OneDrive",
        userProfile + "\\OneDrive - Personal",
    };

    for (const auto& p : commonPaths) {
        if (fs::exists(p)) {
            provider.path = p;
            provider.detected = true;
            return provider;
        }
    }

    provider.path = userProfile + "\\OneDrive";
    return provider;
}

inline CloudProvider DetectDropbox() {
    CloudProvider provider;
    provider.id = "dropbox";
    provider.name = "Dropbox";
    provider.detected = false;
    provider.icon = "📦";

    std::string userProfile = steam::GetUserProfilePath();

    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "LOCALAPPDATA") == 0 && appdata != nullptr) {
        fs::path infoJson = fs::path(appdata) / "Dropbox" / "info.json";
        free(appdata);

        if (fs::exists(infoJson)) {
            auto content = steam::ReadFileContents(infoJson);
            if (content.has_value()) {
                size_t pathPos = content->find("\"path\"");
                if (pathPos != std::string::npos) {
                    size_t colonPos = content->find(':', pathPos);
                    size_t quoteStart = content->find('"', colonPos + 1);
                    size_t quoteEnd = content->find('"', quoteStart + 1);
                    if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
                        std::string dropboxPath = content->substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                        std::string unescaped;
                        for (size_t i = 0; i < dropboxPath.size(); ++i) {
                            if (dropboxPath[i] == '\\' && i + 1 < dropboxPath.size()) {
                                ++i;
                                unescaped += dropboxPath[i];
                            } else {
                                unescaped += dropboxPath[i];
                            }
                        }
                        if (fs::exists(unescaped)) {
                            provider.path = unescaped;
                            provider.detected = true;
                            return provider;
                        }
                    }
                }
            }
        }
    }

    std::vector<std::string> commonPaths = {
        userProfile + "\\Dropbox",
        "C:\\Dropbox",
    };

    for (const auto& p : commonPaths) {
        if (fs::exists(p)) {
            provider.path = p;
            provider.detected = true;
            return provider;
        }
    }

    provider.path = userProfile + "\\Dropbox";
    return provider;
}

inline CloudProvider GetCustomProvider() {
    return {
        "local",
        "Custom Path",
        "",
        true,
        "📂"
    };
}

inline std::vector<CloudProvider> GetAvailableProviders() {
    std::vector<CloudProvider> providers;
    providers.push_back(DetectGoogleDrive());
    providers.push_back(DetectOneDrive());
    providers.push_back(DetectDropbox());
    providers.push_back(GetCustomProvider());
    return providers;
}

inline std::string GetProviderSavePath(
    const std::string& providerId,
    const std::string& customPath,
    const std::string& gameName,
    uint32_t appId)
{
    std::string basePath;

    if (providerId == "local" || providerId == "custom") {
        basePath = customPath;
    } else {
        auto providers = GetAvailableProviders();
        for (const auto& p : providers) {
            if (p.id == providerId) {
                basePath = p.path;
                break;
            }
        }
    }

    if (basePath.empty()) {
        return "";
    }

    std::string sanitizedName = gameName;
    for (char& c : sanitizedName) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ') {
            c = '_';
        }
    }

    fs::path savePath = fs::path(basePath) / "SteamCloudPatcher" /
                        (std::to_string(appId) + "_" + sanitizedName);

    return savePath.string();
}

}

#endif
