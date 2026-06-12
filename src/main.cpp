#include <Windows.h>
#include <shellapi.h>
#include <iostream>
#include <string>
#include <csignal>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <set>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "steam_utils.h"
#include "game_database.h"
#include "save_manager.h"
#include "cloud_provider.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

std::set<uint32_t> games::g_patchedAppIds;

static constexpr int SERVER_PORT = 8847;
static std::atomic<bool> g_running{true};
static httplib::Server* g_serverPtr = nullptr;

static std::mutex g_stateMutex;
static std::string g_steamPath;
static std::string g_steamUserId;
static bool g_steamDetected = false;

static std::atomic<bool> g_autoPatch{false};
static std::string g_autoProvider;
static std::string g_autoCustomPath;
static std::atomic<bool> g_enableBackup{false};
static std::string g_backupPath;

namespace console {
enum class Color : WORD {
    Reset  = 7,
    Red    = 12,
    Green  = 10,
    Yellow = 14,
    Blue   = 9,
    Cyan   = 11,
    White  = 15,
    Gray   = 8,
    Magenta = 13,
};

inline void SetColor(Color color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, static_cast<WORD>(color));
}

inline void ResetColor() {
    SetColor(Color::Reset);
}

inline void PrintBanner() {
    SetColor(Color::Cyan);
    std::cout << R"(
  ╔═══════════════════════════════════════════════════════════════╗
  ║          Steam Cloud Save Patcher v1.0.0                     ║
  ║          ─────────────────────────────────                   ║
  ║          Redirect your game saves to the cloud               ║
  ╚═══════════════════════════════════════════════════════════════╝
)" << std::endl;
    ResetColor();
}

inline void PrintStatus(const std::string& label, const std::string& value, Color valueColor = Color::Green) {
    SetColor(Color::White);
    std::cout << "  [";
    SetColor(Color::Cyan);
    std::cout << label;
    SetColor(Color::White);
    std::cout << "] ";
    SetColor(valueColor);
    std::cout << value << std::endl;
    ResetColor();
}

inline void PrintInfo(const std::string& msg) {
    SetColor(Color::Gray);
    std::cout << "  > " << msg << std::endl;
    ResetColor();
}

inline void PrintSuccess(const std::string& msg) {
    SetColor(Color::Green);
    std::cout << "  ✓ " << msg << std::endl;
    ResetColor();
}

inline void PrintError(const std::string& msg) {
    SetColor(Color::Red);
    std::cout << "  ✗ " << msg << std::endl;
    ResetColor();
}

inline void PrintWarning(const std::string& msg) {
    SetColor(Color::Yellow);
    std::cout << "  ⚠ " << msg << std::endl;
    ResetColor();
}
}

inline std::string GetExecutableDir() {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        fs::path exePath(path);
        return exePath.parent_path().string();
    }
    return fs::current_path().string();
}

inline void LoadConfig(const std::string& exeDir) {
    fs::path configPath = fs::path(exeDir) / "config.json";
    if (fs::exists(configPath)) {
        try {
            auto content = steam::ReadFileContents(configPath);
            if (content) {
                auto j = json::parse(*content);
                g_autoPatch = j.value("autoPatch", false);
                g_autoProvider = j.value("provider", "");
                g_autoCustomPath = j.value("customPath", "");
                g_enableBackup = j.value("enableBackup", false);
                g_backupPath = j.value("backupPath", "");

                games::g_patchedAppIds.clear();
                if (j.contains("patchedGames") && j["patchedGames"].is_array()) {
                    for (const auto& item : j["patchedGames"]) {
                        if (item.is_number()) {
                            games::g_patchedAppIds.insert(item.get<uint32_t>());
                        }
                    }
                }
            }
        } catch (...) {}
    }
}

inline void SaveConfig(const std::string& dir) {
    fs::path configPath = fs::path(dir) / "config.json";
    try {
        json j = {
            {"autoPatch", g_autoPatch.load()},
            {"provider", g_autoProvider},
            {"customPath", g_autoCustomPath},
            {"enableBackup", g_enableBackup.load()},
            {"backupPath", g_backupPath},
            {"patcherPath", (fs::path(GetExecutableDir()) / "SteamCloudPatcher.exe").string()},
            {"patchedGames", json::array()}
        };
        for (uint32_t appId : games::g_patchedAppIds) {
            j["patchedGames"].push_back(appId);
        }
        std::ofstream file(configPath);
        if (file.is_open()) {
            file << j.dump(2);
        }
    } catch (...) {}
}

inline void SaveConfigAll() {
    std::string exeDir = GetExecutableDir();
    SaveConfig(exeDir);
    if (g_steamDetected && !g_steamPath.empty()) {
        SaveConfig(g_steamPath);
    }
}

void RefreshSteamState() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    auto steamPath = steam::DetectSteamPath();
    if (steamPath.has_value()) {
        g_steamPath = *steamPath;
        g_steamDetected = true;
        auto userId = steam::GetSteamUserId(g_steamPath);
        if (userId.has_value()) {
            g_steamUserId = *userId;
        }
    } else {
        g_steamDetected = false;
    }
}

void AddCorsHeaders(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

void JsonResponse(httplib::Response& res, const json& data, int status = 200) {
    AddCorsHeaders(res);
    res.status = status;
    res.set_content(data.dump(2), "application/json");
}

void JsonError(httplib::Response& res, const std::string& message, int status = 400) {
    json error = {
        {"error", true},
        {"message", message}
    };
    JsonResponse(res, error, status);
}

void SignalHandler(int signal) {
    (void)signal;
    g_running = false;
    if (g_serverPtr) {
        g_serverPtr->stop();
    }
}

void HandleStatus(const httplib::Request&, httplib::Response& res) {
    json response = {
        {"status", "running"},
        {"version", "1.0.0"},
        {"port", SERVER_PORT},
        {"uptime", "active"},
        {"steamDetected", g_steamDetected}
    };
    JsonResponse(res, response);
}

void HandleSteam(const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    json response = {
        {"detected", g_steamDetected},
        {"path", g_steamPath},
        {"userId", g_steamUserId}
    };

    if (g_steamDetected) {
        auto folders = steam::FindSteamLibraryFolders(g_steamPath);
        response["libraryFolders"] = json::array();
        for (const auto& f : folders) {
            response["libraryFolders"].push_back(f);
        }
    }
    JsonResponse(res, response);
}

void HandleGames(const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    json response = json::array();

    if (g_steamDetected) {
        auto gamesList = games::DetectInstalledGames(g_steamPath, g_steamUserId);
        for (const auto& game : gamesList) {
            json gameJson = {
                {"appId", game.appId},
                {"name", game.name},
                {"developer", game.developer},
                {"saveRelativePath", game.saveRelativePath},
                {"usesUserdata", game.usesUserdata},
                {"description", game.description},
                {"installed", game.installed},
                {"resolvedSavePath", game.resolvedSavePath},
                {"patched", game.patched}
            };
            if (game.patched) {
                std::string target = cloud::GetProviderSavePath(g_autoProvider, g_autoCustomPath, game.name, game.appId);
                gameJson["patchTarget"] = target;
            }
            response.push_back(gameJson);
        }
    }
    JsonResponse(res, response);
}

void HandlePatchGame(const httplib::Request& req, httplib::Response& res) {
    std::string appIdStr = req.matches[1];
    uint32_t appId = 0;
    try {
        appId = static_cast<uint32_t>(std::stoul(appIdStr));
    } catch (...) {
        JsonError(res, "Invalid app ID: " + appIdStr);
        return;
    }

    json body;
    try {
        body = json::parse(req.body);
    } catch (...) {
        JsonError(res, "Invalid JSON");
        return;
    }

    std::string provider = body.value("provider", "");
    std::string customPath = body.value("customPath", "");

    if (provider.empty()) {
        JsonError(res, "Missing provider");
        return;
    }

    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (!g_steamDetected || g_steamPath.empty() || g_steamUserId.empty()) {
        JsonError(res, "Steam not detected");
        return;
    }

    auto allGames = games::DetectInstalledGames(g_steamPath, g_steamUserId);
    const games::GameInfo* targetGame = nullptr;
    for (const auto& game : allGames) {
        if (game.appId == appId) {
            targetGame = &game;
            break;
        }
    }

    if (!targetGame) {
        JsonError(res, "Game not found", 404);
        return;
    }

    std::string savePath = targetGame->resolvedSavePath;
    std::string targetPath = cloud::GetProviderSavePath(provider, customPath, targetGame->name, appId);

    if (targetPath.empty()) {
        JsonError(res, "Could not resolve target path");
        return;
    }

    saves::PatchResult backupRes;
    if (g_enableBackup) {
        backupRes = saves::CreateBackup(savePath, targetGame->name, appId, g_backupPath);
    }
    saves::SyncDirectories(savePath, targetPath);
    std::string universalPath = cloud::GetProviderSavePath("local", "", targetGame->name, appId);
    if (!universalPath.empty() && universalPath != targetPath) {
        saves::SyncDirectories(savePath, universalPath);
    }

    games::g_patchedAppIds.insert(appId);
    SaveConfigAll();

    fs::path rcPath = fs::path(g_steamPath) / "userdata" / g_steamUserId / std::to_string(appId) / "remotecache.vdf";
    if (fs::exists(rcPath)) {
        try {
            fs::remove(rcPath);
        } catch (...) {}
    }
    steam::TouchDirectoryFiles(savePath);

    json response = {
        {"success", true},
        {"message", "Saves actively sync'd to cloud folder."},
        {"backupPath", backupRes.backupPath},
        {"symlinkPath", savePath},
        {"targetPath", targetPath}
    };
    JsonResponse(res, response);
}

void HandleRestoreGame(const httplib::Request& req, httplib::Response& res) {
    std::string appIdStr = req.matches[1];
    uint32_t appId = 0;
    try {
        appId = static_cast<uint32_t>(std::stoul(appIdStr));
    } catch (...) {
        JsonError(res, "Invalid app ID");
        return;
    }

    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (!g_steamDetected || g_steamPath.empty() || g_steamUserId.empty()) {
        JsonError(res, "Steam not detected");
        return;
    }

    auto allGames = games::DetectInstalledGames(g_steamPath, g_steamUserId);
    const games::GameInfo* targetGame = nullptr;
    for (const auto& game : allGames) {
        if (game.appId == appId) {
            targetGame = &game;
            break;
        }
    }

    if (!targetGame) {
        JsonError(res, "Game not found", 404);
        return;
    }

    games::g_patchedAppIds.erase(appId);
    SaveConfigAll();

    fs::path rcPath = fs::path(g_steamPath) / "userdata" / g_steamUserId / std::to_string(appId) / "remotecache.vdf";
    if (fs::exists(rcPath)) {
        try {
            fs::remove(rcPath);
        } catch (...) {}
    }
    steam::TouchDirectoryFiles(targetGame->resolvedSavePath);

    json response = {
        {"success", true},
        {"message", "Syncing disabled, original files kept intact."}
    };
    JsonResponse(res, response);
}

void HandleRestartSteam(const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!g_steamDetected || g_steamPath.empty()) {
        JsonError(res, "Steam not configured");
        return;
    }

    bool success = steam::RestartSteam(g_steamPath);
    json response = {
        {"success", success},
        {"message", success ? "Steam restarted" : "Restart failed"}
    };
    JsonResponse(res, response, success ? 200 : 500);
}

void HandleFixAllGames(const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!g_steamDetected || g_steamPath.empty() || g_steamUserId.empty()) {
        JsonError(res, "Steam not detected");
        return;
    }

    auto gamesList = games::DetectInstalledGames(g_steamPath, g_steamUserId);
    int fixCount = 0;
    std::vector<std::string> fixedGames;

    for (const auto& game : gamesList) {
        if (game.patched && !game.resolvedSavePath.empty()) {
            std::string targetPath = cloud::GetProviderSavePath(g_autoProvider, g_autoCustomPath, game.name, game.appId);
            if (!targetPath.empty()) {
                saves::SyncDirectories(game.resolvedSavePath, targetPath);
            }
            std::string universalPath = cloud::GetProviderSavePath("local", "", game.name, game.appId);
            if (!universalPath.empty() && universalPath != targetPath) {
                saves::SyncDirectories(game.resolvedSavePath, universalPath);
            }
            fs::path rcPath = fs::path(g_steamPath) / "userdata" / g_steamUserId / std::to_string(game.appId) / "remotecache.vdf";
            if (fs::exists(rcPath)) {
                try {
                    fs::remove(rcPath);
                } catch (...) {}
            }
            steam::TouchDirectoryFiles(game.resolvedSavePath);
            fixCount++;
            fixedGames.push_back(game.name);
        }
    }

    json response = {
        {"success", true},
        {"fixCount", fixCount},
        {"fixedGames", fixedGames},
        {"message", "Successfully fixed cloud sync for " + std::to_string(fixCount) + " game(s)."}
    };
    JsonResponse(res, response);
}

void HandleGetAutoPatch(const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    json response = {
        {"autoPatch", g_autoPatch.load()},
        {"provider", g_autoProvider},
        {"customPath", g_autoCustomPath},
        {"enableBackup", g_enableBackup.load()},
        {"backupPath", g_backupPath}
    };
    JsonResponse(res, response);
}

void HandleSetAutoPatch(const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (...) {
        JsonError(res, "Invalid JSON");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_autoPatch = body.value("autoPatch", false);
        g_autoProvider = body.value("provider", "");
        g_autoCustomPath = body.value("customPath", "");
        g_enableBackup = body.value("enableBackup", false);
        g_backupPath = body.value("backupPath", "");

        SaveConfigAll();

        if (g_steamDetected && !g_steamPath.empty()) {
            fs::path destDll = fs::path(g_steamPath) / "version.dll";
            if (g_autoPatch) {
                try {
                    fs::path exeDir = GetExecutableDir();
                    fs::path sourceDll = fs::path(exeDir) / "version.dll";
                    if (fs::exists(sourceDll)) {
                        fs::copy_file(sourceDll, destDll, fs::copy_options::overwrite_existing);
                    }
                } catch (...) {}
            } else {
                try {
                    if (fs::exists(destDll)) {
                        fs::remove(destDll);
                    }
                } catch (...) {}
            }
        }

        if (g_autoPatch && !g_autoProvider.empty() && g_steamDetected) {
            auto gamesList = games::DetectInstalledGames(g_steamPath, g_steamUserId);
            for (const auto& game : gamesList) {
                if (game.installed && !game.patched && !game.resolvedSavePath.empty()) {
                    std::string targetPath = cloud::GetProviderSavePath(g_autoProvider, g_autoCustomPath, game.name, game.appId);
                    if (!targetPath.empty()) {
                        if (g_enableBackup) {
                            saves::CreateBackup(game.resolvedSavePath, game.name, game.appId, g_backupPath);
                        }
                        saves::SyncDirectories(game.resolvedSavePath, targetPath);
                        std::string universalPath = cloud::GetProviderSavePath("local", "", game.name, game.appId);
                        if (!universalPath.empty() && universalPath != targetPath) {
                            saves::SyncDirectories(game.resolvedSavePath, universalPath);
                        }
                        games::g_patchedAppIds.insert(game.appId);
                    }
                }
            }
            SaveConfigAll();
        }
    }

    json response = {
        {"success", true},
        {"autoPatch", g_autoPatch.load()},
        {"provider", g_autoProvider},
        {"customPath", g_autoCustomPath},
        {"enableBackup", g_enableBackup.load()},
        {"backupPath", g_backupPath}
    };
    JsonResponse(res, response);
}

void HandleProviders(const httplib::Request&, httplib::Response& res) {
    auto providers = cloud::GetAvailableProviders();
    json response = json::array();
    for (const auto& p : providers) {
        response.push_back({
            {"id", p.id},
            {"name", p.name},
            {"path", p.path},
            {"detected", p.detected},
            {"icon", p.icon}
        });
    }
    JsonResponse(res, response);
}

void HandleBackups(const httplib::Request&, httplib::Response& res) {
    auto backups = saves::GetBackupsList();
    json response = json::array();
    for (const auto& b : backups) {
        response.push_back({
            {"gameName", b.gameName},
            {"appId", b.appId},
            {"path", b.path},
            {"timestamp", b.timestamp},
            {"sizeBytes", b.sizeBytes},
            {"sizeMB", static_cast<double>(b.sizeBytes) / (1024.0 * 1024.0)}
        });
    }
    JsonResponse(res, response);
}

void HandleSetSteamPath(const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (...) {
        JsonError(res, "Invalid JSON");
        return;
    }

    std::string newPath = body.value("path", "");
    if (newPath.empty()) {
        JsonError(res, "Missing path");
        return;
    }

    if (!fs::exists(newPath)) {
        JsonError(res, "Path does not exist: " + newPath);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_steamPath = newPath;
        g_steamDetected = true;
        auto userId = steam::GetSteamUserId(g_steamPath);
        if (userId.has_value()) {
            g_steamUserId = *userId;
        }
    }

    json response = {
        {"success", true},
        {"path", newPath},
        {"userId", g_steamUserId}
    };
    JsonResponse(res, response);
}

bool IsGameTracked(uint32_t appId) {
    if (games::g_patchedAppIds.find(appId) != games::g_patchedAppIds.end()) {
        return true;
    }
    if (g_autoPatch && !g_autoProvider.empty() && g_steamDetected) {
        auto list = games::DetectInstalledGames(g_steamPath, g_steamUserId);
        for (const auto& g : list) {
            if (g.appId == appId && g.installed) {
                return true;
            }
        }
    }
    return false;
}

void TriggerLaunchPatch(uint32_t appId) {
    if (!g_steamDetected || g_autoProvider.empty()) return;
    auto list = games::DetectInstalledGames(g_steamPath, g_steamUserId);
    for (const auto& g : list) {
        if (g.appId == appId && !g.resolvedSavePath.empty()) {
            std::string target = cloud::GetProviderSavePath(g_autoProvider, g_autoCustomPath, g.name, g.appId);
            if (!target.empty()) {
                if (g_enableBackup) {
                    saves::CreateBackup(g.resolvedSavePath, g.name, g.appId, g_backupPath);
                }
                saves::SyncDirectories(g.resolvedSavePath, target);
            }
            std::string universalPath = cloud::GetProviderSavePath("local", "", g.name, g.appId);
            if (!universalPath.empty() && universalPath != target) {
                saves::SyncDirectories(g.resolvedSavePath, universalPath);
            }
            if (games::g_patchedAppIds.find(appId) == games::g_patchedAppIds.end()) {
                games::g_patchedAppIds.insert(appId);
                SaveConfigAll();
            }
            break;
        }
    }
}

void TriggerExitPatch(uint32_t appId) {
    if (!g_steamDetected || g_autoProvider.empty()) return;
    auto list = games::DetectInstalledGames(g_steamPath, g_steamUserId);
    for (const auto& g : list) {
        if (g.appId == appId && !g.resolvedSavePath.empty()) {
            std::string target = cloud::GetProviderSavePath(g_autoProvider, g_autoCustomPath, g.name, g.appId);
            if (!target.empty()) {
                if (g_enableBackup) {
                    saves::CreateBackup(g.resolvedSavePath, g.name, g.appId, g_backupPath);
                }
                saves::SyncDirectories(g.resolvedSavePath, target);
            }
            std::string universalPath = cloud::GetProviderSavePath("local", "", g.name, g.appId);
            if (!universalPath.empty() && universalPath != target) {
                saves::SyncDirectories(g.resolvedSavePath, universalPath);
            }
            break;
        }
    }
}

void MonitorSteamRegistry(std::string steamPath, std::string userId) {
    fs::path dllPath = fs::path(steamPath) / "version.dll";
    if (fs::exists(dllPath)) {
        return;
    }
    (void)userId;
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam", 0, KEY_NOTIFY | KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    DWORD lastAppId = 0;
    while (g_running) {
        if (RegNotifyChangeKeyValue(hKey, TRUE, REG_NOTIFY_CHANGE_LAST_SET, nullptr, FALSE) != ERROR_SUCCESS) {
            break;
        }

        auto appVal = steam::ReadRegistryDword(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam", "RunningAppID");
        if (appVal.has_value()) {
            DWORD currentAppId = *appVal;
            if (currentAppId != lastAppId) {
                if (currentAppId != 0) {
                    if (IsGameTracked(currentAppId)) {
                        TriggerLaunchPatch(currentAppId);
                    }
                } else if (lastAppId != 0) {
                    if (IsGameTracked(lastAppId)) {
                        TriggerExitPatch(lastAppId);
                    }
                }
                lastAppId = currentAppId;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    RegCloseKey(hKey);
}

void StartWatchers() {
    if (!g_steamDetected || g_steamPath.empty() || g_steamUserId.empty()) return;
    std::thread regThread(MonitorSteamRegistry, g_steamPath, g_steamUserId);
    regThread.detach();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    std::string exeDir = GetExecutableDir();
    LoadConfig(exeDir);

    RefreshSteamState();
    StartWatchers();

    httplib::Server server;
    g_serverPtr = &server;

    server.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        AddCorsHeaders(res);
        res.status = 204;
    });

    server.Get("/api/status",             HandleStatus);
    server.Get("/api/steam",              HandleSteam);
    server.Get("/api/games",              HandleGames);
    server.Get("/api/providers",          HandleProviders);
    server.Get("/api/backups",            HandleBackups);
    server.Get("/api/settings/autopatch", HandleGetAutoPatch);

    server.Post(R"(/api/games/(\d+)/patch)",   HandlePatchGame);
    server.Post(R"(/api/games/(\d+)/restore)", HandleRestoreGame);
    server.Post("/api/games/fix-all",          HandleFixAllGames);
    server.Post("/api/settings/steam-path",    HandleSetSteamPath);
    server.Post("/api/steam/restart",          HandleRestartSteam);
    server.Post("/api/settings/autopatch",     HandleSetAutoPatch);

    std::string uiDir = (fs::path(exeDir) / "ui").string();
    if (fs::exists(uiDir)) {
        server.set_mount_point("/", uiDir);
    }

    server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        json error = {
            {"error", true},
            {"status", res.status},
            {"message", "Not Found"}
        };
        AddCorsHeaders(res);
        res.set_content(error.dump(2), "application/json");
    });

    bool silent = (lpCmdLine != nullptr && strstr(lpCmdLine, "--silent") != nullptr);
    if (!silent) {
        std::thread browserThread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            if (g_running) {
                std::string url = "http://localhost:" + std::to_string(SERVER_PORT);
                ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        });
        browserThread.detach();
    }

    if (!server.listen("0.0.0.0", SERVER_PORT)) {
        return 1;
    }

    return 0;
}
