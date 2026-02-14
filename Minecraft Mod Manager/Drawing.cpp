#include <Winsock2.h>
#include <set>
#include "Drawing.h"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "IniReader.h"
#include <filesystem>
#include "nlohmann\json.hpp"
#include <chrono>
#include "Shlwapi.h"
#include <stdlib.h>
#include <ShlObj.h>
#include <shobjidl.h>
#include <atlbase.h>
#include <codecvt>
#include <string_view>
#include <algorithm>
#include "include\bit7z\bitarchivereader.hpp"
#include "include\bit7z\bitfileextractor.hpp"
#include <curl/curl.h>
#include <thread>

using json = nlohmann::json;
using namespace std;
namespace fs = std::filesystem;
LPCSTR Drawing::lpWindowName = "Minecraft Mod Manager";
CIniReader iniReader("mods.ini");
bool EnableLog = iniReader.ReadBoolean("Settings", "EnableLogs", true);
float x = iniReader.ReadFloat("Settings", "WindowXSize", 500);
float y = iniReader.ReadFloat("Settings", "WindowYSize", 500);
ImVec2 Drawing::vWindowSize = ImVec2(x, y);
ImGuiWindowFlags Drawing::WindowFlags = 0;
bool Drawing::bDraw = true;
bool UpdateTags = false;
void Drawing::Active() { bDraw = true; }
bool Drawing::isActive() { return bDraw; }
std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}
static size_t WriteData(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}
struct DownloadStatus {
    std::atomic<float> progress{ 0.0f };
    std::atomic<bool> isDownloading{ false };
    std::wstring currentFileName;
    std::atomic<bool> isCopying{ false };
};
DownloadStatus g_Status;
static int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    if (dltotal > 0) {
        g_Status.progress = (float)dlnow / (float)dltotal;
    }
    return 0;
}
std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}
static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t totalSize = size * nitems;
    std::string header(buffer, totalSize);
    std::wstring* outName = static_cast<std::wstring*>(userdata);

    std::string headerLower = header;
    std::transform(headerLower.begin(), headerLower.end(), headerLower.begin(), ::tolower);

    size_t pos = headerLower.find("filename=");
    bool isExtended = false;

    if (pos == std::string::npos) {
        pos = headerLower.find("filename*=");
        if (pos != std::string::npos) {
            pos += 1;
            isExtended = true;
        }
    }

    if (pos != std::string::npos) {
        std::string rawName = header.substr(pos + 9);

        rawName.erase(std::remove(rawName.begin(), rawName.end(), '\"'), rawName.end());
        rawName.erase(std::remove(rawName.begin(), rawName.end(), ';'), rawName.end());

        size_t endPos = rawName.find_first_of("\r\n");
        if (endPos != std::string::npos) rawName.resize(endPos);

        if (isExtended && rawName.find("utf-8''") != std::string::npos) {
            rawName = rawName.substr(rawName.find("utf-8''") + 7);
        }

        if (rawName.length() > 0) {
            *outName = utf8_to_wstring(rawName);
        }
    }
    return totalSize;
}
HANDLE g_hChildStd_OUT_Rd = NULL;
HANDLE g_hChildStd_OUT_Wr = NULL;
bool IsServerReady = false;
HANDLE g_hChildStd_IN_Rd = NULL;
HANDLE g_hChildStd_IN_Wr = NULL;
HANDLE ServerHandle = nullptr;
bool DisableServerInstallButton = false;
bool IsServerInitializing = false;
bool IsStoppingServer = false;
bool ShowCommandLine = false;
bool IsInstallingServer = false;
char ServerCommand[260] = "";
bool ShowCancelButton = true;
bool ShowDeletionConfirmation = false;
bool ServerClosed = true;
bool ShowSuccess = false;
char CurrentFolder[MAX_PATH];
std::string StrCurrentDirectory = CurrentFolder;
void UpdateServerState() {
    if (ServerHandle == nullptr) return;
    DWORD exitCode;
    if (GetExitCodeProcess(ServerHandle, &exitCode) && exitCode != STILL_ACTIVE) {
        CloseHandle(ServerHandle);
        CloseHandle(g_hChildStd_IN_Wr);
        CloseHandle(g_hChildStd_OUT_Rd);
        ServerClosed = true;
        ServerHandle = nullptr;
        IsServerReady = false;
        IsStoppingServer = false;
        return;
    }

    DWORD dwAvail = 0;
    if (PeekNamedPipe(g_hChildStd_OUT_Rd, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
        std::vector<char> buffer(dwAvail + 1);
        DWORD dwRead;
        if (ReadFile(g_hChildStd_OUT_Rd, buffer.data(), dwAvail, &dwRead, NULL)) {
            buffer[dwRead] = '\0';
            std::string output(buffer.data());
            if (output.find("Done") != std::string::npos) {
                IsServerReady = true;
            }
        }
    }
}
void WriteToLog(const std::string& LoggingLine)
{
    if (EnableLog)
    {
        std::ofstream Log(StrCurrentDirectory + "\\MMM.log", std::ios_base::app);
        auto CurrentTimeA = std::chrono::system_clock::now();
        auto localzone = std::chrono::current_zone();
        auto LocalTime = std::chrono::zoned_time{ localzone, CurrentTimeA };
        std::string formatedTime = std::format("{:%Y-%m-%d %H:%M:%S}", LocalTime);
        formatedTime.insert(0, "[");
        formatedTime.insert(formatedTime.size(), "]");
        std::string HourAndLogLine = formatedTime + " " + LoggingLine;
        Log << std::setw(4) << HourAndLogLine << std::endl;
    }
}
HANDLE Execute(const std::wstring& cmdLine, const std::wstring& RunFrom, bool Silent, bool WaitTillFinish, bool CloseTheHandle)
{
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;
    STARTUPINFOW si = { 0 };
    if (!CloseTheHandle)
    {
        if (!CreatePipe(&g_hChildStd_IN_Rd, &g_hChildStd_IN_Wr, &saAttr, 0)) return NULL;
        SetHandleInformation(g_hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0);
        si.cb = sizeof(si);
        si.hStdInput = g_hChildStd_IN_Rd;
        si.dwFlags |= STARTF_USESTDHANDLES;
    }
    DWORD dwCreationFlags = 0;

    if (Silent)
    {
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        dwCreationFlags |= CREATE_NO_WINDOW;
    }
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CloseTheHandle)
    {
        CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &saAttr, 0);
        SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0);
        si.hStdOutput = g_hChildStd_OUT_Wr;
        si.hStdError = g_hChildStd_OUT_Wr;
    }
    LPWSTR CmdL = const_cast<LPWSTR>(cmdLine.c_str());
    if (CreateProcessW(
        nullptr,
        CmdL,
        NULL,
        NULL,
        TRUE,
        dwCreationFlags,
        NULL,
        RunFrom.c_str(),
        &si,
        &pi) == 0)
    {
        DWORD Error = GetLastError();
        WriteToLog("Error code:" + to_string(GetLastError()));
    }

    if (WaitTillFinish)
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    if (CloseTheHandle)
    {
        CloseHandle(g_hChildStd_IN_Wr);
        CloseHandle(g_hChildStd_IN_Rd);
        CloseHandle(pi.hProcess);
        ServerHandle = nullptr;
        g_hChildStd_IN_Wr = nullptr;
        g_hChildStd_IN_Rd = nullptr;
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
bool PickFile(HWND hwnd, std::wstring& path) {
    path.clear();
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)))
        return false;

    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr)) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT);
        hr = dlg->Show(hwnd);
        if (SUCCEEDED(hr)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                    path.assign(psz);
                    CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    CoUninitialize();
    return SUCCEEDED(hr);
}
bool IsFileAccessible(const std::string& filename)
{
    std::ifstream file(filename);
    return file.good();
}
void SendCommandToServer(const std::string& stopCmd) {
    if (g_hChildStd_IN_Wr == NULL) return;

    DWORD dwWritten;
    BOOL success = WriteFile(
        g_hChildStd_IN_Wr,
        stopCmd.c_str(),
        (DWORD)stopCmd.size(),
        &dwWritten,
        NULL
    );

    if (success) {
        FlushFileBuffers(g_hChildStd_IN_Wr);
    }
}
std::vector<std::string> getInDirectoryA(const std::string& directory, bool getFolder)
{
    std::vector<std::string> files;
    std::string searchPath = directory + "\\*";

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return files;

    do {
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (isDir == getFolder &&
            strcmp(fd.cFileName, ".") != 0 &&
            strcmp(fd.cFileName, "..") != 0)
        {
            files.push_back(fd.cFileName);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    
    return files;
}
std::vector<std::wstring> getInDirectoryW(const std::wstring& directory, bool getFolder)
{
    std::vector<std::wstring> files;
    std::wstring searchPath = directory + L"\\*";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return files;

    do {
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir == getFolder &&
            wcscmp(fd.cFileName, L".") != 0 &&
            wcscmp(fd.cFileName, L"..") != 0)
        {
            files.push_back(fd.cFileName);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    return files;
}
std::vector<std::wstring> getAllInDirectoryW(const std::wstring& directory)
{
    std::vector<std::wstring> files;
    std::wstring searchPath = directory + L"\\*";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return files;

    do {

        if (wcscmp(fd.cFileName, L".") != 0 &&
            wcscmp(fd.cFileName, L"..") != 0)
        {
            files.push_back(fd.cFileName);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    return files;
}
chrono::system_clock::time_point GFG(const string& datetimeString, const string& format)
{
    tm tmStruct = {};
    istringstream ss(datetimeString);
    ss >> get_time(&tmStruct, format.c_str());
    return chrono::system_clock::from_time_t(
        mktime(&tmStruct));
}
string DateTime(const chrono::system_clock::time_point& timePoint,
    const string& format)
{
    time_t time
        = chrono::system_clock::to_time_t(timePoint);
    tm* timeinfo = localtime(&time);
    char buffer[70];
    strftime(buffer, sizeof(buffer), format.c_str(),
        timeinfo);
    return buffer;
}
std::vector<std::string> Dates;
std::vector<std::string> LastVersions;
std::vector<std::string> FormattedDates;
std::vector<int> Times;
std::vector<chrono::system_clock::time_point> parsedTime;
chrono::system_clock::time_point CurrentTime;
int MostRecentDate = 0;
bool Remove = false;
bool Success = false;
bool ShowRAMError = false;
bool ShowCloseServerError = false;
bool ShowServerInstall = false;
bool DisableServerSelection = false;
bool DisableServerInstalling = false;
bool UpdateServerModsList = false;
bool UpdateServerResourcePackList = false;
char Namebuf[260] = "";
std::string ProfileDir = "";
std::string MostRecentVersion = "";
std::string MostRecentProfile = "";
std::vector<std::wstring> ServerModList;
std::string ServerResourcePackDir = "";
std::vector<std::wstring> ServerResourcePackList;
std::vector<std::wstring> ServerDisabledResourcePackList;
std::vector<std::string> ProfileNames;
std::vector<std::string> ProfilesWithVersions;
std::vector<std::string> UsedDates;
std::vector<std::string> NonModifiedUsedDates;
std::vector<std::string> UsedVersions;
std::vector<std::string> ServerUsedVersions;
std::vector<std::wstring> ServerListWithVersions;
std::vector<std::string> UsedProfiles;
std::vector<std::pair<std::wstring, int>> ModListAndEnabled;
std::vector<std::pair<std::wstring, int>> ModDisplayListAndEnabled;
std::vector<std::wstring> ModDescriptions;
std::vector<std::wstring> ModVersions;
std::vector<std::wstring> ModAuthors;
std::vector<std::wstring> ServerModDescriptions;
std::vector<std::wstring> ServerModVersions;
std::vector<std::wstring> ServerModAuthors;
std::vector<std::pair<std::wstring, int>> ResourcePackListAndEnabled;
std::vector<std::pair<std::wstring, int>> ResourcePackDisplayListAndEnabled;
std::vector<std::pair<std::wstring, int>> ConfigListAndEnabled;
std::vector<std::pair<std::wstring, int>> ShaderListAndEnabled;
std::vector<std::pair<std::wstring, int>> ShaderDisplayListAndEnabled;
std::vector<std::pair<std::wstring, int>> ServerModListAndEnabled;
std::vector<std::pair<std::wstring, int>> ServerDisplayModListAndEnabled;
std::vector<std::pair<std::wstring, int>> ServerDisplayResourcePackListAndEnabled;
std::vector<std::pair<std::wstring, int>> ServerResourcePackListAndEnabled;
std::string ServerModsDir = "";
std::string ServerConfigDir = "";
std::vector<std::wstring> ServerDisabledModList;
std::vector<std::wstring> ModList;
std::vector<std::wstring> ModDisplayList;
std::vector<std::wstring> DisabledModDisplayList;
std::vector<std::wstring> ResourcePackList;
std::vector<std::wstring> ShaderList;
std::vector<std::wstring> ConfigList;
std::vector<std::wstring> DisabledResourcePackList;
std::vector<std::wstring> DisabledShaderList;
std::vector<std::wstring> DisabledConfigList;
std::vector<std::string> gameDirs;
std::vector<std::string> DisabledMods;
std::vector<std::string> ModLoader;
std::vector<std::string> ProfileIds;
std::vector<std::string> UsedProfileIds;
std::vector<std::string> ServerList;
std::vector<std::string> ServerVersions;
std::vector<std::string> FabricServerVersions;
std::vector<std::string> EulaLines;
std::string MostRecentFabricLoaderVersion;
std::string modsDir = "";
std::string ResourcePackDir = "";
std::string ConfigDir = "";
std::string ShaderDir = "";
std::string ServerDir = "";
std::wstring DirToScanW = L"";
std::vector<std::string> FabricInstallerVersions;
std::string MostRecentFabricInstallerVersion = "";
std::vector<std::string> Options;
std::vector<std::string> ServerOptions;
std::vector<std::string> ServerOptionValues;
std::vector<std::string> NamedOptions;
std::vector<std::string> NamedOptionValues;
std::vector<std::string> NonNamedOptions;
std::vector<std::string> UsedGameDirs;
int MinimumServerRAM = 1;
int MaximumServerRAM = 2;
int PreSelectedIndex = 0;
bool ShowServerRAMError = false;
bool UpdateModsList = false;
bool UpdateShaderList = false;
bool UpdateResourcePackList = false;
bool InitName = true;
bool ShowCloseButton = false;
std::string extractBetweenDelimiters(const std::string& str, const std::string& start_delim, const std::string& end_delim) {
    size_t first_delim_pos = str.find(start_delim);

    if (first_delim_pos == string::npos) {
        return "";
    }
    size_t start_pos = first_delim_pos + start_delim.length();
    size_t second_delim_pos = str.find(end_delim, start_pos);
    if (second_delim_pos == string::npos) {
        return "";
    }
    size_t length = second_delim_pos - start_pos;
    return str.substr(start_pos, length);
}

std::string sSelectedFile = "";
std::string sFilePath = "";

bool FolderPick = false;
bool ModPackFolderPick = false;
bool ResourcePackFolderPick = false;
bool ShaderFolderPick = false;
bool ServerFolderPick = false;
bool ServerModPackFolderPick = false;
bool ServerResourcePackFolderPick = false;
int SelectedIndex = 0;
int ServerSelectedIndex = 0;
int UnitSelectedIndex = 2;
int ServerVersionsSelectedIndex = 0;
int PreServerVersionsSelectedIndex = 0;
int PreServerSelectedIndex = 0;
bool DoOnce = true;
bool AskNamePrompt = false;
bool ShowEqualNameError = false;
char Path[260];
bool RebuildInstalledMods = false;
std::string minecraftDir = "";
char ServerName[260] = "";
int MinimumRAM = 1;
int MaximumRAM = 2;
std::vector<std::string> Unit = { "KB", "MB", "GB" };
std::vector<std::string> Unit2 = { "KB", "MB", "GB" };
int Unit2SelectedIndex = 2;
bool ClearIsEnabled = false;
std::string profilesDir = "";
std::string CurrentProfileDir = "";
bool ServerInit = true;
bool NameAlreadyExists = false;
bool ShowNameChange = false;
bool ShowChangeServerNameButton = true;
bool RewriteProfiles = false;
bool Result = false;
bool UpdateLastUsed = false;
bool ResetLog = true;
bool ShowRenameBox = false;
char buf[260];
std::vector<std::wstring> DisabledModList;
void Drawing::Draw() {
    if (ResetLog)
    {
        GetCurrentDirectoryA(MAX_PATH, CurrentFolder);
        StrCurrentDirectory = CurrentFolder;
        if (PathFileExistsA((StrCurrentDirectory + "\\MMM.log").c_str()))
        {
            DeleteFileA((StrCurrentDirectory + "\\MMM.log").c_str());
        }
        ResetLog = false;
    }
    if (DoOnce)
    {
        strcpy(buf, "");
        ModList.clear();
        DisabledModList.clear();
        ModListAndEnabled.clear();
        RewriteProfiles = false;
        ProfileIds.clear();
        UsedProfiles.clear();
        UsedVersions.clear();
        LastVersions.clear();
        ProfileNames.clear();
        ProfilesWithVersions.clear();
        Dates.clear();
        UsedDates.clear();
        UsedVersions.clear();
        FormattedDates.clear();
        Times.clear();
        SelectedIndex = 0;
        PreSelectedIndex = 0;
        MostRecentProfile.clear();
        MostRecentDate = 0;
        parsedTime.clear();
        DirToScanW.clear();
        char appdata[MAX_PATH];
        GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
        minecraftDir = std::string(appdata) + "\\.minecraft";
        profilesDir = minecraftDir + "\\profiles";
        if (!PathFileExistsA(minecraftDir.c_str()))
        {
            fs::create_directory(minecraftDir);
        }
        if (!PathFileExistsA(profilesDir.c_str()))
        {
            fs::create_directory(profilesDir);
        }
        if (PathFileExistsA((minecraftDir + "\\launcher_profiles.json").c_str()) && !fs::is_empty(minecraftDir + "\\launcher_profiles.json"))
        {
            std::ifstream LauncherProfiles(minecraftDir + "\\launcher_profiles.json");
            const string format = "%Y-%m-%d %H:%M:%S";
            json data = json::parse(LauncherProfiles);
            for (auto& [id, profile] : data["profiles"].items())
            {
                ProfileIds.push_back(std::string(id));
                LastVersions.push_back(profile.value("lastVersionId", ""));
                ProfileNames.push_back(profile.value("name", ""));
                Dates.push_back(profile.value("lastUsed", ""));
                gameDirs.push_back(profile.value("gameDir", ""));
                const std::string& lastVersion = LastVersions.back();
                const std::string& date = Dates.back();
                const std::string& name = ProfileNames.back();
                const std::string& id = ProfileIds.back();
                const std::string& gameDir = gameDirs.back();
                if (lastVersion.find("fabric") != std::string::npos || lastVersion.find("forge") != std::string::npos)
                {
                    UsedProfileIds.push_back(id);
                    if (lastVersion.find("fabric") != std::string::npos)
                    {
                        ModLoader.push_back("fabric");
                    }
                    else if (lastVersion.find("forge") != std::string::npos)
                    {
                        ModLoader.push_back("forge");
                    }
                    else
                    {
                        ModLoader.push_back("");
                    }
                    UsedDates.push_back(date);
                    NonModifiedUsedDates.push_back(date);
                    UsedVersions.push_back(lastVersion);
                    UsedProfiles.push_back(name);
                    if (!gameDir.empty())
                    {
                        UsedGameDirs.push_back(gameDir);
                    }
                    else
                    {
                        UsedGameDirs.push_back(profilesDir + "\\" + UsedProfiles.back());
                        profile["gameDir"] = profilesDir + "\\" + UsedProfiles.back();
                        RewriteProfiles = true;
                    }
                }
            }
            if (RewriteProfiles)
            {
                std::ofstream o(minecraftDir + "\\launcher_profiles.json");
                o << std::setw(4) << data << std::endl;
                RewriteProfiles = false;
            }
            for (int i = 0; i < UsedVersions.size(); i++)
            {
                if (UsedVersions[i].find("+build") != string::npos)
                {
                    UsedVersions[i].erase(0, 31);
                }
                else
                {
                    UsedVersions[i].erase(0, 21);
                }
                ProfilesWithVersions.push_back(UsedProfiles[i] + " (" + UsedVersions[i] + ")");
            }
            for (int i = 0; i < ProfilesWithVersions.size(); i++)
            {
                UsedDates[i].erase(23, 1);
                UsedDates[i].erase(10, 1);
                UsedDates[i].insert(10, " ");
                parsedTime.push_back(GFG(UsedDates[i], format));
                Times.push_back(static_cast<int>(chrono::system_clock::to_time_t(parsedTime[i])));
                if (Times[i] > MostRecentDate) {
                    MostRecentDate = Times[i];
                    MostRecentProfile = ProfilesWithVersions[i];
                    SelectedIndex = i;
                    PreSelectedIndex = i;
                }
            }
            for (std::string CurrentProfile : UsedProfiles)
            {
                if (!fs::exists(profilesDir + "\\" + CurrentProfile))
                {
                    fs::create_directory(profilesDir + "\\" + CurrentProfile);
                }
                if (!PathFileExistsA((profilesDir + "\\" + CurrentProfile + "\\mods").c_str()))
                {
                    fs::create_directory(profilesDir + "\\" + CurrentProfile + "\\mods");
                }
                if (!PathFileExistsA((profilesDir + "\\" + CurrentProfile + "\\mods\\Disabled Mods").c_str()))
                {
                    fs::create_directory(profilesDir + "\\" + CurrentProfile + "\\mods\\Disabled Mods");
                }
                if (!PathFileExistsA((profilesDir + "\\" + CurrentProfile + "\\config").c_str()))
                {
                    fs::create_directory(profilesDir + "\\" + CurrentProfile + "\\config");
                }
                if (!PathFileExistsA((profilesDir + "\\" + CurrentProfile + "\\resourcepacks").c_str()))
                {
                    fs::create_directory(profilesDir + "\\" + CurrentProfile + "\\resourcepacks");
                }
                if (!PathFileExistsA((profilesDir + "\\" + CurrentProfile + "\\resourcepacks\\Disabled Resource Packs").c_str()))
                {
                    fs::create_directory(profilesDir + "\\" + CurrentProfile + "\\resourcepacks\\Disabled Resource Packs");
                }
                if (!PathFileExistsA((profilesDir + "\\" + CurrentProfile + "\\shaderpacks").c_str()))
                {
                    fs::create_directory(profilesDir + "\\" + CurrentProfile + "\\shaderpacks");
                }
                if (!PathFileExistsA((profilesDir + "\\" + CurrentProfile + "\\shaderpacks\\Disabled Shaders").c_str()))
                {
                    fs::create_directory(profilesDir + "\\" + CurrentProfile + "\\shaderpacks\\Disabled Shaders");
                }
            }
            if (!ProfilesWithVersions.empty())
            {
                ProfilesWithVersions[SelectedIndex] = MostRecentProfile.c_str();
            }
            if (!UsedProfiles.empty())
            {
                modsDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\mods";
                ConfigDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\config";
                ShaderDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\shaderpacks";
                ResourcePackDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\resourcepacks";
                ProfileDir = profilesDir + "\\" + UsedProfiles[SelectedIndex];
            }
            else
            {
                modsDir = minecraftDir + "\\mods";
                ConfigDir = minecraftDir + "\\config";
                ShaderDir = minecraftDir + "\\shaderpacks";
                ResourcePackDir = minecraftDir + "\\resourcepacks";
            }
            bit7z::Bit7zLibrary lib{ "7z.dll" };
            std::vector<bit7z::byte_t> ModBuffer;
            ModList = getInDirectoryW(utf8_to_wstring(modsDir), false);
            for (int i = 0; i < ModList.size(); i++)
            {
                if (fs::path(ModList[i]).extension() == ".jar")
                {
                    std::wstring Name = L"";
                    std::wstring Description = L"";
                    std::wstring Version = L"";
                    std::vector<std::string> AuthorStr;
                    std::string AuthorNJ = "";
                    json Author = json::array();
                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                    extractor.extractMatching(modsDir + "\\" + wstring_to_utf8(ModList[i]), "fabric.mod.json", ModBuffer);
                    if (!ModBuffer.empty())
                    {
                        try
                        {
                            json mod = json::parse(ModBuffer);
                            Name = utf8_to_wstring(mod.value("name", ""));
                            Description = utf8_to_wstring(mod.value("description", ""));
                            Version = utf8_to_wstring(mod.value("version", ""));
                            if (mod.contains("authors") && mod["authors"].is_array()) {
                                for (auto& item : mod["authors"]) {
                                    if (item.is_string()) {
                                        AuthorStr.push_back(item.get<std::string>());
                                    }
                                    else if (item.is_object() && item.contains("name")) {
                                        AuthorStr.push_back(item["name"].get<std::string>());
                                    }
                                }
                            }
                            for (size_t i = 0; i < AuthorStr.size(); ++i) {
                                AuthorNJ += AuthorStr[i];
                                if (i < AuthorStr.size() - 1) {
                                    AuthorNJ += ", ";
                                }
                            }
                        }
                        catch (json::exception& except)
                        {
                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ModList[i]));
                        }
                    }
                    if (Name != L"")
                    {
                        ModDisplayListAndEnabled.push_back(make_pair(Name, true));
                    }
                    else
                    {
                        ModDisplayListAndEnabled.push_back(make_pair(ModList[i], true));
                    }
                    ModAuthors.push_back(utf8_to_wstring(AuthorNJ));
                    ModDescriptions.push_back(Description);
                    ModVersions.push_back(Version);
                    ModListAndEnabled.push_back(make_pair(ModList[i], true));
                }
            }
            DisabledModList = getInDirectoryW(utf8_to_wstring(modsDir) + L"\\Disabled Mods\\", false);
            for (int i = 0; i < DisabledModList.size(); i++)
            {
                    std::wstring Name = L"";
                    std::wstring Description = L"";
                    std::wstring Version = L"";
                    std::vector<std::string> AuthorStr;
                    std::string AuthorNJ = "";
                    json Author = json::array();
                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                    extractor.extractMatching(modsDir + "\\Disabled Mods\\" + wstring_to_utf8(DisabledModList[i]), "fabric.mod.json", ModBuffer);
                    if (!ModBuffer.empty())
                    {
                        try
                        {
                            json mod = json::parse(ModBuffer);
                            Name = utf8_to_wstring(mod.value("name", ""));
                            Description = utf8_to_wstring(mod.value("description", ""));
                            Version = utf8_to_wstring(mod.value("version", ""));
                            if (mod.contains("authors") && mod["authors"].is_array()) {
                                for (auto& item : mod["authors"]) {
                                    if (item.is_string()) {
                                        AuthorStr.push_back(item.get<std::string>());
                                    }
                                    else if (item.is_object() && item.contains("name")) {
                                        AuthorStr.push_back(item["name"].get<std::string>());
                                    }
                                }
                            }
                            for (size_t i = 0; i < AuthorStr.size(); ++i) {
                                AuthorNJ += AuthorStr[i];
                                if (i < AuthorStr.size() - 1) {
                                    AuthorNJ += ", ";
                                }
                            }
                        }
                        catch (json::exception& except)
                        {
                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(DisabledModList[i]));
                        }
                    }
                    if (Name != L"")
                    {
                        ModDisplayListAndEnabled.push_back(make_pair(Name, false));
                    }
                    else
                    {
                        ModDisplayListAndEnabled.push_back(make_pair(DisabledModList[i], false));
                    }
                    ModAuthors.push_back(utf8_to_wstring(AuthorNJ));
                    ModDescriptions.push_back(Description);
                    ModVersions.push_back(Version);
                    ModListAndEnabled.push_back(make_pair(DisabledModList[i], false));
            }
            std::sort(ModDisplayListAndEnabled.begin(), ModDisplayListAndEnabled.end());
            ResourcePackList = getInDirectoryW(utf8_to_wstring(ResourcePackDir), false);
            std::vector<bit7z::byte_t> ResourcePackBuffer;
            for (int i = 0; i < ResourcePackList.size(); i++)
            {
                std::wstring Name = L"";
                if (fs::path(ResourcePackList[i]).extension() == ".zip")
                {
                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                    try
                    {
                        extractor.extractMatching(ResourcePackDir + "\\" + wstring_to_utf8(ResourcePackList[i]), "pack.mcmeta", ResourcePackBuffer);
                    }
                    catch (const bit7z::BitException& except)
                    {
                        WriteToLog(except.what());
                    }
                    if (!ResourcePackBuffer.empty())
                    {
                        try
                        {
                            json resourcepack = json::parse(ResourcePackBuffer);
                            Name = utf8_to_wstring(resourcepack["pack"].value("name", ""));
                        }
                        catch (json::exception& except)
                        {
                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ResourcePackList[i]));
                        }
                    }
                    if (Name != L"")
                    {
                        ResourcePackDisplayListAndEnabled.push_back(make_pair(Name, true));
                    }
                    else
                    {
                        ResourcePackDisplayListAndEnabled.push_back(make_pair(ResourcePackList[i], true));
                    }
                    ResourcePackListAndEnabled.push_back(make_pair(ResourcePackList[i], true));
                }
            }
            DisabledResourcePackList = getInDirectoryW(utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\", false);
            for (int i = 0; i < DisabledResourcePackList.size(); i++)
            {
                std::wstring Name = L"";
                if (fs::path(DisabledResourcePackList[i]).extension() == ".zip")
                {
                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                    try
                    {
                        extractor.extractMatching(ResourcePackDir + "\\Disabled Resource Packs\\" + wstring_to_utf8(DisabledResourcePackList[i]), "pack.mcmeta", ResourcePackBuffer);
                    }
                    catch (bit7z::BitException& except)
                    {
                        WriteToLog(except.what());
                    }
                    if (!ResourcePackBuffer.empty())
                    {
                        try
                        {
                            json resourcepack = json::parse(ResourcePackBuffer);
                            Name = utf8_to_wstring(resourcepack["pack"].value("name", ""));
                        }
                        catch (json::exception& except)
                        {
                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ResourcePackList[i]));
                        }
                    }
                    if (Name != L"")
                    {
                        ResourcePackDisplayListAndEnabled.push_back(make_pair(Name, false));
                    }
                    else
                    {
                        ResourcePackDisplayListAndEnabled.push_back(make_pair(DisabledResourcePackList[i], false));
                    }
                    ResourcePackListAndEnabled.push_back(make_pair(DisabledResourcePackList[i], false));
                }
            }
            std::sort(ResourcePackDisplayListAndEnabled.begin(), ResourcePackDisplayListAndEnabled.end());
            ShaderList = getInDirectoryW(utf8_to_wstring(ShaderDir), false);
            for (int i = 0; i < ShaderList.size(); i++)
            {
                if (fs::path(ShaderList[i]).extension() == ".zip")
                {
                    ShaderListAndEnabled.push_back(make_pair(ShaderList[i], true));
                }
            }
            DisabledShaderList = getInDirectoryW(utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\", false);
            for (int i = 0; i < DisabledShaderList.size(); i++)
            {
                if (fs::path(DisabledShaderList[i]).extension() == ".zip")
                {
                    ShaderListAndEnabled.push_back(make_pair(DisabledShaderList[i], false));
                }
            }
            std::sort(ShaderListAndEnabled.begin(), ShaderListAndEnabled.end());
            /*ConfigList = getInDirectoryW(utf8_to_wstring(ConfigDir), false);
            for (int i = 0; i < ConfigList.size(); i++)
            {
                ConfigListAndEnabled.push_back(make_pair(ConfigList[i], true));
            }
            DisabledConfigList = getInDirectoryW(utf8_to_wstring(ConfigDir) + L"\\Disabled Resource Packs\\", false);
            for (int i = 0; i < DisabledConfigList.size(); i++)
            {
                ConfigListAndEnabled.push_back(make_pair(DisabledConfigList[i], false));
            }
            std::sort(ConfigListAndEnabled.begin(), ConfigListAndEnabled.end());*/
        }
        else
        {
            if (MessageBoxA(NULL, "You need a valid Minecraft installation for this (launcher_profiles.json is empty/missing!)", NULL, MB_OK) == IDOK)
            {
                PostQuitMessage(0);
            }
        }
        DoOnce = false;
        }
        
    if (ServerInit)
    {
        ShowServerInstall = false;
        Success = false;
        EulaLines.clear();
        FabricServerVersions.clear();
        ServerDir = minecraftDir + "\\servers";
        ServerUsedVersions.clear();
        ServerList.clear();
        ServerList = getInDirectoryA(ServerDir, true);
        if (!ServerList.empty())
        {
            ServerModsDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods";
            ServerConfigDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\config";
            ServerResourcePackDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\resourcepacks";
        }
        for (std::string& ServerItem : ServerList)
        {
            bit7z::Bit7zLibrary lib{ "7z.dll" };
            std::vector<bit7z::byte_t> ServerBuffer;
            bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
            try
            {
                extractor.extractMatching(ServerDir + "\\" + ServerItem + "\\server.jar", "install.properties", ServerBuffer);
            }
            catch (bit7z::BitException& except)
            {
                WriteToLog(except.what() + std::string(" At File: ") + ServerDir + "\\" + ServerItem + "server.jar");
            }
            std::string properties = "";
            for (int i = 0; i < ServerBuffer.size(); i++)
            {
                properties += ServerBuffer[i];
            }
            size_t pos = properties.find("game-version=");
            if (pos != std::string::npos)
            {
                std::string GameVer = properties.substr(pos + 13);
                ServerUsedVersions.push_back(GameVer);
                ServerListWithVersions.push_back(utf8_to_wstring(ServerItem + " (" + GameVer + ")"));
            }
        }
        MostRecentFabricInstallerVersion.clear();
        MostRecentFabricLoaderVersion = "";
        AskNamePrompt = false;
        ServerSelectedIndex = 0;
        PreServerSelectedIndex = 0;
        ServerVersionsSelectedIndex = 0;
        UnitSelectedIndex = 2;
        MinimumRAM = 1;
        MaximumRAM = 2;
        ShowRAMError = false;
        strcpy(ServerName, "");
        strcpy(Namebuf, "");
        NameAlreadyExists = false;
        strcpy(ServerCommand, "");
        if (!PathFileExistsA(ServerDir.c_str()))
        {
            fs::create_directory(ServerDir);
        }
        for (int i = 0; i < ServerList.size(); i++)
        {
            if (!PathFileExistsA((ServerDir + "\\" + ServerList[i]).c_str()))
            {
                fs::create_directory(ServerDir + "\\" + ServerList[i]);
            }
            if (!PathFileExistsA((ServerDir + "\\" + ServerList[i] + "\\mods").c_str()))
            {
                fs::create_directory(ServerDir + "\\" + ServerList[i] + "\\mods");
            }
            if (!PathFileExistsA((ServerDir + "\\" + ServerList[i] + "\\mods\\Disabled Mods").c_str()))
            {
                fs::create_directory(ServerDir + "\\" + ServerList[i] + "\\mods\\Disabled Mods");
            }
            if (!PathFileExistsA((ServerDir + "\\" + ServerList[i] + "\\config").c_str()))
            {
                fs::create_directory(ServerDir + "\\" + ServerList[i] + "\\config");
            }
            if (!PathFileExistsA((ServerDir + "\\" + ServerList[i] + "\\resourcepacks").c_str()))
            {
                fs::create_directory(ServerDir + "\\" + ServerList[i] + "\\resourcepacks");
            }
            if (!PathFileExistsA((ServerDir + "\\" + ServerList[i] + "\\resourcepacks\\Disabled Resource Packs").c_str()))
            {
                fs::create_directory(ServerDir + "\\" + ServerList[i] + "\\resourcepacks\\Disabled Resource Packs");
            }
        }
        if (!ServerList.empty())
        {
            bit7z::Bit7zLibrary lib{ "7z.dll" };
            std::vector<bit7z::byte_t> ModBuffer;
            ServerModList = getInDirectoryW(utf8_to_wstring(ServerModsDir), false);
            for (int i = 0; i < ServerModList.size(); i++)
            {
                if (fs::path(ServerModList[i]).extension() == ".jar")
                {
                    std::wstring Name = L"";
                    std::wstring Description = L"";
                    std::wstring Version = L"";
                    std::vector<std::string> AuthorStr;
                    std::string AuthorNJ = "";
                    json Author = json::array();
                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                    extractor.extractMatching(ServerModsDir + "\\" + wstring_to_utf8(ServerModList[i]), "fabric.mod.json", ModBuffer);
                    if (!ModBuffer.empty())
                    {
                        try
                        {
                            json mod = json::parse(ModBuffer);
                            Name = utf8_to_wstring(mod.value("name", ""));
                            Description = utf8_to_wstring(mod.value("description", ""));
                            Version = utf8_to_wstring(mod.value("version", ""));
                            if (mod.contains("authors") && mod["authors"].is_array()) {
                                for (auto& item : mod["authors"]) {
                                    if (item.is_string()) {
                                        AuthorStr.push_back(item.get<std::string>());
                                    }
                                    else if (item.is_object() && item.contains("name")) {
                                        AuthorStr.push_back(item["name"].get<std::string>());
                                    }
                                }
                            }
                            for (size_t i = 0; i < AuthorStr.size(); ++i) {
                                AuthorNJ += AuthorStr[i];
                                if (i < AuthorStr.size() - 1) {
                                    AuthorNJ += ", ";
                                }
                            }
                        }
                        catch (json::exception& except)
                        {
                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ServerModList[i]));
                        }
                    }
                    if (Name != L"")
                    {
                        ServerDisplayModListAndEnabled.push_back(make_pair(Name, true));
                    }
                    else
                    {
                        ServerDisplayModListAndEnabled.push_back(make_pair(ServerModList[i], true));
                    }
                    ServerModAuthors.push_back(utf8_to_wstring(AuthorNJ));
                    ServerModDescriptions.push_back(Description);
                    ServerModVersions.push_back(Version);
                    ServerModListAndEnabled.push_back(make_pair(ServerModList[i], true));
                }
            }
            ServerDisabledModList = getInDirectoryW(utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\", false);
            for (int i = 0; i < ServerDisabledModList.size(); i++)
            {
                std::wstring Name = L"";
                std::wstring Description = L"";
                std::wstring Version = L"";
                std::vector<std::string> AuthorStr;
                std::string AuthorNJ = "";
                json Author = json::array();
                bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                extractor.extractMatching(ServerModsDir + "\\Disabled Mods\\" + wstring_to_utf8(ServerDisabledModList[i]), "fabric.mod.json", ModBuffer);
                if (!ModBuffer.empty())
                {
                    try
                    {
                        json mod = json::parse(ModBuffer);
                        Name = utf8_to_wstring(mod.value("name", ""));
                        Description = utf8_to_wstring(mod.value("description", ""));
                        Version = utf8_to_wstring(mod.value("version", ""));
                        if (mod.contains("authors") && mod["authors"].is_array()) {
                            for (auto& item : mod["authors"]) {
                                if (item.is_string()) {
                                    AuthorStr.push_back(item.get<std::string>());
                                }
                                else if (item.is_object() && item.contains("name")) {
                                    AuthorStr.push_back(item["name"].get<std::string>());
                                }
                            }
                        }
                        for (size_t i = 0; i < AuthorStr.size(); ++i) {
                            AuthorNJ += AuthorStr[i];
                            if (i < AuthorStr.size() - 1) {
                                AuthorNJ += ", ";
                            }
                        }
                    }
                    catch (json::exception& except)
                    {
                        WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ServerDisabledModList[i]));
                    }
                }
                if (Name != L"")
                {
                    ServerDisplayModListAndEnabled.push_back(make_pair(Name, false));
                }
                else
                {
                    ServerDisplayModListAndEnabled.push_back(make_pair(ServerDisabledModList[i], false));
                }
                ServerModAuthors.push_back(utf8_to_wstring(AuthorNJ));
                ServerModDescriptions.push_back(Description);
                ServerModVersions.push_back(Version);
                ServerModListAndEnabled.push_back(make_pair(DisabledModList[i], false));
            }
            std::sort(ServerDisplayModListAndEnabled.begin(), ServerDisplayModListAndEnabled.end());
            ServerResourcePackList = getInDirectoryW(utf8_to_wstring(ServerResourcePackDir), false);
            std::vector<bit7z::byte_t> ResourcePackBuffer;
            for (int i = 0; i < ServerResourcePackList.size(); i++)
            {
                std::wstring Name = L"";
                if (fs::path(ServerResourcePackList[i]).extension() == ".zip")
                {
                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                    try
                    {
                        extractor.extractMatching(ServerResourcePackDir + "\\" + wstring_to_utf8(ServerResourcePackList[i]), "pack.mcmeta", ResourcePackBuffer);
                    }
                    catch (const bit7z::BitException& except)
                    {
                        WriteToLog(except.what());
                    }
                    if (!ResourcePackBuffer.empty())
                    {
                        try
                        {
                            json resourcepack = json::parse(ResourcePackBuffer);
                            Name = utf8_to_wstring(resourcepack["pack"].value("name", ""));
                        }
                        catch (json::exception& except)
                        {
                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ServerResourcePackList[i]));
                        }
                    }
                    if (Name != L"")
                    {
                        ServerDisplayResourcePackListAndEnabled.push_back(make_pair(Name, true));
                    }
                    else
                    {
                        ServerDisplayResourcePackListAndEnabled.push_back(make_pair(ServerResourcePackList[i], true));
                    }
                    ServerResourcePackListAndEnabled.push_back(make_pair(ServerResourcePackList[i], true));
                }
            }
            ServerDisabledResourcePackList = getInDirectoryW(utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\", false);
            for (int i = 0; i < ServerDisabledResourcePackList.size(); i++)
            {
                std::wstring Name = L"";
                if (fs::path(ServerDisabledResourcePackList[i]).extension() == ".zip")
                {
                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                    try
                    {
                        extractor.extractMatching(ServerResourcePackDir + "\\Disabled Resource Packs\\" + wstring_to_utf8(ServerDisabledResourcePackList[i]), "pack.mcmeta", ResourcePackBuffer);
                    }
                    catch (bit7z::BitException& except)
                    {
                        WriteToLog(except.what());
                    }
                    if (!ResourcePackBuffer.empty())
                    {
                        try
                        {
                            json resourcepack = json::parse(ResourcePackBuffer);
                            Name = utf8_to_wstring(resourcepack["pack"].value("name", ""));
                        }
                        catch (json::exception& except)
                        {
                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ResourcePackList[i]));
                        }
                    }
                    if (Name != L"")
                    {
                        ServerDisplayResourcePackListAndEnabled.push_back(make_pair(Name, false));
                    }
                    else
                    {
                        ServerDisplayResourcePackListAndEnabled.push_back(make_pair(ServerDisabledResourcePackList[i], false));
                    }
                    ServerResourcePackListAndEnabled.push_back(make_pair(DisabledResourcePackList[i], false));
                }
            }
            std::sort(ServerDisplayResourcePackListAndEnabled.begin(), ServerDisplayResourcePackListAndEnabled.end());
        }
        ServerInit = false;
    }
    if (isActive()) {
        ImGui::SetNextWindowSize(vWindowSize, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(1.0f);
        if (ImGui::Begin(lpWindowName, &bDraw, WindowFlags))
        {
            ImVec2 currentSize = ImGui::GetWindowSize();
            if (vWindowSize.x != currentSize.x)
            {
                iniReader.WriteFloat("Settings", "WindowXSize", currentSize.x);
            }
            if (vWindowSize.y != currentSize.y)
            {
                iniReader.WriteFloat("Settings", "WindowYSize", currentSize.y);
            }
            if (ImGui::BeginTabBar("NOTITLE", ImGuiTabBarFlags_NoTooltip))
            {
                if (ImGui::BeginTabItem("Profiles"))
                {
                    if (ImGui::Button("Reload Profiles"))
                    {
                        ModDisplayListAndEnabled.clear();
                        ResourcePackDisplayListAndEnabled.clear();
                        ModListAndEnabled.clear();
                        ResourcePackListAndEnabled.clear();
                        ShaderListAndEnabled.clear();
                        ModVersions.clear();
                        ModVersions.clear();
                        ModDescriptions.clear();
                        DoOnce = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Open Fabric Installer"))
                    {
                        std::thread([=]()
                        {
                                if (!PathFileExistsA("index.html"))
                                {
                                    CURL* curl = curl_easy_init();
                                    if (curl) {
                                        curl_easy_setopt(curl, CURLOPT_URL, "https://maven.fabricmc.net/net/fabricmc/fabric-installer/");
                                        FILE* fp = _wfopen((utf8_to_wstring("index.html")).c_str(), L"wb");
                                        if (fp == NULL) {
                                            perror("File open failed");
                                            return;
                                        }
                                        if (fp) {
                                            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                            curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                            CURLcode res = curl_easy_perform(curl);
                                            g_Status.currentFileName = L"Downloading asset:index.html";
                                            if (res != CURLE_OK) {
                                                if (fs::exists("index.html"))
                                                {
                                                    fs::remove("index.html");
                                                }
                                            }
                                            fclose(fp);
                                        }
                                        curl_easy_cleanup(curl);
                                    }
                                }
                                if (FabricInstallerVersions.empty())
                                {
                                    std::ifstream index("index.html");
                                    std::string currin;
                                    if (index.is_open())
                                    {
                                        while (std::getline(index, currin)) {
                                            if (currin.find("href=") != string::npos && currin.find("<h1>") == string::npos && currin.find("maven") == string::npos)
                                            {
                                                FabricInstallerVersions.push_back(currin);
                                            }
                                        }
                                        index.close();
                                    }
                                }
                                if (!FabricInstallerVersions.empty())
                                {
                                    std::string VersionToInstall = extractBetweenDelimiters(FabricInstallerVersions[FabricInstallerVersions.size() - 1], R"(")", R"(/)");
                                    std::string FabricInstaller = "fabric-installer-" + VersionToInstall + ".exe";
                                    if (!PathFileExistsA((FabricInstaller).c_str()))
                                    {
                                        std::vector<std::string> DirFiles = getInDirectoryA(CurrentFolder, false);
                                        for (std::string File : DirFiles)
                                        {
                                            if (File.find("fabric-installer") != string::npos)
                                            {
                                                DeleteFileA(File.c_str());
                                            }
                                        }
                                        CURL* curl = curl_easy_init();
                                        if (curl) {
                                            curl_easy_setopt(curl, CURLOPT_URL, ("https://maven.fabricmc.net/net/fabricmc/fabric-installer/" + VersionToInstall + "/fabric-installer-" + VersionToInstall + ".exe").c_str());
                                            FILE* fp = fopen(("fabric-installer-" + VersionToInstall + ".exe").c_str(), "wb");
                                            if (fp == NULL) {
                                                perror("File open failed");
                                                return;
                                            }
                                            if (fp) {
                                                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                CURLcode res = curl_easy_perform(curl);
                                                g_Status.isDownloading = true;
                                                g_Status.currentFileName = L"Downloading asset: fabric-installer-" + utf8_to_wstring(VersionToInstall) + L".exe";
                                                if (res != CURLE_OK) {
                                                    if (fs::exists("fabric-installer-" + VersionToInstall + ".exe"))
                                                    {
                                                        fs::remove("fabric-installer-" + VersionToInstall + ".exe");
                                                    }
                                                }
                                                fclose(fp);
                                            }
                                            curl_easy_cleanup(curl);
                                        }
                                    }
                                    Execute(utf8_to_wstring(FabricInstaller), utf8_to_wstring(StrCurrentDirectory), false, false, true);
                                    g_Status.isDownloading = false;
                                }
                                DeleteFileA("index.html");
                        }).detach();
                    }
                    if (!UsedProfiles.empty())
                    {
                        ImGui::Text("Current Modded Profile:");
                        ImGui::SameLine();
                        if (ImGui::BeginCombo("##Combo", ProfilesWithVersions[SelectedIndex].c_str())) {
                            for (int i = 0; i < ProfilesWithVersions.size(); ++i)
                            {
                                const bool isSelected = (SelectedIndex == i);
                                if (ImGui::Selectable(ProfilesWithVersions[i].c_str(), isSelected))
                                {
                                    SelectedIndex = i;
                                }
                                if (isSelected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::Text("Current Mod Loader: %s", ModLoader[SelectedIndex].c_str());
                        if (PreSelectedIndex != SelectedIndex)
                        {
                            UpdateModsList = true;
                            UpdateResourcePackList = true;
                            UpdateShaderList = true;
                            UpdateLastUsed = true;
                            PreSelectedIndex = SelectedIndex;
                        }
                        if (ImGui::Button("Rename this profile"))
                        {
                            ShowRenameBox = true;
                        }
                        if (ShowRenameBox)
                        {
                            ImGui::SameLine();
                            if (ImGui::Button("Cancel"))
                            {
                                ShowRenameBox = false;
                            }
                            ImGui::Text("New profile name:");
                            ImGui::SameLine();
                            if (ImGui::InputText("##ProfileRenameBox", buf, IM_ARRAYSIZE(buf), ImGuiInputTextFlags_EnterReturnsTrue) && strcmp(buf, "") != 0)
                            {
                                std::string Origin = profilesDir + "\\" + UsedProfiles[SelectedIndex];
                                std::string Destination = profilesDir + "\\" + buf;
                                fs::rename(Origin, Destination);
                                std::ifstream LauncherProfiles(minecraftDir + "\\launcher_profiles.json");
                                json data = json::parse(LauncherProfiles);
                                if (data["profiles"].contains(UsedProfileIds[SelectedIndex])) {
                                    data["profiles"][buf] = data["profiles"][UsedProfileIds[SelectedIndex]];
                                    data["profiles"][buf]["name"] = buf;
                                    data["profiles"][buf]["gameDir"] = profilesDir + "\\" + buf;
                                    data["profiles"].erase(UsedProfileIds[SelectedIndex]);
                                }
                                std::ofstream o(minecraftDir + "\\launcher_profiles.json");
                                o << std::setw(4) << data << std::endl;
                                UsedProfiles[SelectedIndex] = buf;
                                ProfilesWithVersions[SelectedIndex] = UsedProfiles[SelectedIndex] + " (" + UsedVersions[SelectedIndex] + ")";
                                modsDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\mods";
                                ResourcePackDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\resourcepacks";
                                ShaderDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\shaderpacks";
                                ShowRenameBox = false;
                            }
                        }
                        if (ImGui::Button("Add Modpack") && !g_Status.isDownloading)
                        {
                            if (ModPackFolderPick)
                            {
                                struct ComInit {
                                    ComInit() { CoInitialize(nullptr); }
                                    ~ComInit() { CoUninitialize(); }
                                };
                                ComInit com;
                                CComPtr<IFileOpenDialog> pFolderDlg;
                                HRESULT hr = CoCreateInstance(
                                    CLSID_FileOpenDialog,
                                    NULL,
                                    CLSCTX_ALL,
                                    IID_IFileOpenDialog,
                                    (void**)&pFolderDlg
                                );

                                if (SUCCEEDED(hr)) {
                                    DWORD options;
                                    pFolderDlg->GetOptions(&options);
                                    pFolderDlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                                    hr = pFolderDlg->Show(NULL);

                                    if (SUCCEEDED(hr)) {
                                        CComPtr<IShellItem> pItem;
                                        hr = pFolderDlg->GetResult(&pItem);

                                        if (SUCCEEDED(hr)) {
                                            LPWSTR path;
                                            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);

                                            if (SUCCEEDED(hr)) {
                                                std::thread([=]()
                                                    {
                                                        bit7z::Bit7zLibrary lib{ "7z.dll" };
                                                        std::string ModPackDir = wstring_to_utf8(path);
                                                        if (PathFileExistsA((ModPackDir).c_str()) && !fs::is_empty(ModPackDir))
                                                        {
                                                            if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods").c_str()))
                                                            {
                                                                std::vector<std::wstring> ModpackMods = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\mods", false);
                                                                for (std::wstring CurrentMod : ModpackMods)
                                                                {
                                                                    if (!PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + CurrentMod).c_str()) && !PathFileExistsW((utf8_to_wstring(modsDir) + CurrentMod).c_str()))
                                                                    {
                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods\\" + CurrentMod).c_str(), (utf8_to_wstring(modsDir) + L"\\" + CurrentMod).c_str(), true);
                                                                    }
                                                                }
                                                            }
                                                            if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks").c_str()))
                                                            {
                                                                std::vector<std::wstring> ModpackResourcePacks = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks", false);
                                                                for (std::wstring CurrentResourcePack : ModpackResourcePacks)
                                                                {
                                                                    if (!PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentResourcePack).c_str()) && !PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentResourcePack).c_str()))
                                                                    {
                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks\\" + CurrentResourcePack).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentResourcePack).c_str(), true);
                                                                    }
                                                                }
                                                            }
                                                            if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks").c_str()))
                                                            {
                                                                std::vector<std::wstring> ModpackShaders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks", false);
                                                                for (std::wstring CurrentShader : ModpackShaders)
                                                                {
                                                                    if (!PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + CurrentShader).c_str()) && !PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + CurrentShader).c_str()))
                                                                    {
                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks\\" + CurrentShader).c_str(), (utf8_to_wstring(ShaderDir) + L"\\" + CurrentShader).c_str(), true);
                                                                    }
                                                                }
                                                            }
                                                            if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config").c_str()))
                                                            {
                                                                std::vector<std::wstring> ModpackConfig = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\config", false);
                                                                for (std::wstring CurrentConfig : ModpackConfig)
                                                                {
                                                                    if (!PathFileExistsW((utf8_to_wstring(ConfigDir) + L"\\Disabled Config\\" + CurrentConfig).c_str()) && !PathFileExistsW((utf8_to_wstring(ConfigDir) + L"\\" + CurrentConfig).c_str()))
                                                                    {
                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config\\" + CurrentConfig).c_str(), (utf8_to_wstring(ConfigDir) + L"\\" + CurrentConfig).c_str(), true);
                                                                    }
                                                                }
                                                            }
                                                            std::vector<std::wstring> ModpackFolders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides", true);
                                                            for (std::wstring CurrentFolder2 : ModpackFolders)
                                                            {
                                                                if (!PathFileExistsW((utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2).c_str()))
                                                                {
                                                                    fs::create_directory(utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2);
                                                                }
                                                                std::vector<std::wstring> CurrentFiles = getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2);
                                                                for (std::wstring CurrentFile : CurrentFiles)
                                                                {
                                                                    if (!PathFileExistsW((utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile).c_str()))
                                                                    {
                                                                        try
                                                                        {
                                                                            fs::copy(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2 + L"\\" + CurrentFile, utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile);
                                                                        }
                                                                        catch (...)
                                                                        {
                                                                            //WriteToLog("Copying failure on " + ModPackDir + "\\overrides\\" + CurrentFolder2 + "\\" + CurrentFile + " !");
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                            if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\manifest.json").c_str()))
                                                            {
                                                                if (!PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\Downloads").c_str()))
                                                                {
                                                                    fs::create_directory(ModPackDir + "\\Downloads");
                                                                }
                                                                std::ifstream ModpackMods(ModPackDir + "\\manifest.json");
                                                                json data = json::parse(ModpackMods);
                                                                bool MustRun = false;
                                                                std::set<int> existingFileIDs;
                                                                std::set<int> existingProjectIDs;
                                                                std::ifstream RFileIDs(ModPackDir + "\\Downloads\\FileIDs.txt");
                                                                std::ifstream RProjectIDs(ModPackDir + "\\Downloads\\ProjectIDs.txt");
                                                                std::string line;
                                                                while (std::getline(RFileIDs, line)) {
                                                                    if (!line.empty())
                                                                    {
                                                                        existingFileIDs.insert(std::stoi(line));
                                                                    }
                                                                }
                                                                RFileIDs.close();
                                                                std::string Pline;
                                                                while (std::getline(RProjectIDs, Pline)) {
                                                                    if (!Pline.empty())
                                                                    {
                                                                        existingProjectIDs.insert(std::stoi(Pline));
                                                                    }
                                                                }
                                                                RProjectIDs.close();
                                                                std::ofstream OutProjectIDs(ModPackDir + "\\Downloads\\ProjectIDs.txt", std::ios::app);
                                                                std::ofstream OutFileIDs(ModPackDir + "\\Downloads\\FileIDs.txt", std::ios::app);
                                                                for (auto& file : data["files"])
                                                                {
                                                                    int CurrentProjID = file.value("projectID", 0);
                                                                    int CurrentFileID = file.value("fileID", 0);
                                                                    if (existingFileIDs.count(CurrentFileID) == 0 && existingProjectIDs.count(CurrentProjID) == 0)
                                                                    {
                                                                        CURL* curl = curl_easy_init();
                                                                        if (curl) {
                                                                            std::wstring finalFileName = L"unknown.jar";
                                                                            std::wstring tempPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\downloading.tmp";
                                                                            FILE* fp = _wfopen(tempPath.c_str(), L"wb");
                                                                            if (fp == NULL) {
                                                                                perror("File open failed");
                                                                                return;
                                                                            }
                                                                            if (fp) {
                                                                                curl_easy_setopt(curl, CURLOPT_URL, ("https://www.curseforge.com/api/v1/mods/" + to_string(CurrentProjID) + "/files/" + to_string(CurrentFileID) + "/download").c_str());
                                                                                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                                                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                                                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                                                curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                                                curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
                                                                                curl_easy_setopt(curl, CURLOPT_HEADERDATA, &finalFileName);
                                                                                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                                                curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                                                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                                                CURLcode res = curl_easy_perform(curl);
                                                                                g_Status.isDownloading = true;
                                                                                fclose(fp);
                                                                                if (res != CURLE_OK) {
                                                                                    if (fs::exists(tempPath))
                                                                                    {
                                                                                        DeleteFileW(tempPath.c_str());
                                                                                    }
                                                                                }
                                                                                else
                                                                                {
                                                                                    if (finalFileName == L"unknown.jar")
                                                                                    {
                                                                                        char* url = NULL;
                                                                                        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
                                                                                        if (url) {
                                                                                            std::string finalUrl = url;

                                                                                            size_t queryPos = finalUrl.find('?');
                                                                                            if (queryPos != std::string::npos) finalUrl.resize(queryPos);

                                                                                            size_t lastSlash = finalUrl.find_last_of('/');
                                                                                            if (lastSlash != std::string::npos) {
                                                                                                std::string encodedName = finalUrl.substr(lastSlash + 1);

                                                                                                int outLength;
                                                                                                char* decoded = curl_easy_unescape(curl, encodedName.c_str(), (int)encodedName.length(), &outLength);
                                                                                                if (decoded) {
                                                                                                    finalFileName = utf8_to_wstring(std::string(decoded, outLength));
                                                                                                    g_Status.currentFileName = L"Downloading asset: " + finalFileName;
                                                                                                    curl_free(decoded);
                                                                                                }
                                                                                            }
                                                                                        }
                                                                                    }
                                                                                    std::wstring finalPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + finalFileName;
                                                                                    fs::rename(tempPath, finalPath);
                                                                                    OutProjectIDs << CurrentProjID << "\n";
                                                                                    OutFileIDs << CurrentFileID << "\n";
                                                                                    existingFileIDs.insert(CurrentFileID);
                                                                                    existingProjectIDs.insert(CurrentProjID);
                                                                                }
                                                                            }
                                                                            curl_easy_cleanup(curl);
                                                                        }
                                                                    }
                                                                }
                                                                for (std::wstring& CurrentFile : getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\Downloads", false))
                                                                {
                                                                    std::filesystem::path CurrentPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile;
                                                                    if (CurrentPath.extension() == ".zip" || CurrentPath.extension() == ".jar")
                                                                    {
                                                                        std::vector<std::wstring> ArchiveItems;
                                                                        try {
                                                                            std::wstring FilPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile;
                                                                            bit7z::BitArchiveReader arc{ lib, wstring_to_utf8(FilPath), bit7z::BitFormat::Zip };
                                                                            for (const auto& item : arc)
                                                                            {
                                                                                ArchiveItems.push_back(utf8_to_wstring(item.name()));
                                                                            }
                                                                        }
                                                                        catch (const bit7z::BitException& ex) {
                                                                            WriteToLog(std::string(ex.what()));
                                                                        }
                                                                        for (std::wstring& CurrentItem : ArchiveItems)
                                                                        {
                                                                            if (CurrentItem.find(L"fabric.mod.json") != std::wstring::npos ||
                                                                                CurrentItem.find(L"mods.toml") != std::wstring::npos ||
                                                                                CurrentItem.find(L"mcmod.info") != std::wstring::npos)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(modsDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + CurrentFile).c_str()))
                                                                                {
                                                                                    if (fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".jar")
                                                                                    {
                                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).c_str(), (utf8_to_wstring(modsDir) + L"\\" + CurrentFile).c_str(), true);
                                                                                    }
                                                                                    break;
                                                                                }
                                                                            }
                                                                            else if (CurrentItem.find(L"pack.mcmeta") != std::wstring::npos)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentFile).c_str()))
                                                                                {
                                                                                    if (fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".zip" || fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".txt")
                                                                                    {
                                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentFile).c_str(), true);
                                                                                    }
                                                                                    break;
                                                                                }
                                                                            }
                                                                            else if (CurrentItem.find(LR"(shaders)") != std::wstring::npos)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Shaders\\" + CurrentFile).c_str()))
                                                                                {
                                                                                    if (fs::path(utf8_to_wstring(ShaderDir) + L"\\" + CurrentFile).extension() == ".zip")
                                                                                    {
                                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).c_str(), (utf8_to_wstring(ShaderDir) + L"\\" + CurrentFile).c_str(), true);
                                                                                    }
                                                                                    break;
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                            else if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\modrinth.index.json").c_str()))
                                                            {
                                                                std::ifstream ModpackMods(ModPackDir + "\\modrinth.index.json");
                                                                json data = json::parse(ModpackMods);
                                                                std::string DownloadNJ = "";
                                                                std::wstring Path = L"";
                                                                std::wstring FullPath = L"";
                                                                std::wstring Name = L"";
                                                                for (auto& file : data["files"])
                                                                {
                                                                    if (file.contains("downloads") && file["downloads"].is_array()) {
                                                                        for (auto& item : file["downloads"]) {
                                                                            if (item.is_string()) {
                                                                                DownloadNJ = item.get<std::string>();
                                                                            }
                                                                        }
                                                                    }
                                                                    if (file.contains("path") && file["path"].is_string())
                                                                    {
                                                                        int pos = file.value("path", "").find("/");
                                                                        Path = utf8_to_wstring(file.value("path", "").substr(0, pos));
                                                                        Name = utf8_to_wstring(file.value("path", "").substr(pos + 1, file.value("path", "").size()));
                                                                        FullPath = utf8_to_wstring(file.value("path", ""));
                                                                    }
                                                                    if (DownloadNJ != "" && Path != L"" && !fs::exists(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name))
                                                                    {
                                                                        g_Status.currentFileName = L"Downloading asset: " + Name;
                                                                        CURL* curl = curl_easy_init();
                                                                        if (curl) {
                                                                            curl_easy_setopt(curl, CURLOPT_URL, DownloadNJ.c_str());
                                                                            FILE* fp = _wfopen((utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name).c_str(), L"wb");
                                                                            if (fp == NULL) {
                                                                                perror("File open failed");
                                                                                return;
                                                                            }
                                                                            if (fp) {
                                                                                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                                                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                                                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                                                curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                                                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                                                curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                                                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                                                CURLcode res = curl_easy_perform(curl);
                                                                                g_Status.isDownloading = true;
                                                                                if (res != CURLE_OK) {
                                                                                    if (fs::exists(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name))
                                                                                    {
                                                                                        DeleteFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name).c_str());
                                                                                    }
                                                                                }
                                                                                fclose(fp);
                                                                            }
                                                                            curl_easy_cleanup(curl);
                                                                        }
                                                                    }
                                                                }
                                                                for (std::wstring& CurrentFile : getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides"))
                                                                {
                                                                    std::filesystem::path CurrentPath = utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFile;
                                                                    if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods").c_str()))
                                                                    {
                                                                        std::vector<std::wstring> ModpackMods = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\mods", false);
                                                                        for (std::wstring CurrentMod : ModpackMods)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + CurrentMod).c_str()) && !PathFileExistsW((utf8_to_wstring(modsDir) + CurrentMod).c_str()))
                                                                            {
                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods\\" + CurrentMod).c_str(), (utf8_to_wstring(modsDir) + L"\\" + CurrentMod).c_str(), true);
                                                                            }
                                                                        }
                                                                    }
                                                                    if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks").c_str()))
                                                                    {
                                                                        std::vector<std::wstring> ModpackResourcePacks = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks", false);
                                                                        for (std::wstring CurrentResourcePack : ModpackResourcePacks)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentResourcePack).c_str()) && !PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentResourcePack).c_str()))
                                                                            {
                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks\\" + CurrentResourcePack).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentResourcePack).c_str(), true);
                                                                            }
                                                                        }
                                                                    }
                                                                    if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks").c_str()))
                                                                    {
                                                                        std::vector<std::wstring> ModpackShaders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks", false);
                                                                        for (std::wstring CurrentShader : ModpackShaders)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + CurrentShader).c_str()) && !PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + CurrentShader).c_str()))
                                                                            {
                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks\\" + CurrentShader).c_str(), (utf8_to_wstring(ShaderDir) + L"\\" + CurrentShader).c_str(), true);
                                                                            }
                                                                        }
                                                                    }
                                                                    if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config").c_str()))
                                                                    {
                                                                        std::vector<std::wstring> ModpackConfig = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\config", false);
                                                                        for (std::wstring CurrentConfig : ModpackConfig)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(ConfigDir) + L"\\Disabled Config\\" + CurrentConfig).c_str()) && !PathFileExistsW((utf8_to_wstring(ConfigDir) + L"\\" + CurrentConfig).c_str()))
                                                                            {
                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config\\" + CurrentConfig).c_str(), (utf8_to_wstring(ConfigDir) + L"\\" + CurrentConfig).c_str(), true);
                                                                            }
                                                                        }
                                                                    }
                                                                    std::vector<std::wstring> ModpackFolders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides", true);
                                                                    for (std::wstring CurrentFolder2 : ModpackFolders)
                                                                    {
                                                                        if (!PathFileExistsW((utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2).c_str()))
                                                                        {
                                                                            fs::create_directory(utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2);
                                                                        }
                                                                        std::vector<std::wstring> CurrentFiles = getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2);
                                                                        for (std::wstring CurrentFile : CurrentFiles)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile).c_str()))
                                                                            {
                                                                                try
                                                                                {
                                                                                    fs::copy(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2 + L"\\" + CurrentFile, utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile);
                                                                                }
                                                                                catch (...)
                                                                                {
                                                                                    //WriteToLog("Copying failure on " + ModPackDir + "\\overrides\\" + CurrentFolder2 + "\\" + CurrentFile + " !");
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                        CoTaskMemFree(path);
                                                        g_Status.isDownloading = false;
                                                        g_Status.progress = 0.0f;
                                                        UpdateModsList = true;
                                                        UpdateResourcePackList = true;
                                                        UpdateShaderList = true;
                                                    }).detach();
                                            }
                                        }
                                    }
                                }
                            }
                            else
                            {
                                HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
                                if (SUCCEEDED(hr))
                                {
                                    CComPtr<IFileOpenDialog> pFileDlg;
                                    hr = pFileDlg.CoCreateInstance(CLSID_FileOpenDialog);

                                    if (SUCCEEDED(hr))
                                    {
                                        DWORD dwFlags;
                                        pFileDlg->GetOptions(&dwFlags);
                                        pFileDlg->SetOptions(dwFlags | FOS_FORCEFILESYSTEM);

                                        COMDLG_FILTERSPEC fileTypes[] = { { L"Modpack files", L"*.zip; *.mrpack" } };
                                        pFileDlg->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);

                                        hr = pFileDlg->Show(NULL);
                                        if (SUCCEEDED(hr))
                                        {
                                            CComPtr<IShellItemArray> pResults;
                                            hr = pFileDlg->GetResults(&pResults);

                                            if (SUCCEEDED(hr))
                                            {
                                                DWORD dwCount = 0;
                                                pResults->GetCount(&dwCount);

                                                for (DWORD i = 0; i < dwCount; i++)
                                                {
                                                    CComPtr<IShellItem> pItem;
                                                    pResults->GetItemAt(i, &pItem);

                                                    LPWSTR pszFilePath = NULL;
                                                    pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);

                                                    if (pszFilePath)
                                                    {
                                                        std::thread ([=]()
                                                        {
                                                                std::string fullPath = wstring_to_utf8(pszFilePath);
                                                                std::string filename = fs::path(fullPath).stem().string();
                                                                std::string ModPackDir = fs::path(fullPath).parent_path().string() + "\\" + filename;
                                                                bit7z::Bit7zLibrary lib{ "7z.dll" };
                                                                if (!PathFileExistsA(ModPackDir.c_str()))
                                                                {
                                                                    fs::create_directory(ModPackDir);
                                                                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                                                                    extractor.extract(fullPath, ModPackDir);
                                                                }
                                                                if (PathFileExistsA((ModPackDir).c_str()) && !fs::is_empty(ModPackDir))
                                                                {
                                                                    if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods").c_str()))
                                                                    {
                                                                        std::vector<std::wstring> ModpackMods = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\mods", false);
                                                                        for (std::wstring CurrentMod : ModpackMods)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + CurrentMod).c_str()) && !PathFileExistsW((utf8_to_wstring(modsDir) + CurrentMod).c_str()))
                                                                            {
                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods\\" + CurrentMod).c_str(), (utf8_to_wstring(modsDir) + L"\\" + CurrentMod).c_str(), true);
                                                                            }
                                                                        }
                                                                    }
                                                                    if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks").c_str()))
                                                                    {
                                                                        std::vector<std::wstring> ModpackResourcePacks = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks", false);
                                                                        for (std::wstring CurrentResourcePack : ModpackResourcePacks)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentResourcePack).c_str()) && !PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentResourcePack).c_str()))
                                                                            {
                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks\\" + CurrentResourcePack).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentResourcePack).c_str(), true);
                                                                            }
                                                                        }
                                                                    }
                                                                    if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks").c_str()))
                                                                    {
                                                                        std::vector<std::wstring> ModpackShaders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks", false);
                                                                        for (std::wstring CurrentShader : ModpackShaders)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + CurrentShader).c_str()) && !PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + CurrentShader).c_str()))
                                                                            {
                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks\\" + CurrentShader).c_str(), (utf8_to_wstring(ShaderDir) + L"\\" + CurrentShader).c_str(), true);
                                                                            }
                                                                        }
                                                                    }
                                                                    if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config").c_str()))
                                                                    {
                                                                        std::vector<std::wstring> ModpackConfig = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\config", false);
                                                                        for (std::wstring CurrentConfig : ModpackConfig)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(ConfigDir) + L"\\Disabled Config\\" + CurrentConfig).c_str()) && !PathFileExistsW((utf8_to_wstring(ConfigDir) + L"\\" + CurrentConfig).c_str()))
                                                                            {
                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config\\" + CurrentConfig).c_str(), (utf8_to_wstring(ConfigDir) + L"\\" + CurrentConfig).c_str(), true);
                                                                            }
                                                                        }
                                                                    }
                                                                    std::vector<std::wstring> ModpackFolders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides", true);
                                                                    for (std::wstring CurrentFolder2 : ModpackFolders)
                                                                    {
                                                                        if (!PathFileExistsW((utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2).c_str()))
                                                                        {
                                                                            fs::create_directory(utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2);
                                                                        }
                                                                        std::vector<std::wstring> CurrentFiles = getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2);
                                                                        for (std::wstring CurrentFile : CurrentFiles)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile).c_str()))
                                                                            {
                                                                                try
                                                                                {
                                                                                    fs::copy(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2 + L"\\" + CurrentFile, utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile);
                                                                                }
                                                                                catch (...)
                                                                                {
                                                                                    //WriteToLog("Copying failure on " + ModPackDir + "\\overrides\\" + CurrentFolder2 + "\\" + CurrentFile + " !");
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                    if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\manifest.json").c_str()))
                                                                    {
                                                                        if (!PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\Downloads").c_str()))
                                                                        {
                                                                            fs::create_directory(ModPackDir + "\\Downloads");
                                                                        }
                                                                        std::ifstream ModpackMods(ModPackDir + "\\manifest.json");
                                                                        json data = json::parse(ModpackMods);
                                                                        bool MustRun = false;
                                                                        std::set<int> existingFileIDs;
                                                                        std::set<int> existingProjectIDs;
                                                                        std::ifstream RFileIDs(ModPackDir + "\\Downloads\\FileIDs.txt");
                                                                        std::ifstream RProjectIDs(ModPackDir + "\\Downloads\\ProjectIDs.txt");
                                                                        std::string line;
                                                                        while (std::getline(RFileIDs, line)) {
                                                                            if (!line.empty())
                                                                            {
                                                                                existingFileIDs.insert(std::stoi(line));
                                                                            }
                                                                        }
                                                                        RFileIDs.close();
                                                                        std::string Pline;
                                                                        while (std::getline(RProjectIDs, Pline)) {
                                                                            if (!Pline.empty())
                                                                            {
                                                                                existingProjectIDs.insert(std::stoi(Pline));
                                                                            }
                                                                        }
                                                                        RProjectIDs.close();
                                                                        std::ofstream OutProjectIDs(ModPackDir + "\\Downloads\\ProjectIDs.txt", std::ios::app);
                                                                        std::ofstream OutFileIDs(ModPackDir + "\\Downloads\\FileIDs.txt", std::ios::app);
                                                                        for (auto& file : data["files"])
                                                                        {
                                                                            int CurrentProjID = file.value("projectID", 0);
                                                                            int CurrentFileID = file.value("fileID", 0);
                                                                            if (existingFileIDs.count(CurrentFileID) == 0 && existingProjectIDs.count(CurrentProjID) == 0)
                                                                            {
                                                                                CURL* curl = curl_easy_init();
                                                                                if (curl) {
                                                                                    std::wstring finalFileName = L"unknown.jar";
                                                                                    std::wstring tempPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\downloading.tmp";
                                                                                    FILE* fp = _wfopen(tempPath.c_str(), L"wb");
                                                                                    if (fp == NULL) {
                                                                                        perror("File open failed");
                                                                                        return;
                                                                                    }
                                                                                    if (fp) {
                                                                                        curl_easy_setopt(curl, CURLOPT_URL, ("https://www.curseforge.com/api/v1/mods/" + to_string(CurrentProjID) + "/files/" + to_string(CurrentFileID) + "/download").c_str());
                                                                                        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                                                        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                                                        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                                                        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                                                        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
                                                                                        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &finalFileName);
                                                                                        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                                                        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                                                        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                                                        CURLcode res = curl_easy_perform(curl);
                                                                                        g_Status.isDownloading = true;
                                                                                        fclose(fp);
                                                                                        if (res != CURLE_OK) {
                                                                                            if (fs::exists(tempPath))
                                                                                            {
                                                                                                DeleteFileW(tempPath.c_str());
                                                                                            }
                                                                                        }
                                                                                        else
                                                                                        {
                                                                                            if (finalFileName == L"unknown.jar")
                                                                                            {
                                                                                                char* url = NULL;
                                                                                                curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
                                                                                                if (url) {
                                                                                                    std::string finalUrl = url;

                                                                                                    size_t queryPos = finalUrl.find('?');
                                                                                                    if (queryPos != std::string::npos) finalUrl.resize(queryPos);

                                                                                                    size_t lastSlash = finalUrl.find_last_of('/');
                                                                                                    if (lastSlash != std::string::npos) {
                                                                                                        std::string encodedName = finalUrl.substr(lastSlash + 1);

                                                                                                        int outLength;
                                                                                                        char* decoded = curl_easy_unescape(curl, encodedName.c_str(), (int)encodedName.length(), &outLength);
                                                                                                        if (decoded) {
                                                                                                            finalFileName = utf8_to_wstring(std::string(decoded, outLength));
                                                                                                            g_Status.currentFileName = L"Downloading asset: " + finalFileName;
                                                                                                            curl_free(decoded);
                                                                                                        }
                                                                                                    }
                                                                                                }
                                                                                            }
                                                                                            std::wstring finalPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + finalFileName;
                                                                                            fs::rename(tempPath, finalPath);
                                                                                            OutProjectIDs << CurrentProjID << "\n";
                                                                                            OutFileIDs << CurrentFileID << "\n";
                                                                                            existingFileIDs.insert(CurrentFileID);
                                                                                            existingProjectIDs.insert(CurrentProjID);
                                                                                        }
                                                                                    }
                                                                                    curl_easy_cleanup(curl);
                                                                                }
                                                                            }
                                                                        }
                                                                        for (std::wstring& CurrentFile : getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\Downloads", false))
                                                                        {
                                                                            std::filesystem::path CurrentPath = utf8_to_wstring(ModPackDir) + L"\\Downloads" + CurrentFile;
                                                                            if (CurrentPath.extension() == ".zip" || CurrentPath.extension() == ".jar")
                                                                            {
                                                                                std::vector<std::wstring> ArchiveItems;
                                                                                try {
                                                                                    std::wstring FilPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile;
                                                                                    bit7z::BitArchiveReader arc{ lib, wstring_to_utf8(FilPath), bit7z::BitFormat::Zip };
                                                                                    for (const auto& item : arc)
                                                                                    {
                                                                                        ArchiveItems.push_back(utf8_to_wstring(item.name()));
                                                                                    }
                                                                                }
                                                                                catch (const bit7z::BitException& ex) {
                                                                                    WriteToLog(std::string(ex.what()));
                                                                                }
                                                                                for (std::wstring& CurrentItem : ArchiveItems)
                                                                                {
                                                                                    if (CurrentItem.find(L"fabric.mod.json") != std::wstring::npos ||
                                                                                        CurrentItem.find(L"mods.toml") != std::wstring::npos ||
                                                                                        CurrentItem.find(L"mcmod.info") != std::wstring::npos)
                                                                                    {
                                                                                        if (!PathFileExistsW((utf8_to_wstring(modsDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + CurrentFile).c_str()))
                                                                                        {
                                                                                            if (fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".jar")
                                                                                            {
                                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).c_str(), (utf8_to_wstring(modsDir) + L"\\" + CurrentFile).c_str(), true);
                                                                                            }
                                                                                            break;
                                                                                        }
                                                                                    }
                                                                                    else if (CurrentItem.find(L"pack.mcmeta") != std::wstring::npos)
                                                                                    {
                                                                                        if (!PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentFile).c_str()))
                                                                                        {
                                                                                            if (fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".zip" || fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".txt")
                                                                                            {
                                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentFile).c_str(), true);
                                                                                            }
                                                                                            break;
                                                                                        }
                                                                                    }
                                                                                    else if (CurrentItem.find(L"shaders") != std::wstring::npos)
                                                                                    {
                                                                                        if (!PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Shaders\\" + CurrentFile).c_str()))
                                                                                        {
                                                                                            if (fs::path(utf8_to_wstring(ShaderDir) + L"\\" + CurrentFile).extension() == ".zip")
                                                                                            {
                                                                                                CopyFileW((utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).c_str(), (utf8_to_wstring(ShaderDir) + L"\\" + CurrentFile).c_str(), true);
                                                                                            }
                                                                                            break;
                                                                                        }
                                                                                    }
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                    else if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\modrinth.index.json").c_str()))
                                                                    {
                                                                        std::ifstream ModpackMods(ModPackDir + "\\modrinth.index.json");
                                                                        json data = json::parse(ModpackMods);
                                                                        std::string DownloadNJ = "";
                                                                        std::wstring Path = L"";
                                                                        std::wstring FullPath = L"";
                                                                        std::wstring Name = L"";
                                                                        for (auto& file : data["files"])
                                                                        {
                                                                            if (file.contains("downloads") && file["downloads"].is_array()) {
                                                                                for (auto& item : file["downloads"]) {
                                                                                    if (item.is_string()) {
                                                                                        DownloadNJ = item.get<std::string>();
                                                                                    }
                                                                                }
                                                                            }
                                                                            if (file.contains("path") && file["path"].is_string())
                                                                            {
                                                                                int pos = file.value("path", "").find("/");
                                                                                Path = utf8_to_wstring(file.value("path", "").substr(0, pos));
                                                                                Name = utf8_to_wstring(file.value("path", "").substr(pos + 1, file.value("path", "").size()));
                                                                                FullPath = utf8_to_wstring(file.value("path", ""));
                                                                            }
                                                                            if (DownloadNJ != "" && Path != L"" && !fs::exists(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name))
                                                                            {
                                                                                g_Status.currentFileName = L"Downloading asset: " + Name;
                                                                                CURL* curl = curl_easy_init();
                                                                                if (curl) {
                                                                                    curl_easy_setopt(curl, CURLOPT_URL, DownloadNJ.c_str());
                                                                                    FILE* fp = _wfopen((utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name).c_str(), L"wb");
                                                                                    if (fp == NULL) {
                                                                                        perror("File open failed");
                                                                                        return;
                                                                                    }
                                                                                    if (fp) {
                                                                                        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                                                        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                                                        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                                                        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                                                        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                                                        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                                                        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                                                        CURLcode res = curl_easy_perform(curl);
                                                                                        g_Status.isDownloading = true;
                                                                                        if (res != CURLE_OK) {
                                                                                            if (fs::exists(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name))
                                                                                            {
                                                                                                DeleteFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name).c_str());
                                                                                            }
                                                                                        }
                                                                                        fclose(fp);
                                                                                    }
                                                                                    curl_easy_cleanup(curl);
                                                                                }
                                                                            }
                                                                        }
                                                                        for (std::wstring& CurrentFile : getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides"))
                                                                        {
                                                                            std::filesystem::path CurrentPath = utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFile;
                                                                            if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods").c_str()))
                                                                            {
                                                                                std::vector<std::wstring> ModpackMods = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\mods", false);
                                                                                for (std::wstring CurrentMod : ModpackMods)
                                                                                {
                                                                                    if (!PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + CurrentMod).c_str()) && !PathFileExistsW((utf8_to_wstring(modsDir) + CurrentMod).c_str()))
                                                                                    {
                                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods\\" + CurrentMod).c_str(), (utf8_to_wstring(modsDir) + L"\\" + CurrentMod).c_str(), true);
                                                                                    }
                                                                                }
                                                                            }
                                                                            if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks").c_str()))
                                                                            {
                                                                                std::vector<std::wstring> ModpackResourcePacks = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks", false);
                                                                                for (std::wstring CurrentResourcePack : ModpackResourcePacks)
                                                                                {
                                                                                    if (!PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentResourcePack).c_str()) && !PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentResourcePack).c_str()))
                                                                                    {
                                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks\\" + CurrentResourcePack).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentResourcePack).c_str(), true);
                                                                                    }
                                                                                }
                                                                            }
                                                                            if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks").c_str()))
                                                                            {
                                                                                std::vector<std::wstring> ModpackShaders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks", false);
                                                                                for (std::wstring CurrentShader : ModpackShaders)
                                                                                {
                                                                                    if (!PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + CurrentShader).c_str()) && !PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + CurrentShader).c_str()))
                                                                                    {
                                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\shaderpacks\\" + CurrentShader).c_str(), (utf8_to_wstring(ShaderDir) + L"\\" + CurrentShader).c_str(), true);
                                                                                    }
                                                                                }
                                                                            }
                                                                            if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config").c_str()))
                                                                            {
                                                                                std::vector<std::wstring> ModpackConfig = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\config", false);
                                                                                for (std::wstring CurrentConfig : ModpackConfig)
                                                                                {
                                                                                    if (!PathFileExistsW((utf8_to_wstring(ConfigDir) + L"\\Disabled Config\\" + CurrentConfig).c_str()) && !PathFileExistsW((utf8_to_wstring(ConfigDir) + L"\\" + CurrentConfig).c_str()))
                                                                                    {
                                                                                        CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config\\" + CurrentConfig).c_str(), (utf8_to_wstring(ConfigDir) + L"\\" + CurrentConfig).c_str(), true);
                                                                                    }
                                                                                }
                                                                            }
                                                                            std::vector<std::wstring> ModpackFolders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides", true);
                                                                            for (std::wstring CurrentFolder2 : ModpackFolders)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2).c_str()))
                                                                                {
                                                                                    fs::create_directory(utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2);
                                                                                }
                                                                                std::vector<std::wstring> CurrentFiles = getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2);
                                                                                for (std::wstring CurrentFile : CurrentFiles)
                                                                                {
                                                                                    if (!PathFileExistsW((utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile).c_str()))
                                                                                    {
                                                                                        try
                                                                                        {
                                                                                            fs::copy(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2 + L"\\" + CurrentFile, utf8_to_wstring(profilesDir) + L"\\" + utf8_to_wstring(UsedProfiles[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile);
                                                                                        }
                                                                                        catch (...)
                                                                                        {
                                                                                            //WriteToLog("Copying failure on " + ModPackDir + "\\overrides\\" + CurrentFolder2 + "\\" + CurrentFile + " !");
                                                                                        }
                                                                                    }
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                                CoTaskMemFree(pszFilePath);
                                                                g_Status.isDownloading = false;
                                                                g_Status.progress = 0.0f;
                                                                UpdateModsList = true;
                                                                UpdateResourcePackList = true;
                                                                UpdateShaderList = true;
                                                        }).detach();
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        ImGui::SameLine();
                        ImGui::Checkbox("Select per folder##Modpack", &ModPackFolderPick);
                        if (g_Status.isDownloading) {
                            ImGui::Text("%s", wstring_to_utf8(g_Status.currentFileName).c_str());
                            ImGui::ProgressBar(g_Status.progress);
                        }
                        if (ImGui::CollapsingHeader("Mods"))
                        {
                            if (ImGui::Button("Add Mods"))
                            {
                                if (!FolderPick)
                                {
                                    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
                                    if (SUCCEEDED(hr))
                                    {
                                        CComPtr<IFileOpenDialog> pFileDlg;
                                        hr = pFileDlg.CoCreateInstance(CLSID_FileOpenDialog);

                                        if (SUCCEEDED(hr))
                                        {
                                            DWORD dwFlags;
                                            pFileDlg->GetOptions(&dwFlags);
                                            pFileDlg->SetOptions(dwFlags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);

                                            COMDLG_FILTERSPEC fileTypes[] = { { L"Java Mod Files", L"*.jar" } };
                                            pFileDlg->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);

                                            hr = pFileDlg->Show(NULL);
                                            if (SUCCEEDED(hr))
                                            {
                                                CComPtr<IShellItemArray> pResults;
                                                hr = pFileDlg->GetResults(&pResults);

                                                if (SUCCEEDED(hr))
                                                {
                                                    DWORD dwCount = 0;
                                                    pResults->GetCount(&dwCount);

                                                    for (DWORD i = 0; i < dwCount; i++)
                                                    {
                                                        CComPtr<IShellItem> pItem;
                                                        pResults->GetItemAt(i, &pItem);

                                                        LPWSTR pszFilePath = NULL;
                                                        pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);

                                                        if (pszFilePath)
                                                        {
                                                            std::wstring fullPath = pszFilePath;
                                                            std::wstring filename = fs::path(fullPath).filename().wstring();
                                                            std::wstring destination = utf8_to_wstring(modsDir + "\\") + filename;
                                                            std::wstring DisabledDest = utf8_to_wstring(modsDir + "\\Disabled Mods\\") + filename;
                                                            if (!PathFileExistsW(destination.c_str()) && !PathFileExistsW(DisabledDest.c_str()))
                                                            {
                                                                CopyFileW(fullPath.c_str(), destination.c_str(), FALSE);
                                                            }
                                                            CoTaskMemFree(pszFilePath);
                                                        }
                                                    }
                                                    UpdateModsList = true;
                                                }
                                            }
                                        }
                                        CoUninitialize();
                                    }
                                }
                                else
                                {
                                    struct ComInit {
                                        ComInit() { CoInitialize(nullptr); }
                                        ~ComInit() { CoUninitialize(); }
                                    };
                                    ComInit com;
                                    CComPtr<IFileOpenDialog> pFolderDlg;
                                    HRESULT hr = CoCreateInstance(
                                        CLSID_FileOpenDialog,
                                        NULL,
                                        CLSCTX_ALL,
                                        IID_IFileOpenDialog,
                                        (void**)&pFolderDlg
                                    );

                                    if (SUCCEEDED(hr)) {
                                        DWORD options;
                                        pFolderDlg->GetOptions(&options);
                                        pFolderDlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                                        hr = pFolderDlg->Show(NULL);

                                        if (SUCCEEDED(hr)) {
                                            CComPtr<IShellItem> pItem;
                                            hr = pFolderDlg->GetResult(&pItem);

                                            if (SUCCEEDED(hr)) {
                                                LPWSTR path;
                                                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);

                                                if (SUCCEEDED(hr)) {
                                                    std::wstring PickedFolder = path;
                                                    std::vector<std::wstring> FilesToCopy = getInDirectoryW(PickedFolder, false);
                                                    for (std::wstring CurrentFile : FilesToCopy)
                                                    {
                                                        if (!PathFileExistsW((utf8_to_wstring(modsDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW(((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + CurrentFile).c_str())))
                                                        {
                                                            CopyFileW(
                                                                (PickedFolder + L"\\" + CurrentFile).c_str(),
                                                                (utf8_to_wstring(modsDir) + L"\\" + CurrentFile).c_str(), true);
                                                        }
                                                    }
                                                    UpdateModsList = true;
                                                    CoTaskMemFree(path);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            ImGui::SameLine();
                            ImGui::Checkbox("Select per folder", &FolderPick);
                            if (ImGui::Button("Open Modrinth Page"))
                            {
                                std::string url = "https://modrinth.com/discover/mods?g=categories:" + ModLoader[SelectedIndex] + "&v=" + UsedVersions[SelectedIndex];
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Open CurseForge Page"))
                            {
                                int gameVersionTypeId = 0;
                                if (ModLoader[SelectedIndex] == "fabric")
                                {
                                    gameVersionTypeId = 4;
                                }
                                else if (ModLoader[SelectedIndex] == "forge")
                                {
                                    gameVersionTypeId = 1;
                                }
                                else if (ModLoader[SelectedIndex] == "neoforge")
                                {
                                    gameVersionTypeId = 6;
                                }
                                else
                                {
                                    gameVersionTypeId = 5;
                                }
                                std::string baselink = "https://www.curseforge.com/minecraft/search?page=1&pageSize=20&sortBy=relevancy&class=mc-mods";
                                std::string url = baselink + "&version=" + UsedVersions[SelectedIndex] + "&gameVersionTypeId=" + to_string(gameVersionTypeId);
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            
                            if (!ModListAndEnabled.empty())
                            {
                                if (ImGui::Button("Remove all##Mods"))
                                {
                                    for (int i = 0; i < ModListAndEnabled.size(); i++)
                                    {
                                        if (ModListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(modsDir) + L"\\" + ModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(modsDir) + L"\\" + ModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                    }
                                    ModDisplayListAndEnabled.clear();
                                    ModList.clear();
                                    ModListAndEnabled.clear();
                                    DisabledModList.clear();
                                    UpdateLastUsed = true;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Disable all##Mods"))
                                {
                                    for (int i = 0; i < ModListAndEnabled.size(); i++)
                                    {
                                        ModListAndEnabled[i].second = false;
                                        if (PathFileExistsW((utf8_to_wstring(modsDir) + L"\\" + ModListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileW((utf8_to_wstring(modsDir) + L"\\" + ModListAndEnabled[i].first).c_str(), (utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str());
                                        }
                                    } 
                                    UpdateLastUsed = true;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Enable all##Mods"))
                                {
                                    for (int i = 0; i < ModListAndEnabled.size(); i++)
                                    {
                                        ModListAndEnabled[i].second = true;
                                        if (PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str(), (utf8_to_wstring(modsDir) + L"\\" + ModListAndEnabled[i].first).c_str());
                                        }
                                    }
                                    UpdateLastUsed = true;
                                }
                                ImGui::Text("Mod list:");
                                ImGui::Separator();
                                for (int i = 0; i < ModListAndEnabled.size(); i++)
                                {
                                    ImGui::TextWrapped("%s", wstring_to_utf8(ModDisplayListAndEnabled[i].first).c_str());
                                    ImGui::SameLine();
                                    if (ImGui::Checkbox(("Enabled##" + to_string(i)).c_str(), reinterpret_cast<bool*>(&ModListAndEnabled[i].second)))
                                    {
                                            if (!ModListAndEnabled[i].second)
                                            {
                                                if (PathFileExistsW((utf8_to_wstring(modsDir) + L"\\" + ModListAndEnabled[i].first).c_str()))
                                                {
                                                    MoveFileW((utf8_to_wstring(modsDir) + L"\\" + ModListAndEnabled[i].first).c_str(), (utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str());
                                                }
                                            }
                                            else
                                            {
                                                if (PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str()))
                                                {
                                                    MoveFileW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str(), (utf8_to_wstring(modsDir) + L"\\" + ModListAndEnabled[i].first).c_str());
                                                }
                                            }
                                        UpdateLastUsed = true;
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button(("Remove##" + to_string(i)).c_str()))
                                    {
                                        UpdateLastUsed = true;
                                        if (ModListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(modsDir) + L"\\" + ModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(modsDir) + L"\\" + ModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        ModListAndEnabled.erase(ModListAndEnabled.begin() + i);
                                        ModDisplayListAndEnabled.erase(ModDisplayListAndEnabled.begin() + i);
                                        if (i != 0)
                                        {
                                            i--;
                                        }
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button(("Details##Mods" + to_string(i)).c_str()))
                                    {
                                        ImGui::OpenPopup(("DetailsPopup##Mods" + to_string(i)).c_str());
                                    }
                                    if (ImGui::BeginPopup(("DetailsPopup##Mods" + to_string(i)).c_str()))
                                    {
                                        ImGui::Text("Description: %s", wstring_to_utf8(ModDescriptions[i]).c_str());
                                        ImGui::Text("Version: %s", wstring_to_utf8(ModVersions[i]).c_str());
                                        ImGui::Text("Author(s): %s", wstring_to_utf8(ModAuthors[i]).c_str());
                                        ImGui::EndPopup();
                                    }
                                    ImGui::Separator();
                                }
                            }
                        }
                        if (UpdateModsList)
                        {
                            ModList.clear();
                            ModListAndEnabled.clear();
                            ModDisplayList.clear();
                            ModDisplayListAndEnabled.clear();
                            DisabledModList.clear();
                            modsDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\mods";
                            bit7z::Bit7zLibrary lib{ "7z.dll" };
                            std::vector<bit7z::byte_t> ModBuffer;
                            ModList = getInDirectoryW(utf8_to_wstring(modsDir), false);
                            for (int i = 0; i < ModList.size(); i++)
                            {
                                if (fs::path(ModList[i]).extension() == ".jar")
                                {
                                    std::wstring Name = L"";
                                    std::wstring Description = L"";
                                    std::wstring Version = L"";
                                    std::vector<std::string> AuthorStr;
                                    std::string AuthorNJ = "";
                                    json Author = json::array();
                                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                                    extractor.extractMatching(modsDir + "\\" + wstring_to_utf8(ModList[i]), "fabric.mod.json", ModBuffer);
                                    if (!ModBuffer.empty())
                                    {
                                        try
                                        {
                                            json mod = json::parse(ModBuffer);
                                            Name = utf8_to_wstring(mod.value("name", ""));
                                            Description = utf8_to_wstring(mod.value("description", ""));
                                            Version = utf8_to_wstring(mod.value("version", ""));
                                            if (mod.contains("authors") && mod["authors"].is_array()) {
                                                for (auto& item : mod["authors"]) {
                                                    if (item.is_string()) {
                                                        AuthorStr.push_back(item.get<std::string>());
                                                    }
                                                    else if (item.is_object() && item.contains("name")) {
                                                        AuthorStr.push_back(item["name"].get<std::string>());
                                                    }
                                                }
                                            }
                                            for (size_t i = 0; i < AuthorStr.size(); ++i) {
                                                AuthorNJ += AuthorStr[i];
                                                if (i < AuthorStr.size() - 1) {
                                                    AuthorNJ += ", ";
                                                }
                                            }
                                        }
                                        catch (json::exception& except)
                                        {
                                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ModList[i]));
                                        }
                                    }
                                    if (Name != L"")
                                    {
                                        ModDisplayListAndEnabled.push_back(make_pair(Name, true));
                                    }
                                    else
                                    {
                                        ModDisplayListAndEnabled.push_back(make_pair(ModList[i], true));
                                    }
                                    ModAuthors.push_back(utf8_to_wstring(AuthorNJ));
                                    ModDescriptions.push_back(Description);
                                    ModVersions.push_back(Version);
                                    ModListAndEnabled.push_back(make_pair(ModList[i], true));
                                }
                            }
                            DisabledModList = getInDirectoryW(utf8_to_wstring(modsDir) + L"\\Disabled Mods\\", false);
                            for (int i = 0; i < DisabledModList.size(); i++)
                            {
                                std::wstring Name = L"";
                                std::wstring Description = L"";
                                std::wstring Version = L"";
                                std::vector<std::string> AuthorStr;
                                std::string AuthorNJ = "";
                                json Author = json::array();
                                bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                                extractor.extractMatching(modsDir + "\\Disabled Mods\\" + wstring_to_utf8(DisabledModList[i]), "fabric.mod.json", ModBuffer);
                                if (!ModBuffer.empty())
                                {
                                    try
                                    {
                                        json mod = json::parse(ModBuffer);
                                        Name = utf8_to_wstring(mod.value("name", ""));
                                        Description = utf8_to_wstring(mod.value("description", ""));
                                        Version = utf8_to_wstring(mod.value("version", ""));
                                        if (mod.contains("authors") && mod["authors"].is_array()) {
                                            for (auto& item : mod["authors"]) {
                                                if (item.is_string()) {
                                                    AuthorStr.push_back(item.get<std::string>());
                                                }
                                                else if (item.is_object() && item.contains("name")) {
                                                    AuthorStr.push_back(item["name"].get<std::string>());
                                                }
                                            }
                                        }
                                        for (size_t i = 0; i < AuthorStr.size(); ++i) {
                                            AuthorNJ += AuthorStr[i];
                                            if (i < AuthorStr.size() - 1) {
                                                AuthorNJ += ", ";
                                            }
                                        }
                                    }
                                    catch (json::exception& except)
                                    {
                                        WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(DisabledModList[i]));
                                    }
                                }
                                if (Name != L"")
                                {
                                    ModDisplayListAndEnabled.push_back(make_pair(Name, false));
                                }
                                else
                                {
                                    ModDisplayListAndEnabled.push_back(make_pair(DisabledModList[i], false));
                                }
                                ModAuthors.push_back(utf8_to_wstring(AuthorNJ));
                                ModDescriptions.push_back(Description);
                                ModVersions.push_back(Version);
                                ModListAndEnabled.push_back(make_pair(DisabledModList[i], false));
                            }
                            std::sort(ModDisplayListAndEnabled.begin(), ModDisplayListAndEnabled.end());
                            UpdateModsList = false;
                        }
                        if (ImGui::CollapsingHeader("Resource Packs"))
                        {
                            if (ImGui::Button("Add Resource Pack"))
                            {
                                if (!ResourcePackFolderPick)
                                {
                                    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
                                    if (SUCCEEDED(hr))
                                    {
                                        CComPtr<IFileOpenDialog> pFileDlg;
                                        hr = pFileDlg.CoCreateInstance(CLSID_FileOpenDialog);

                                        if (SUCCEEDED(hr))
                                        {
                                            DWORD dwFlags;
                                            pFileDlg->GetOptions(&dwFlags);
                                            pFileDlg->SetOptions(dwFlags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);

                                            COMDLG_FILTERSPEC fileTypes[] = { { L"Resource Pack Files", L"*.zip" } };
                                            pFileDlg->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);

                                            hr = pFileDlg->Show(NULL);
                                            if (SUCCEEDED(hr))
                                            {
                                                CComPtr<IShellItemArray> pResults;
                                                hr = pFileDlg->GetResults(&pResults);

                                                if (SUCCEEDED(hr))
                                                {
                                                    DWORD dwCount = 0;
                                                    pResults->GetCount(&dwCount);

                                                    for (DWORD i = 0; i < dwCount; i++)
                                                    {
                                                        CComPtr<IShellItem> pItem;
                                                        pResults->GetItemAt(i, &pItem);

                                                        LPWSTR pszFilePath = NULL;
                                                        pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);

                                                        if (pszFilePath)
                                                        {
                                                            std::wstring fullPath = pszFilePath;
                                                            std::wstring filename = fs::path(fullPath).filename().wstring();
                                                            std::wstring destination = utf8_to_wstring(ResourcePackDir + "\\") + filename;
                                                            std::wstring DisabledDest = utf8_to_wstring(ResourcePackDir + "\\Disabled Resource Packs\\") + filename;
                                                            if (!PathFileExistsW(destination.c_str()) && !PathFileExistsW(DisabledDest.c_str()))
                                                            {
                                                                CopyFileW(fullPath.c_str(), destination.c_str(), FALSE);
                                                            }
                                                            CoTaskMemFree(pszFilePath);
                                                        }
                                                    }
                                                    UpdateResourcePackList = true;
                                                }
                                            }
                                        }
                                        CoUninitialize();
                                    }
                                }
                                else
                                {
                                    struct ComInit {
                                        ComInit() { CoInitialize(nullptr); }
                                        ~ComInit() { CoUninitialize(); }
                                    };
                                    ComInit com;
                                    CComPtr<IFileOpenDialog> pFolderDlg;
                                    HRESULT hr = CoCreateInstance(
                                        CLSID_FileOpenDialog,
                                        NULL,
                                        CLSCTX_ALL,
                                        IID_IFileOpenDialog,
                                        (void**)&pFolderDlg
                                    );

                                    if (SUCCEEDED(hr)) {
                                        DWORD options;
                                        pFolderDlg->GetOptions(&options);
                                        pFolderDlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                                        hr = pFolderDlg->Show(NULL);

                                        if (SUCCEEDED(hr)) {
                                            CComPtr<IShellItem> pItem;
                                            hr = pFolderDlg->GetResult(&pItem);

                                            if (SUCCEEDED(hr)) {
                                                LPWSTR path;
                                                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);

                                                if (SUCCEEDED(hr)) {
                                                    std::wstring PickedFolder = path;
                                                    std::vector<std::wstring> FilesToCopy = getInDirectoryW(PickedFolder, false);
                                                    for (std::wstring CurrentFile : FilesToCopy)
                                                    {
                                                        if (!PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentFile).c_str()))
                                                        {
                                                            CopyFileW(
                                                                (PickedFolder + L"\\" + CurrentFile).c_str(),
                                                                (utf8_to_wstring(ResourcePackDir) + L"\\" + CurrentFile).c_str(), true);
                                                        }
                                                    }
                                                    UpdateResourcePackList = true;
                                                    CoTaskMemFree(path);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            ImGui::SameLine();
                            ImGui::Checkbox("Select per folder##ResourcePack", &ResourcePackFolderPick);
                            if (ImGui::Button("Open Modrinth Page##ResourcePack"))
                            {
                                std::string url = "https://modrinth.com/discover/resourcepacks?v=" + UsedVersions[SelectedIndex];
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Open CurseForge Page##ResourcePack"))
                            {
                                std::string url = "https://www.curseforge.com/minecraft/search?page=1&pageSize=20&sortBy=relevancy&class=texture-packs&version=" + UsedVersions[SelectedIndex];
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            if (!ResourcePackListAndEnabled.empty())
                            {
                                if (ImGui::Button("Remove all##Resource Packs"))
                                {
                                    for (int i = 0; i < ResourcePackListAndEnabled.size(); i++)
                                    {
                                        if (ResourcePackListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + ResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ResourcePackDir) + L"\\" + ResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                    }
                                    ResourcePackList.clear();
                                    ResourcePackDisplayListAndEnabled.clear();
                                    ResourcePackListAndEnabled.clear();
                                    DisabledResourcePackList.clear();
                                    UpdateLastUsed = true;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Disable all##Resource Packs"))
                                {
                                    for (int i = 0; i < ResourcePackListAndEnabled.size(); i++)
                                    {
                                        ResourcePackListAndEnabled[i].second = false;
                                        if (PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + ResourcePackListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileW((utf8_to_wstring(ResourcePackDir) + L"\\" + ResourcePackListAndEnabled[i].first).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str());
                                        }
                                    }
                                    UpdateLastUsed = true;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Enable all##Resource Packs"))
                                {
                                    for (int i = 0; i < ResourcePackListAndEnabled.size(); i++)
                                    {
                                        ResourcePackListAndEnabled[i].second = true;
                                        if (PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\" + ResourcePackListAndEnabled[i].first).c_str());
                                        }
                                    }
                                    UpdateLastUsed = true;
                                }
                                ImGui::Text("Resource Pack List:");
                                ImGui::Separator();
                                for (int i = 0; i < ResourcePackListAndEnabled.size(); i++)
                                {
                                    ImGui::TextWrapped("%s", wstring_to_utf8(ResourcePackDisplayListAndEnabled[i].first).c_str());
                                    ImGui::SameLine();
                                    if (ImGui::Checkbox(("Enabled##ResourcePack" + to_string(i)).c_str(), reinterpret_cast<bool*>(&ResourcePackListAndEnabled[i].second)))
                                    {
                                        if (!ResourcePackListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + ResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                MoveFileW((utf8_to_wstring(ResourcePackDir) + L"\\" + ResourcePackListAndEnabled[i].first).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                MoveFileW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\" + ResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        UpdateLastUsed = true;
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button(("Remove##ResourcePack" + to_string(i)).c_str()))
                                    {
                                        UpdateLastUsed = true;
                                        if (ResourcePackListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\" + ResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ResourcePackDir) + L"\\" + ResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        ResourcePackListAndEnabled.erase(ResourcePackListAndEnabled.begin() + i);
                                        ResourcePackDisplayListAndEnabled.erase(ResourcePackDisplayListAndEnabled.begin() + i);
                                        if (i != 0)
                                        {
                                            i--;
                                        }
                                    }
                                    ImGui::Separator();
                                }
                            }
                        }
                        if (UpdateResourcePackList)
                        {
                            ResourcePackList.clear();
                            ResourcePackDisplayListAndEnabled.clear();
                            ResourcePackListAndEnabled.clear();
                            DisabledResourcePackList.clear();
                            ResourcePackDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\resourcepacks";
                            ResourcePackList = getInDirectoryW(utf8_to_wstring(ResourcePackDir), false);
                            bit7z::Bit7zLibrary lib{ "7z.dll" };
                            ResourcePackList = getInDirectoryW(utf8_to_wstring(ResourcePackDir), false);
                            std::vector<bit7z::byte_t> ResourcePackBuffer;
                            for (int i = 0; i < ResourcePackList.size(); i++)
                            {
                                std::wstring Name = L"";
                                if (fs::path(ResourcePackList[i]).extension() == ".zip")
                                {
                                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                                    try
                                    {
                                        extractor.extractMatching(ResourcePackDir + "\\" + wstring_to_utf8(ResourcePackList[i]), "pack.mcmeta", ResourcePackBuffer);
                                    }
                                    catch (const bit7z::BitException& except)
                                    {
                                        WriteToLog(except.what());
                                    }
                                    if (!ResourcePackBuffer.empty())
                                    {
                                        try
                                        {
                                            json resourcepack = json::parse(ResourcePackBuffer);
                                            Name = utf8_to_wstring(resourcepack["pack"].value("name", ""));
                                        }
                                        catch (json::exception& except)
                                        {
                                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ResourcePackList[i]));
                                        }
                                    }
                                    if (Name != L"")
                                    {
                                        ResourcePackDisplayListAndEnabled.push_back(make_pair(Name, true));
                                    }
                                    else
                                    {
                                        ResourcePackDisplayListAndEnabled.push_back(make_pair(ResourcePackList[i], true));
                                    }
                                    ResourcePackListAndEnabled.push_back(make_pair(ResourcePackList[i], true));
                                }
                            }
                            DisabledResourcePackList = getInDirectoryW(utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\", false);
                            for (int i = 0; i < DisabledResourcePackList.size(); i++)
                            {
                                std::wstring Name = L"";
                                if (fs::path(DisabledResourcePackList[i]).extension() == ".zip")
                                {
                                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                                    try
                                    {
                                        extractor.extractMatching(ResourcePackDir + "\\Disabled Resource Packs\\" + wstring_to_utf8(DisabledResourcePackList[i]), "pack.mcmeta", ResourcePackBuffer);
                                    }
                                    catch (bit7z::BitException& except)
                                    {
                                        WriteToLog(except.what());
                                    }
                                    if (!ResourcePackBuffer.empty())
                                    {
                                        try
                                        {
                                            json resourcepack = json::parse(ResourcePackBuffer);
                                            Name = utf8_to_wstring(resourcepack["pack"].value("name", ""));
                                        }
                                        catch (json::exception& except)
                                        {
                                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ResourcePackList[i]));
                                        }
                                    }
                                    if (Name != L"")
                                    {
                                        ResourcePackDisplayListAndEnabled.push_back(make_pair(Name, false));
                                    }
                                    else
                                    {
                                        ResourcePackDisplayListAndEnabled.push_back(make_pair(DisabledResourcePackList[i], false));
                                    }
                                    ResourcePackListAndEnabled.push_back(make_pair(DisabledResourcePackList[i], false));
                                }
                            }
                            std::sort(ResourcePackDisplayListAndEnabled.begin(), ResourcePackDisplayListAndEnabled.end());
                            UpdateResourcePackList = false;
                        }
                        if (ImGui::CollapsingHeader("Shaders"))
                        {
                            if (ImGui::Button("Add Shader"))
                            {
                                if (!ShaderFolderPick)
                                {
                                    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
                                    if (SUCCEEDED(hr))
                                    {
                                        CComPtr<IFileOpenDialog> pFileDlg;
                                        hr = pFileDlg.CoCreateInstance(CLSID_FileOpenDialog);

                                        if (SUCCEEDED(hr))
                                        {
                                            DWORD dwFlags;
                                            pFileDlg->GetOptions(&dwFlags);
                                            pFileDlg->SetOptions(dwFlags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);

                                            COMDLG_FILTERSPEC fileTypes[] = { { L"Shader Files", L"*.zip" } };
                                            pFileDlg->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);

                                            hr = pFileDlg->Show(NULL);
                                            if (SUCCEEDED(hr))
                                            {
                                                CComPtr<IShellItemArray> pResults;
                                                hr = pFileDlg->GetResults(&pResults);

                                                if (SUCCEEDED(hr))
                                                {
                                                    DWORD dwCount = 0;
                                                    pResults->GetCount(&dwCount);

                                                    for (DWORD i = 0; i < dwCount; i++)
                                                    {
                                                        CComPtr<IShellItem> pItem;
                                                        pResults->GetItemAt(i, &pItem);

                                                        LPWSTR pszFilePath = NULL;
                                                        pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);

                                                        if (pszFilePath)
                                                        {
                                                            std::wstring fullPath = pszFilePath;
                                                            std::wstring filename = fs::path(fullPath).filename().wstring();
                                                            std::wstring destination = utf8_to_wstring(ShaderDir + "\\") + filename;
                                                            std::wstring DisabledDest = utf8_to_wstring(ShaderDir + "\\Disabled Shaders\\") + filename;
                                                            if (!PathFileExistsW(destination.c_str()) && !PathFileExistsW(DisabledDest.c_str()))
                                                            {
                                                                CopyFileW(fullPath.c_str(), destination.c_str(), FALSE);
                                                            }
                                                            CoTaskMemFree(pszFilePath);
                                                        }
                                                    }
                                                    UpdateShaderList = true;
                                                }
                                            }
                                        }
                                        CoUninitialize();
                                    }
                                }
                                else
                                {
                                    struct ComInit {
                                        ComInit() { CoInitialize(nullptr); }
                                        ~ComInit() { CoUninitialize(); }
                                    };
                                    ComInit com;
                                    CComPtr<IFileOpenDialog> pFolderDlg;
                                    HRESULT hr = CoCreateInstance(
                                        CLSID_FileOpenDialog,
                                        NULL,
                                        CLSCTX_ALL,
                                        IID_IFileOpenDialog,
                                        (void**)&pFolderDlg
                                    );

                                    if (SUCCEEDED(hr)) {
                                        DWORD options;
                                        pFolderDlg->GetOptions(&options);
                                        pFolderDlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                                        hr = pFolderDlg->Show(NULL);

                                        if (SUCCEEDED(hr)) {
                                            CComPtr<IShellItem> pItem;
                                            hr = pFolderDlg->GetResult(&pItem);

                                            if (SUCCEEDED(hr)) {
                                                LPWSTR path;
                                                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);

                                                if (SUCCEEDED(hr)) {
                                                    std::wstring PickedFolder = path;
                                                    std::vector<std::wstring> FilesToCopy = getInDirectoryW(PickedFolder, false);
                                                    for (std::wstring CurrentFile : FilesToCopy)
                                                    {
                                                        if (!PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + CurrentFile).c_str()))
                                                        {
                                                            CopyFileW(
                                                                (PickedFolder + L"\\" + CurrentFile).c_str(),
                                                                (utf8_to_wstring(ShaderDir) + L"\\" + CurrentFile).c_str(), true);
                                                        }
                                                    }
                                                    UpdateShaderList = true;
                                                    CoTaskMemFree(path);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            ImGui::SameLine();
                            ImGui::Checkbox("Select per folder##Shader", &ShaderFolderPick);
                            if (ImGui::Button("Open Modrinth Page##Shader"))
                            {
                                std::string url = "https://modrinth.com/discover/shaders?v=" + UsedVersions[SelectedIndex];
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Open CurseForge Page##Shader"))
                            {
                                std::string url = "https://www.curseforge.com/minecraft/search?page=1&pageSize=20&sortBy=relevancy&class=shaders&version=" + UsedVersions[SelectedIndex];
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            if (!ShaderListAndEnabled.empty())
                            {
                                if (ImGui::Button("Remove all##Shaders"))
                                {
                                    for (int i = 0; i < ShaderListAndEnabled.size(); i++)
                                    {
                                        if (ShaderListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + ShaderListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ShaderDir) + L"\\" + ShaderListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + ShaderListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + ShaderListAndEnabled[i].first).c_str());
                                            }
                                        }
                                    }
                                    ShaderList.clear();
                                    ShaderListAndEnabled.clear();
                                    DisabledShaderList.clear();
                                    UpdateLastUsed = true;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Disable all##Shaders"))
                                {
                                    for (int i = 0; i < ShaderListAndEnabled.size(); i++)
                                    {
                                        ShaderListAndEnabled[i].second = false;
                                        if (PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + ShaderListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileW((utf8_to_wstring(ShaderDir) + L"\\" + ShaderListAndEnabled[i].first).c_str(), (utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + ShaderListAndEnabled[i].first).c_str());
                                        }
                                    }
                                    UpdateLastUsed = true;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Enable all##Shaders"))
                                {
                                    for (int i = 0; i < ShaderListAndEnabled.size(); i++)
                                    {
                                        ShaderListAndEnabled[i].second = true;
                                        if (PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + ShaderListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + ShaderListAndEnabled[i].first).c_str(), (utf8_to_wstring(ShaderDir) + L"\\" + ShaderListAndEnabled[i].first).c_str());
                                        }
                                    }
                                    UpdateLastUsed = true;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Erase All Shaders Settings"))
                                {
                                    for (std::wstring& ConfFile : getInDirectoryW(utf8_to_wstring(ShaderDir), false))
                                    {
                                        if (fs::path(utf8_to_wstring(ShaderDir) + L"\\" + ConfFile).extension() == ".txt")
                                        {
                                            DeleteFileW((utf8_to_wstring(ShaderDir) + L"\\" + ConfFile).c_str());
                                        }
                                    }
                                }
                                ImGui::Text("Shader List:");
                                ImGui::Separator();
                                for (int i = 0; i < ShaderListAndEnabled.size(); i++)
                                {
                                    ImGui::TextWrapped("%s", wstring_to_utf8(ShaderListAndEnabled[i].first).c_str());
                                    ImGui::SameLine();
                                    if (ImGui::Checkbox(("Enabled##Shader" + to_string(i)).c_str(), reinterpret_cast<bool*>(&ShaderListAndEnabled[i].second)))
                                    {
                                        if (!ShaderListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + ShaderListAndEnabled[i].first).c_str()))
                                            {
                                                MoveFileW((utf8_to_wstring(ShaderDir) + L"\\" + ShaderListAndEnabled[i].first).c_str(), (utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + ShaderListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + ShaderListAndEnabled[i].first).c_str()))
                                            {
                                                MoveFileW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + ShaderListAndEnabled[i].first).c_str(), (utf8_to_wstring(ShaderDir) + L"\\" + ShaderListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        UpdateLastUsed = true;
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button(("Remove##Shader" + to_string(i)).c_str()))
                                    {
                                        UpdateLastUsed = true;
                                        if (ShaderListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\" + ShaderListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ShaderDir) + L"\\" + ShaderListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + ShaderListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\" + ShaderListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        ShaderListAndEnabled.erase(ShaderListAndEnabled.begin() + i);
                                        if (i != 0)
                                        {
                                            i--;
                                        }
                                    }
                                    ImGui::Separator();
                                }
                            }
                        }
                        if (UpdateShaderList)
                        {
                            ShaderList.clear();
                            ShaderListAndEnabled.clear();
                            DisabledShaderList.clear();
                            ShaderDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\shaderpacks";
                            ShaderList = getInDirectoryW(utf8_to_wstring(ShaderDir), false);
                            for (int i = 0; i < ShaderList.size(); i++)
                            {
                                if (fs::path(ShaderList[i]).extension() == ".zip")
                                {
                                    ShaderListAndEnabled.push_back(make_pair(ShaderList[i], true));
                                }
                            }
                            DisabledShaderList = getInDirectoryW(utf8_to_wstring(ShaderDir) + L"\\Disabled Shaders\\", false);
                            for (int i = 0; i < DisabledShaderList.size(); i++)
                            {
                                if (fs::path(DisabledShaderList[i]).extension() == ".zip")
                                {
                                    ShaderListAndEnabled.push_back(make_pair(DisabledShaderList[i], false));
                                }
                            }
                            std::sort(ShaderListAndEnabled.begin(), ShaderListAndEnabled.end());
                            UpdateShaderList = false;
                        }
                        if (ImGui::CollapsingHeader("Config"))
                        {
                            ImGui::Text("TBD");
                        }
                        if (UpdateLastUsed)
                        {
                            auto CurrentTime = std::chrono::system_clock::now();
                            auto localzone = std::chrono::current_zone();
                            auto LocalTime = std::chrono::zoned_time{ localzone, CurrentTime };
                            std::string formatedTime = std::format("{:%Y-%m-%d %H:%M:%S}", CurrentTime);
                            formatedTime.erase(10, 1);
                            formatedTime.insert(10, "T");
                            formatedTime.erase(23, formatedTime.size());
                            formatedTime.insert(23, "Z");
                            std::ifstream LauncherProfiles(minecraftDir + "\\launcher_profiles.json");
                            json data = json::parse(LauncherProfiles);
                            for (auto& profile : data["profiles"]) {
                                if (NonModifiedUsedDates[SelectedIndex] == (std::string)profile.value("lastUsed", ""))
                                {
                                    data["profiles"][UsedProfileIds[SelectedIndex]]["lastUsed"] = formatedTime;
                                }
                            }
                            std::ofstream o(minecraftDir + "\\launcher_profiles.json");
                            o << std::setw(4) << data << std::endl;
                        }
                    }
                    else
                    {
                        ImGui::Text("No modded profiles found!");
                    }
                    ImGui::EndTabItem();
                }


                //
                // Server Tab
                //

                if (ImGui::BeginTabItem("Server"))
                {
                    if (DisableServerInstalling)
                    {
                        ImGui::BeginDisabled();
                    }
                    if (DisableServerInstallButton)
                    {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Button("Reload servers"))
                    {
                        ServerInit = true;
                    }
                    if (Success)
                    {
                        for (int i = 0; i < ServerList.size(); i++)
                        {
                            if (ServerList[i] == ServerName)
                            {
                                ServerSelectedIndex = i;
                                break;
                            }
                        }
                        Success = false;
                    }
                    if (ImGui::Button("Add Server"))
                    {
                        if (!ShowServerInstall)
                        {
                            if (!PathFileExistsA((StrCurrentDirectory + "\\" + "loader").c_str()))
                            {
                                g_Status.currentFileName = L"Downloading asset: loader(index)";
                                CURL* curl = curl_easy_init();
                                if (curl) {
                                    curl_easy_setopt(curl, CURLOPT_URL, "https://meta.fabricmc.net/v2/versions/loader");
                                    FILE* fp = _wfopen((utf8_to_wstring(StrCurrentDirectory) + L"\\" + L"loader").c_str(), L"wb");
                                    if (fp == NULL) {
                                        perror("File open failed");
                                        return;
                                    }
                                    if (fp) {
                                        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                        CURLcode res = curl_easy_perform(curl);
                                        g_Status.isDownloading = true;
                                        if (res != CURLE_OK) {
                                            if (fs::exists(utf8_to_wstring(StrCurrentDirectory) + L"\\" + L"loader"))
                                            {
                                                DeleteFileW((utf8_to_wstring(StrCurrentDirectory) + L"\\" + L"loader").c_str());
                                            }
                                        }
                                        fclose(fp);
                                    }
                                    curl_easy_cleanup(curl);
                                }
                                g_Status.isDownloading = false;
                            }
                            if (PathFileExistsA((StrCurrentDirectory + "\\" + "loader").c_str()))
                            {
                                std::ifstream loader(StrCurrentDirectory + "\\" + "loader");
                                if (loader.is_open())
                                {
                                    json jloader = json::parse(loader);
                                    for (auto& item : jloader)
                                    {
                                        if (item.contains("version"))
                                        {
                                            MostRecentFabricLoaderVersion = item["version"];
                                            break;
                                        }
                                    }
                                    loader.close();
                                    DeleteFileA((StrCurrentDirectory + "\\" + "loader").c_str());
                                }
                            }
                            if (MostRecentFabricLoaderVersion != "")
                            {
                                if (!PathFileExistsA((StrCurrentDirectory + "\\" + "versions").c_str()))
                                {
                                    g_Status.currentFileName = L"Downloading asset: versions(index)";
                                    CURL* curl = curl_easy_init();
                                    if (curl) {
                                        curl_easy_setopt(curl, CURLOPT_URL, "https://meta.fabricmc.net/v2/versions");
                                        FILE* fp = _wfopen((utf8_to_wstring(StrCurrentDirectory) + L"\\" + L"versions").c_str(), L"wb");
                                        if (fp == NULL) {
                                            perror("File open failed");
                                            return;
                                        }
                                        if (fp) {
                                            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                            curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                            CURLcode res = curl_easy_perform(curl);
                                            g_Status.isDownloading = true;
                                            if (res != CURLE_OK) {
                                                if (fs::exists(utf8_to_wstring(StrCurrentDirectory) + L"\\" + L"versions"))
                                                {
                                                    DeleteFileW((utf8_to_wstring(StrCurrentDirectory) + L"\\" + L"versions").c_str());
                                                }
                                            }
                                            fclose(fp);
                                        }
                                        curl_easy_cleanup(curl);
                                    }
                                    g_Status.isDownloading = false;
                                }
                                if (PathFileExistsA((StrCurrentDirectory + "\\" + "versions").c_str()))
                                {
                                    std::ifstream ver(StrCurrentDirectory + "\\" + "versions");
                                    if (ver.is_open())
                                    {
                                        json versions = json::parse(ver);
                                        for (auto& curritem : versions["game"])
                                        {
                                            if (curritem.contains("version"))
                                            {
                                                FabricServerVersions.push_back(curritem["version"]);
                                            }
                                        }
                                        ver.close();
                                        DeleteFileA((StrCurrentDirectory + "\\" + "versions").c_str());
                                    }
                                }
                            }
                            ShowServerInstall = true;
                        }
                    }
                    if (DisableServerInstalling)
                    {
                        ImGui::EndDisabled();
                    }
                    if (ShowServerInstall)
                    {
                        if (!FabricServerVersions.empty())
                        {
                            ImGui::Text("Server version:");
                            ImGui::SameLine();
                            if (ImGui::BeginCombo("##Combo3", FabricServerVersions[ServerVersionsSelectedIndex].c_str()))
                            {
                                for (int i = 0; i < FabricServerVersions.size(); ++i)
                                {
                                    const bool isSelected = (ServerVersionsSelectedIndex == i);
                                    if (ImGui::Selectable(FabricServerVersions[i].c_str(), isSelected))
                                    {
                                        ServerVersionsSelectedIndex = i;
                                    }
                                    if (isSelected)
                                    {
                                        ImGui::SetItemDefaultFocus();
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::Text("Server name:");
                            ImGui::SameLine();
                            ImGui::InputText("##a", ServerName, IM_ARRAYSIZE(ServerName));
                            ImGui::Text("Minimum Server RAM:");
                            ImGui::SameLine();
                            ImGui::InputInt("##b", &MinimumRAM);
                            ImGui::SameLine();
                            ImGui::Text("Maximum Server RAM:");
                            ImGui::SameLine();
                            ImGui::InputInt("##c", &MaximumRAM);
                            ImGui::SameLine();
                            if (ImGui::BeginCombo("##Combo4", Unit[UnitSelectedIndex].c_str()))
                            {
                                for (int i = 0; i < Unit.size(); ++i)
                                {
                                    const bool isSelected = (UnitSelectedIndex == i);
                                    if (ImGui::Selectable(Unit[i].c_str(), isSelected))
                                    {
                                        UnitSelectedIndex = i;
                                    }
                                    if (isSelected)
                                    {
                                        ImGui::SetItemDefaultFocus();
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            if (ImGui::Button("Install server"))
                            {
                                ShowSuccess = false;
                                Success = false;
                                ServerClosed = false;
                                if (MinimumRAM < MaximumRAM)
                                {
                                    ShowRAMError = false;
                                }
                                else
                                {
                                    ShowRAMError = true;
                                }
                                if (ServerName[0] == '\0')
                                {
                                    AskNamePrompt = true;
                                }
                                else
                                {
                                    AskNamePrompt = false;
                                }
                                for (auto& Servers : ServerList)
                                {
                                    if (ServerName == Servers)
                                    {
                                        NameAlreadyExists = true;
                                        break;
                                    }
                                    else
                                    {
                                        NameAlreadyExists = false;
                                    }
                                }
                                if (!ShowRAMError && !AskNamePrompt && !NameAlreadyExists)
                                {
                                    ShowCancelButton = false;
                                    DisableServerSelection = true;
                                    if (!PathFileExistsA((StrCurrentDirectory + "\\" + "installer").c_str()))
                                    {
                                        g_Status.currentFileName = L"Downloading asset: installer(index)";
                                        CURL* curl = curl_easy_init();
                                        if (curl) {
                                            curl_easy_setopt(curl, CURLOPT_URL, "https://meta.fabricmc.net/v2/versions/installer");
                                            FILE* fp = _wfopen((utf8_to_wstring(StrCurrentDirectory) + L"\\" + L"installer").c_str(), L"wb");
                                            if (fp == NULL) {
                                                perror("File open failed");
                                                return;
                                            }
                                            if (fp) {
                                                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                CURLcode res = curl_easy_perform(curl);
                                                g_Status.isDownloading = true;
                                                if (res != CURLE_OK) {
                                                    if (fs::exists(utf8_to_wstring(StrCurrentDirectory) + L"\\" + L"installer"))
                                                    {
                                                        DeleteFileW((utf8_to_wstring(StrCurrentDirectory) + L"\\" + L"installer").c_str());
                                                    }
                                                }
                                                fclose(fp);
                                            }
                                            curl_easy_cleanup(curl);
                                        }
                                        g_Status.isDownloading = false;
                                    }
                                    if (PathFileExistsA((StrCurrentDirectory + "\\" + "installer").c_str()))
                                    {
                                        std::ifstream installers(StrCurrentDirectory + "\\" + "installer");
                                        if (installers.is_open())
                                        {
                                            json jinstallers = json::parse(installers);
                                            for (auto& item : jinstallers)
                                            {
                                                if (item.contains("version"))
                                                {
                                                    MostRecentFabricInstallerVersion = item["version"];
                                                    break;
                                                }
                                            }
                                            installers.close();
                                            DeleteFileA((StrCurrentDirectory + "\\" + "installer").c_str());
                                        }
                                    }
                                    if (!PathFileExistsA((ServerDir + "\\" + ServerName).c_str()))
                                    {
                                        fs::create_directory((ServerDir + "\\" + ServerName).c_str());
                                    }
                                    if (!PathFileExistsA((ServerDir + "\\" + ServerName + "\\" + "jar").c_str()))
                                    {
                                        g_Status.currentFileName = L"Downloading asset: server.jar";
                                        CURL* curl = curl_easy_init();
                                        if (curl) {
                                            curl_easy_setopt(curl, CURLOPT_URL, ("https://meta.fabricmc.net/v2/versions/loader/" + FabricServerVersions[ServerVersionsSelectedIndex] + "/" + MostRecentFabricLoaderVersion + "/" + MostRecentFabricInstallerVersion + "/server/jar").c_str());
                                            FILE* fp = _wfopen((utf8_to_wstring(ServerDir + "\\" + ServerName) + L"\\" + L"server.jar").c_str(), L"wb");
                                            if (fp == NULL) {
                                                perror("File open failed");
                                                return;
                                            }
                                            if (fp) {
                                                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                CURLcode res = curl_easy_perform(curl);
                                                g_Status.isDownloading = true;
                                                if (res != CURLE_OK) {
                                                    if (fs::exists(utf8_to_wstring(ServerDir + "\\" + ServerName) + L"\\" + L"server.jar"))
                                                    {
                                                        DeleteFileW((utf8_to_wstring(ServerDir + "\\" + ServerName) + L"\\" + L"server.jar").c_str());
                                                    }
                                                }
                                                fclose(fp);
                                            }
                                            curl_easy_cleanup(curl);
                                        }
                                        g_Status.isDownloading = false;
                                    }
                                    if (PathFileExistsA((ServerDir + "\\" + ServerName + "\\" + "jar").c_str()) &&
                                        !PathFileExistsA((ServerDir + "\\" + ServerName + "\\" + "server.jar").c_str()))
                                    {
                                        MoveFileA((ServerDir + "\\" + ServerName + "\\" + "jar").c_str(), (ServerDir + "\\" + ServerName + "\\" + "server.jar").c_str());
                                    }
                                    if (PathFileExistsA((ServerDir + "\\" + ServerName + "\\" + "server.jar").c_str()))
                                    {
                                        std::string SelectedUnit = Unit[UnitSelectedIndex].substr(0, 1);
                                        std::wstring passtoshell = utf8_to_wstring("java -jar -Xms" + to_string(MinimumRAM) + SelectedUnit + " -Xmx" + to_string(MaximumRAM) + SelectedUnit + R"( ")" + ServerDir + "\\" + ServerName + "\\" + "server.jar" + R"(")" + " nogui");
                                        ServerHandle = Execute(passtoshell.c_str(), utf8_to_wstring(ServerDir + "\\" + ServerName), true, false, false);
                                        if (ServerHandle != nullptr) {
                                            IsInstallingServer = true;
                                        }
                                    }
                                    
                                }
                            }
                            if (DisableServerInstallButton)
                            {
                                ImGui::EndDisabled();
                            }
                            ImGui::SameLine();
                        }
                        if (IsInstallingServer)
                        {
                            UpdateServerState();
                            DWORD exitCode;
                            if (GetExitCodeProcess(ServerHandle, &exitCode) && exitCode != STILL_ACTIVE || ServerClosed)
                            {
                                EulaLines.clear();
                                IsInstallingServer = false;
                                if (PathFileExistsA((ServerDir + "\\" + ServerName + "\\" + "eula.txt").c_str()))
                                {
                                    std::ifstream eula((ServerDir + "\\" + ServerName + "\\" + "eula.txt"));
                                    std::string currline;
                                    if (eula.is_open())
                                    {
                                        while (std::getline(eula, currline))
                                        {
                                            EulaLines.push_back(currline);
                                        }
                                        eula.close();
                                    }
                                }
                                if (!EulaLines.empty())
                                {
                                    for (int i = 0; i < EulaLines.size(); i++)
                                    {
                                        if (EulaLines[i].find("eula=false") != std::string::npos)
                                        {
                                            EulaLines[i] = "eula=true";
                                            break;
                                        }
                                    }
                                    CIniReader RAMini(ServerDir + "\\" + ServerName + "\\" + "RAM.ini");
                                    RAMini.WriteInteger("Server RAM", "MinimumRAM", MinimumRAM);
                                    RAMini.WriteInteger("Server RAM", "MaximumRAM", MaximumRAM);
                                    RAMini.WriteInteger("Server RAM", "SavedUnit", UnitSelectedIndex);
                                    std::ofstream EulaOut((ServerDir + "\\" + ServerName + "\\" + "eula.txt").c_str(), std::ios::trunc);
                                    if (EulaOut.is_open())
                                    {
                                        for (auto& writeline : EulaLines)
                                        {
                                            EulaOut << writeline << std::endl;
                                        }
                                        Success = true;
                                        EulaOut.close();
                                    }
                                    else
                                        Success = false;
                                }
                                if (!fs::exists(ServerDir + "\\" + ServerName + "\\mods\\"))
                                {
                                    fs::create_directory(ServerDir + "\\" + ServerName + "\\mods\\");
                                }
                                if (!fs::exists(ServerDir + "\\" + ServerName + "\\mods\\Disabled Mods\\"))
                                {
                                    fs::create_directory(ServerDir + "\\" + ServerName + "\\mods\\Disabled Mods\\");
                                }
                            }
                            else
                            {
                                ImGui::Text("Installing server... please wait.");
                                DisableServerInstallButton = true;
                            }
                        }
                        if (ShowCancelButton)
                        {
                            if (ImGui::Button("Cancel"))
                            {
                                ShowServerInstall = false;
                                Success = false;
                                ShowSuccess = false;
                                ShowRAMError = false;
                                AskNamePrompt = false;
                                MostRecentFabricLoaderVersion = "";
                                FabricServerVersions.clear();
                                ShowCancelButton = true;
                            }
                        }
                        if (NameAlreadyExists)
                        {
                            ImGui::Text("This server name already exists!");
                        }
                        if (AskNamePrompt)
                        {
                            ImGui::Text("Please provide a server name!");
                        }
                        if (ShowRAMError)
                        {
                            ImGui::Text("Minimum RAM can't be more than the Maximum RAM!");
                        }
                    }
                    if (Success)
                    {
                        DisableServerInstallButton = false;
                        ShowSuccess = true;
                        DisableServerSelection = false;
                        ShowCancelButton = true;
                        ServerList.push_back(ServerName);
                        bit7z::Bit7zLibrary lib{ "7z.dll" };
                        std::vector<bit7z::byte_t> ServerBuffer;
                        bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                        try
                        {
                            extractor.extractMatching(ServerDir + "\\" + ServerName + "\\server.jar", "install.properties", ServerBuffer);
                        }
                        catch (bit7z::BitException& except)
                        {
                            WriteToLog(except.what() + std::string(" At File: ") + ServerDir + "\\" + ServerName + "server.jar");
                        }
                        std::string properties = "";
                        for (int i = 0; i < ServerBuffer.size(); i++)
                        {
                            properties += ServerBuffer[i];
                        }
                        size_t pos = properties.find("game-version=");
                        if (pos != std::string::npos)
                        {
                            std::string GameVer = properties.substr(pos + 13);
                            ServerUsedVersions.push_back(GameVer);
                            ServerListWithVersions.push_back(utf8_to_wstring(ServerName + std::string(" (") + GameVer + ")"));
                        }
                        if (ServerSelectedIndex > 0)
                        {
                            ServerSelectedIndex++;
                        }
                        UpdateServerModsList = true;
                        UpdateServerResourcePackList = true;
                        Success = false;
                    }
                    if (ShowSuccess)
                    {
                        ImGui::Text("Server successfully installed!");
                    }
                    if (PreServerSelectedIndex != ServerSelectedIndex)
                    {
                        ShowDeletionConfirmation = false;
                        ServerModsDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods";
                        ServerConfigDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\config";
                        ServerResourcePackDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\resourcepacks";
                        iniReader.WriteString("Server", "Most Recent Server", ServerList[ServerSelectedIndex]);
                        UpdateServerModsList = true;
                        UpdateServerResourcePackList = true;
                        Options.clear();
                        ServerOptions.clear();
                        ServerOptionValues.clear();
                        NamedOptions.clear();
                        NamedOptionValues.clear();
                        ServerModListAndEnabled.clear();
                        ServerDisplayModListAndEnabled.clear();
                        ServerResourcePackListAndEnabled.clear();
                        ServerDisplayResourcePackListAndEnabled.clear();
                        PreServerSelectedIndex = ServerSelectedIndex;
                    }
                    if (!ServerList.empty())
                    {
                        if (DisableServerSelection)
                        {
                            ImGui::BeginDisabled();
                        }
                        ImGui::Text("Current Server:");
                        ImGui::SameLine();
                        if (ImGui::BeginCombo("##Combo5", wstring_to_utf8(ServerListWithVersions[ServerSelectedIndex]).c_str())) {
                            for (int i = 0; i < ServerListWithVersions.size(); ++i)
                            {
                                const bool isSelected = (ServerSelectedIndex == i);
                                if (ImGui::Selectable(wstring_to_utf8(ServerListWithVersions[i]).c_str(), isSelected))
                                {
                                    ServerSelectedIndex = i;
                                }
                                if (isSelected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        if (ShowChangeServerNameButton)
                        {
                            if (ImGui::Button("Change server name"))
                            {
                                ShowNameChange = true;
                                ShowChangeServerNameButton = false;
                                InitName = true;
                            }
                        }
                        if (ShowNameChange)
                        {
                            if (InitName)
                            {
                                strcpy(Namebuf, ServerList[ServerSelectedIndex].c_str());
                                InitName = false;
                            }
                            if (ImGui::InputText("##name", Namebuf, IM_ARRAYSIZE(Namebuf), ImGuiInputTextFlags_EnterReturnsTrue))
                            {
                                for (int i = 0; i < ServerList.size(); i++)
                                {
                                    if (Namebuf == ServerList[i] && Namebuf != ServerList[ServerSelectedIndex])
                                    {
                                        ShowEqualNameError = true;
                                        break;
                                    }
                                    else
                                        ShowEqualNameError = false;
                                }
                                if (!ShowEqualNameError)
                                {
                                    MoveFileA((ServerDir + "\\" + ServerList[ServerSelectedIndex]).c_str(), (ServerDir + "\\" + Namebuf).c_str());
                                    ServerList[SelectedIndex] = Namebuf;
                                    ServerListWithVersions[ServerSelectedIndex] = utf8_to_wstring(Namebuf + std::string(" (") + ServerUsedVersions[ServerSelectedIndex] + ")");
                                    ShowNameChange = false;
                                    ShowChangeServerNameButton = true;
                                    InitName = true;
                                }
                            }
                            if (ShowEqualNameError)
                            {
                                ImGui::Text("This server name already exists!");
                            }
                            if (ImGui::Button("Cancel##2"))
                            {
                                ShowNameChange = false;
                                ShowChangeServerNameButton = true;
                                InitName = true;
                            }
                        }
                        UpdateServerState();
                        if (ServerHandle == nullptr) {
                            DisableServerInstalling = false;
                            if (ImGui::Button("Run")) {
                                IsServerReady = false;
                                IsStoppingServer = false;
                                std::string SelectedUnit = Unit2[Unit2SelectedIndex].substr(0, 1);
                                std::wstring passtoshell = utf8_to_wstring("java -jar -Xms" + to_string(MinimumServerRAM) + SelectedUnit + " -Xmx" + to_string(MaximumServerRAM) + SelectedUnit + " " + ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\" + "server.jar nogui");
                                ServerHandle = Execute(passtoshell.c_str(), utf8_to_wstring(ServerDir + "\\" + ServerList[ServerSelectedIndex]), true, false, false);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Delete"))
                            {
                                ShowDeletionConfirmation = true;
                            }
                            if (ShowDeletionConfirmation)
                            {
                                ImGui::Text("Are you sure you want to delete this server?");
                                if (ImGui::Button("Yes"))
                                {
                                    if (fs::exists(ServerDir + "\\" + ServerList[ServerSelectedIndex]))
                                    {
                                        if (fs::remove_all(ServerDir + "\\" + ServerList[ServerSelectedIndex]))
                                        {
                                            if (ServerSelectedIndex != 0)
                                            {
                                                ServerSelectedIndex = ServerSelectedIndex - 1;
                                            }
                                            else
                                            {
                                                ServerList.clear();
                                                ServerListWithVersions.clear();
                                            }
                                        }
                                    }
                                    ShowDeletionConfirmation = false;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("No"))
                                {
                                    ShowDeletionConfirmation = false;
                                }
                            }
                            if (ImGui::Button("Add Modpack##Server") && !g_Status.isDownloading)
                            {
                                if (ServerModPackFolderPick)
                                {
                                    struct ComInit {
                                        ComInit() { CoInitialize(nullptr); }
                                        ~ComInit() { CoUninitialize(); }
                                    };
                                    ComInit com;
                                    CComPtr<IFileOpenDialog> pFolderDlg;
                                    HRESULT hr = CoCreateInstance(
                                        CLSID_FileOpenDialog,
                                        NULL,
                                        CLSCTX_ALL,
                                        IID_IFileOpenDialog,
                                        (void**)&pFolderDlg
                                    );

                                    if (SUCCEEDED(hr)) {
                                        DWORD options;
                                        pFolderDlg->GetOptions(&options);
                                        pFolderDlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                                        hr = pFolderDlg->Show(NULL);

                                        if (SUCCEEDED(hr)) {
                                            CComPtr<IShellItem> pItem;
                                            hr = pFolderDlg->GetResult(&pItem);

                                            if (SUCCEEDED(hr)) {
                                                LPWSTR path;
                                                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);

                                                if (SUCCEEDED(hr)) {
                                                    std::thread([=]()
                                                        {
                                                            bit7z::Bit7zLibrary lib{ "7z.dll" };
                                                            std::string ModPackDir = wstring_to_utf8(path);
                                                            if (PathFileExistsA((ModPackDir).c_str()) && !fs::is_empty(ModPackDir))
                                                            {
                                                                if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods").c_str()))
                                                                {
                                                                    std::vector<std::wstring> ModpackMods = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\mods", false);
                                                                    for (std::wstring CurrentMod : ModpackMods)
                                                                    {
                                                                        if (!PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + CurrentMod).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerModsDir) + CurrentMod).c_str()))
                                                                        {
                                                                            CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods\\" + CurrentMod).c_str(), (utf8_to_wstring(ServerModsDir) + L"\\" + CurrentMod).c_str(), true);
                                                                        }
                                                                    }
                                                                }
                                                                if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks").c_str()))
                                                                {
                                                                    std::vector<std::wstring> ModpackResourcePacks = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks", false);
                                                                    for (std::wstring CurrentResourcePack : ModpackResourcePacks)
                                                                    {
                                                                        if (!PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentResourcePack).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentResourcePack).c_str()))
                                                                        {
                                                                            CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks\\" + CurrentResourcePack).c_str(), (utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentResourcePack).c_str(), true);
                                                                        }
                                                                    }
                                                                }
                                                                if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config").c_str()))
                                                                {
                                                                    std::vector<std::wstring> ModpackConfig = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\config", false);
                                                                    for (std::wstring CurrentConfig : ModpackConfig)
                                                                    {
                                                                        if (!PathFileExistsW((utf8_to_wstring(ServerConfigDir) + L"\\Disabled Config\\" + CurrentConfig).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerConfigDir) + L"\\" + CurrentConfig).c_str()))
                                                                        {
                                                                            CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config\\" + CurrentConfig).c_str(), (utf8_to_wstring(ServerConfigDir) + L"\\" + CurrentConfig).c_str(), true);
                                                                        }
                                                                    }
                                                                }
                                                                std::vector<std::wstring> ModpackFolders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides", true);
                                                                for (std::wstring CurrentFolder2 : ModpackFolders)
                                                                {
                                                                    if (!PathFileExistsW((utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[ServerSelectedIndex]) + L"\\" + CurrentFolder2).c_str()))
                                                                    {
                                                                        fs::create_directory(utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[ServerSelectedIndex]) + L"\\" + CurrentFolder2);
                                                                    }
                                                                    std::vector<std::wstring> CurrentFiles = getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2);
                                                                    for (std::wstring CurrentFile : CurrentFiles)
                                                                    {
                                                                        if (!PathFileExistsW((utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[ServerSelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile).c_str()))
                                                                        {
                                                                            try
                                                                            {
                                                                                fs::copy(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2 + L"\\" + CurrentFile, utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[ServerSelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile);
                                                                            }
                                                                            catch (...)
                                                                            {
                                                                                //WriteToLog("Copying failure on " + ModPackDir + "\\overrides\\" + CurrentFolder2 + "\\" + CurrentFile + " !");
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                                if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\manifest.json").c_str()))
                                                                {
                                                                    if (!PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\Downloads").c_str()))
                                                                    {
                                                                        fs::create_directory(ModPackDir + "\\Downloads");
                                                                    }
                                                                    std::ifstream ModpackMods(ModPackDir + "\\manifest.json");
                                                                    json data = json::parse(ModpackMods);
                                                                    bool MustRun = false;
                                                                    std::set<int> existingFileIDs;
                                                                    std::set<int> existingProjectIDs;
                                                                    std::ifstream RFileIDs(ModPackDir + "\\Downloads\\FileIDs.txt");
                                                                    std::ifstream RProjectIDs(ModPackDir + "\\Downloads\\ProjectIDs.txt");
                                                                    std::string line;
                                                                    while (std::getline(RFileIDs, line)) {
                                                                        if (!line.empty())
                                                                        {
                                                                            existingFileIDs.insert(std::stoi(line));
                                                                        }
                                                                    }
                                                                    RFileIDs.close();
                                                                    std::string Pline;
                                                                    while (std::getline(RProjectIDs, Pline)) {
                                                                        if (!Pline.empty())
                                                                        {
                                                                            existingProjectIDs.insert(std::stoi(Pline));
                                                                        }
                                                                    }
                                                                    RProjectIDs.close();
                                                                    std::ofstream OutProjectIDs(ModPackDir + "\\Downloads\\ProjectIDs.txt", std::ios::app);
                                                                    std::ofstream OutFileIDs(ModPackDir + "\\Downloads\\FileIDs.txt", std::ios::app);
                                                                    for (auto& file : data["files"])
                                                                    {
                                                                        int CurrentProjID = file.value("projectID", 0);
                                                                        int CurrentFileID = file.value("fileID", 0);
                                                                        if (existingFileIDs.count(CurrentFileID) == 0 && existingProjectIDs.count(CurrentProjID) == 0)
                                                                        {
                                                                            CURL* curl = curl_easy_init();
                                                                            if (curl) {
                                                                                std::wstring finalFileName = L"unknown.jar";
                                                                                std::wstring tempPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\downloading.tmp";
                                                                                FILE* fp = _wfopen(tempPath.c_str(), L"wb");
                                                                                if (fp == NULL) {
                                                                                    perror("File open failed");
                                                                                    return;
                                                                                }
                                                                                if (fp) {
                                                                                    curl_easy_setopt(curl, CURLOPT_URL, ("https://www.curseforge.com/api/v1/mods/" + to_string(CurrentProjID) + "/files/" + to_string(CurrentFileID) + "/download").c_str());
                                                                                    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                                                    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                                                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                                                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                                                    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
                                                                                    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &finalFileName);
                                                                                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                                                    curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                                                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                                                    CURLcode res = curl_easy_perform(curl);
                                                                                    g_Status.isDownloading = true;
                                                                                    fclose(fp);
                                                                                    if (res != CURLE_OK) {
                                                                                        if (fs::exists(tempPath))
                                                                                        {
                                                                                            DeleteFileW(tempPath.c_str());
                                                                                        }
                                                                                    }
                                                                                    else
                                                                                    {
                                                                                        if (finalFileName == L"unknown.jar")
                                                                                        {
                                                                                            char* url = NULL;
                                                                                            curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
                                                                                            if (url) {
                                                                                                std::string finalUrl = url;

                                                                                                size_t queryPos = finalUrl.find('?');
                                                                                                if (queryPos != std::string::npos) finalUrl.resize(queryPos);

                                                                                                size_t lastSlash = finalUrl.find_last_of('/');
                                                                                                if (lastSlash != std::string::npos) {
                                                                                                    std::string encodedName = finalUrl.substr(lastSlash + 1);

                                                                                                    int outLength;
                                                                                                    char* decoded = curl_easy_unescape(curl, encodedName.c_str(), (int)encodedName.length(), &outLength);
                                                                                                    if (decoded) {
                                                                                                        finalFileName = utf8_to_wstring(std::string(decoded, outLength));
                                                                                                        g_Status.currentFileName = L"Downloading asset: " + finalFileName;
                                                                                                        curl_free(decoded);
                                                                                                    }
                                                                                                }
                                                                                            }
                                                                                        }
                                                                                        std::wstring finalPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + finalFileName;
                                                                                        fs::rename(tempPath, finalPath);
                                                                                        OutProjectIDs << CurrentProjID << "\n";
                                                                                        OutFileIDs << CurrentFileID << "\n";
                                                                                        existingFileIDs.insert(CurrentFileID);
                                                                                        existingProjectIDs.insert(CurrentProjID);
                                                                                    }
                                                                                }
                                                                                curl_easy_cleanup(curl);
                                                                            }
                                                                        }
                                                                    }
                                                                    for (std::wstring& CurrentFile : getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\Downloads", false))
                                                                    {
                                                                        std::filesystem::path CurrentPath = utf8_to_wstring(ModPackDir) + L"\\Downloads" + CurrentFile;
                                                                        if (CurrentPath.extension() == ".zip" || CurrentPath.extension() == ".jar")
                                                                        {
                                                                            std::vector<std::wstring> ArchiveItems;
                                                                            try {
                                                                                std::wstring FilPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile;
                                                                                bit7z::BitArchiveReader arc{ lib, wstring_to_utf8(FilPath), bit7z::BitFormat::Zip };
                                                                                for (const auto& item : arc)
                                                                                {
                                                                                    ArchiveItems.push_back(utf8_to_wstring(item.name()));
                                                                                }
                                                                            }
                                                                            catch (const bit7z::BitException& ex) {
                                                                                WriteToLog(std::string(ex.what()));
                                                                            }
                                                                            for (std::wstring& CurrentItem : ArchiveItems)
                                                                            {
                                                                                if (CurrentItem.find(L"fabric.mod.json") != std::wstring::npos ||
                                                                                    CurrentItem.find(L"mods.toml") != std::wstring::npos ||
                                                                                    CurrentItem.find(L"mcmod.info") != std::wstring::npos)
                                                                                {
                                                                                    if (!PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + CurrentFile).c_str()))
                                                                                    {
                                                                                        if (fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".jar")
                                                                                        {
                                                                                            CopyFileW((utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).c_str(), (utf8_to_wstring(ServerModsDir) + L"\\" + CurrentFile).c_str(), true);
                                                                                        }
                                                                                        break;
                                                                                    }
                                                                                }
                                                                                else if (CurrentItem.find(L"pack.mcmeta") != std::wstring::npos)
                                                                                {
                                                                                    if (!PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentFile).c_str()))
                                                                                    {
                                                                                        if (fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".zip" || fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".txt")
                                                                                        {
                                                                                            CopyFileW((utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).c_str(), (utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentFile).c_str(), true);
                                                                                        }
                                                                                        break;
                                                                                    }
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                                else if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\modrinth.index.json").c_str()))
                                                                {
                                                                    std::ifstream ModpackMods(ModPackDir + "\\modrinth.index.json");
                                                                    json data = json::parse(ModpackMods);
                                                                    std::string DownloadNJ = "";
                                                                    std::wstring Path = L"";
                                                                    std::wstring FullPath = L"";
                                                                    std::wstring Name = L"";
                                                                    for (auto& file : data["files"])
                                                                    {
                                                                        if (file.contains("downloads") && file["downloads"].is_array()) {
                                                                            for (auto& item : file["downloads"]) {
                                                                                if (item.is_string()) {
                                                                                    DownloadNJ = item.get<std::string>();
                                                                                }
                                                                            }
                                                                        }
                                                                        if (file.contains("path") && file["path"].is_string())
                                                                        {
                                                                            int pos = file.value("path", "").find("/");
                                                                            Path = utf8_to_wstring(file.value("path", "").substr(0, pos));
                                                                            Name = utf8_to_wstring(file.value("path", "").substr(pos + 1, file.value("path", "").size()));
                                                                            FullPath = utf8_to_wstring(file.value("path", ""));
                                                                        }
                                                                        if (DownloadNJ != "" && Path != L"" && !fs::exists(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name))
                                                                        {
                                                                            g_Status.currentFileName = L"Downloading asset: " + Name;
                                                                            CURL* curl = curl_easy_init();
                                                                            if (curl) {
                                                                                curl_easy_setopt(curl, CURLOPT_URL, DownloadNJ.c_str());
                                                                                FILE* fp = _wfopen((utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name).c_str(), L"wb");
                                                                                if (fp == NULL) {
                                                                                    perror("File open failed");
                                                                                    return;
                                                                                }
                                                                                if (fp) {
                                                                                    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                                                    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                                                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                                                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                                                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                                                    curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                                                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                                                    CURLcode res = curl_easy_perform(curl);
                                                                                    g_Status.isDownloading = true;
                                                                                    if (res != CURLE_OK) {
                                                                                        if (fs::exists(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name))
                                                                                        {
                                                                                            DeleteFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name).c_str());
                                                                                        }
                                                                                    }
                                                                                    fclose(fp);
                                                                                }
                                                                                curl_easy_cleanup(curl);
                                                                            }
                                                                        }
                                                                    }
                                                                    for (std::wstring& CurrentFile : getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides"))
                                                                    {
                                                                        std::filesystem::path CurrentPath = utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFile;
                                                                        if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods").c_str()))
                                                                        {
                                                                            std::vector<std::wstring> ModpackMods = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\mods", false);
                                                                            for (std::wstring CurrentMod : ModpackMods)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(modsDir) + L"\\Disabled Mods\\" + CurrentMod).c_str()) && !PathFileExistsW((utf8_to_wstring(modsDir) + CurrentMod).c_str()))
                                                                                {
                                                                                    CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods\\" + CurrentMod).c_str(), (utf8_to_wstring(modsDir) + L"\\" + CurrentMod).c_str(), true);
                                                                                }
                                                                            }
                                                                        }
                                                                        if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks").c_str()))
                                                                        {
                                                                            std::vector<std::wstring> ModpackResourcePacks = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks", false);
                                                                            for (std::wstring CurrentResourcePack : ModpackResourcePacks)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentResourcePack).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentResourcePack).c_str()))
                                                                                {
                                                                                    CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks\\" + CurrentResourcePack).c_str(), (utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentResourcePack).c_str(), true);
                                                                                }
                                                                            }
                                                                        }
                                                                        if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config").c_str()))
                                                                        {
                                                                            std::vector<std::wstring> ModpackConfig = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\config", false);
                                                                            for (std::wstring CurrentConfig : ModpackConfig)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(ServerConfigDir) + L"\\Disabled Config\\" + CurrentConfig).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerConfigDir) + L"\\" + CurrentConfig).c_str()))
                                                                                {
                                                                                    CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config\\" + CurrentConfig).c_str(), (utf8_to_wstring(ServerConfigDir) + L"\\" + CurrentConfig).c_str(), true);
                                                                                }
                                                                            }
                                                                        }
                                                                        std::vector<std::wstring> ModpackFolders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides", true);
                                                                        for (std::wstring CurrentFolder2 : ModpackFolders)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[ServerSelectedIndex]) + L"\\" + CurrentFolder2).c_str()))
                                                                            {
                                                                                fs::create_directory(utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[ServerSelectedIndex]) + L"\\" + CurrentFolder2);
                                                                            }
                                                                            std::vector<std::wstring> CurrentFiles = getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2);
                                                                            for (std::wstring CurrentFile : CurrentFiles)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[ServerSelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile).c_str()))
                                                                                {
                                                                                    try
                                                                                    {
                                                                                        fs::copy(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2 + L"\\" + CurrentFile, utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[ServerSelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile);
                                                                                    }
                                                                                    catch (...)
                                                                                    {
                                                                                        //WriteToLog("Copying failure on " + ModPackDir + "\\overrides\\" + CurrentFolder2 + "\\" + CurrentFile + " !");
                                                                                    }
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                            CoTaskMemFree(path);
                                                            g_Status.isDownloading = false;
                                                            g_Status.progress = 0.0f;
                                                            ServerModListAndEnabled.clear();
                                                            ServerDisplayModListAndEnabled.clear();
                                                            ServerDisplayResourcePackListAndEnabled.clear();
                                                            ServerResourcePackListAndEnabled.clear();
                                                            UpdateServerModsList = true;
                                                            UpdateServerResourcePackList = true;
                                                        }).detach();
                                                }
                                            }
                                        }
                                    }
                                }
                                else
                                {
                                    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
                                    if (SUCCEEDED(hr))
                                    {
                                        CComPtr<IFileOpenDialog> pFileDlg;
                                        hr = pFileDlg.CoCreateInstance(CLSID_FileOpenDialog);

                                        if (SUCCEEDED(hr))
                                        {
                                            DWORD dwFlags;
                                            pFileDlg->GetOptions(&dwFlags);
                                            pFileDlg->SetOptions(dwFlags | FOS_FORCEFILESYSTEM);

                                            COMDLG_FILTERSPEC fileTypes[] = { { L"Modpack files", L"*.zip; *.mrpack" } };
                                            pFileDlg->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);

                                            hr = pFileDlg->Show(NULL);
                                            if (SUCCEEDED(hr))
                                            {
                                                CComPtr<IShellItemArray> pResults;
                                                hr = pFileDlg->GetResults(&pResults);

                                                if (SUCCEEDED(hr))
                                                {
                                                    DWORD dwCount = 0;
                                                    pResults->GetCount(&dwCount);

                                                    for (DWORD i = 0; i < dwCount; i++)
                                                    {
                                                        CComPtr<IShellItem> pItem;
                                                        pResults->GetItemAt(i, &pItem);

                                                        LPWSTR pszFilePath = NULL;
                                                        pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);

                                                        if (pszFilePath)
                                                        {
                                                            std::thread([=]()
                                                                {
                                                                    std::string fullPath = wstring_to_utf8(pszFilePath);
                                                                    std::string filename = fs::path(fullPath).stem().string();
                                                                    std::string ModPackDir = fs::path(fullPath).parent_path().string() + "\\" + filename;
                                                                    bit7z::Bit7zLibrary lib{ "7z.dll" };
                                                                    if (!PathFileExistsA(ModPackDir.c_str()))
                                                                    {
                                                                        fs::create_directory(ModPackDir);
                                                                        bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                                                                        extractor.extract(fullPath, ModPackDir);
                                                                    }
                                                                    if (PathFileExistsA((ModPackDir).c_str()) && !fs::is_empty(ModPackDir))
                                                                    {
                                                                        if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods").c_str()))
                                                                        {
                                                                            std::vector<std::wstring> ModpackMods = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\mods", false);
                                                                            for (std::wstring CurrentMod : ModpackMods)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + CurrentMod).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerModsDir) + CurrentMod).c_str()))
                                                                                {
                                                                                    CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods\\" + CurrentMod).c_str(), (utf8_to_wstring(ServerModsDir) + L"\\" + CurrentMod).c_str(), true);
                                                                                }
                                                                            }
                                                                        }
                                                                        if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks").c_str()))
                                                                        {
                                                                            std::vector<std::wstring> ModpackResourcePacks = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks", false);
                                                                            for (std::wstring CurrentResourcePack : ModpackResourcePacks)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentResourcePack).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentResourcePack).c_str()))
                                                                                {
                                                                                    CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks\\" + CurrentResourcePack).c_str(), (utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentResourcePack).c_str(), true);
                                                                                }
                                                                            }
                                                                        }
                                                                        if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config").c_str()))
                                                                        {
                                                                            std::vector<std::wstring> ModpackConfig = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\config", false);
                                                                            for (std::wstring CurrentConfig : ModpackConfig)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(ServerConfigDir) + L"\\Disabled Config\\" + CurrentConfig).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerConfigDir) + L"\\" + CurrentConfig).c_str()))
                                                                                {
                                                                                    CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config\\" + CurrentConfig).c_str(), (utf8_to_wstring(ServerConfigDir) + L"\\" + CurrentConfig).c_str(), true);
                                                                                }
                                                                            }
                                                                        }
                                                                        std::vector<std::wstring> ModpackFolders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides", true);
                                                                        for (std::wstring CurrentFolder2 : ModpackFolders)
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[SelectedIndex]) + L"\\" + CurrentFolder2).c_str()))
                                                                            {
                                                                                fs::create_directory(utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[SelectedIndex]) + L"\\" + CurrentFolder2);
                                                                            }
                                                                            std::vector<std::wstring> CurrentFiles = getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2);
                                                                            for (std::wstring CurrentFile : CurrentFiles)
                                                                            {
                                                                                if (!PathFileExistsW((utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile).c_str()))
                                                                                {
                                                                                    try
                                                                                    {
                                                                                        fs::copy(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2 + L"\\" + CurrentFile, utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile);
                                                                                    }
                                                                                    catch (...)
                                                                                    {
                                                                                        //WriteToLog("Copying failure on " + ModPackDir + "\\overrides\\" + CurrentFolder2 + "\\" + CurrentFile + " !");
                                                                                    }
                                                                                }
                                                                            }
                                                                        }
                                                                        if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\manifest.json").c_str()))
                                                                        {
                                                                            if (!PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\Downloads").c_str()))
                                                                            {
                                                                                fs::create_directory(ModPackDir + "\\Downloads");
                                                                            }
                                                                            std::ifstream ModpackMods(ModPackDir + "\\manifest.json");
                                                                            json data = json::parse(ModpackMods);
                                                                            bool MustRun = false;
                                                                            std::set<int> existingFileIDs;
                                                                            std::set<int> existingProjectIDs;
                                                                            std::ifstream RFileIDs(ModPackDir + "\\Downloads\\FileIDs.txt");
                                                                            std::ifstream RProjectIDs(ModPackDir + "\\Downloads\\ProjectIDs.txt");
                                                                            std::string line;
                                                                            while (std::getline(RFileIDs, line)) {
                                                                                if (!line.empty())
                                                                                {
                                                                                    existingFileIDs.insert(std::stoi(line));
                                                                                }
                                                                            }
                                                                            RFileIDs.close();
                                                                            std::string Pline;
                                                                            while (std::getline(RProjectIDs, Pline)) {
                                                                                if (!Pline.empty())
                                                                                {
                                                                                    existingProjectIDs.insert(std::stoi(Pline));
                                                                                }
                                                                            }
                                                                            RProjectIDs.close();
                                                                            std::ofstream OutProjectIDs(ModPackDir + "\\Downloads\\ProjectIDs.txt", std::ios::app);
                                                                            std::ofstream OutFileIDs(ModPackDir + "\\Downloads\\FileIDs.txt", std::ios::app);
                                                                            for (auto& file : data["files"])
                                                                            {
                                                                                int CurrentProjID = file.value("projectID", 0);
                                                                                int CurrentFileID = file.value("fileID", 0);
                                                                                if (existingFileIDs.count(CurrentFileID) == 0 && existingProjectIDs.count(CurrentProjID) == 0)
                                                                                {
                                                                                    CURL* curl = curl_easy_init();
                                                                                    if (curl) {
                                                                                        std::wstring finalFileName = L"unknown.jar";
                                                                                        std::wstring tempPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\downloading.tmp";
                                                                                        FILE* fp = _wfopen(tempPath.c_str(), L"wb");
                                                                                        if (fp == NULL) {
                                                                                            perror("File open failed");
                                                                                            return;
                                                                                        }
                                                                                        if (fp) {
                                                                                            curl_easy_setopt(curl, CURLOPT_URL, ("https://www.curseforge.com/api/v1/mods/" + to_string(CurrentProjID) + "/files/" + to_string(CurrentFileID) + "/download").c_str());
                                                                                            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                                                            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                                                            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                                                            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                                                            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
                                                                                            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &finalFileName);
                                                                                            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                                                            curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                                                            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                                                            CURLcode res = curl_easy_perform(curl);
                                                                                            g_Status.isDownloading = true;
                                                                                            fclose(fp);
                                                                                            if (res != CURLE_OK) {
                                                                                                if (fs::exists(tempPath))
                                                                                                {
                                                                                                    DeleteFileW(tempPath.c_str());
                                                                                                }
                                                                                            }
                                                                                            else
                                                                                            {
                                                                                                if (finalFileName == L"unknown.jar")
                                                                                                {
                                                                                                    char* url = NULL;
                                                                                                    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
                                                                                                    if (url) {
                                                                                                        std::string finalUrl = url;

                                                                                                        size_t queryPos = finalUrl.find('?');
                                                                                                        if (queryPos != std::string::npos) finalUrl.resize(queryPos);

                                                                                                        size_t lastSlash = finalUrl.find_last_of('/');
                                                                                                        if (lastSlash != std::string::npos) {
                                                                                                            std::string encodedName = finalUrl.substr(lastSlash + 1);

                                                                                                            int outLength;
                                                                                                            char* decoded = curl_easy_unescape(curl, encodedName.c_str(), (int)encodedName.length(), &outLength);
                                                                                                            if (decoded) {
                                                                                                                finalFileName = utf8_to_wstring(std::string(decoded, outLength));
                                                                                                                g_Status.currentFileName = L"Downloading asset: " + finalFileName;
                                                                                                                curl_free(decoded);
                                                                                                            }
                                                                                                        }
                                                                                                    }
                                                                                                }
                                                                                                std::wstring finalPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + finalFileName;
                                                                                                fs::rename(tempPath, finalPath);
                                                                                                OutProjectIDs << CurrentProjID << "\n";
                                                                                                OutFileIDs << CurrentFileID << "\n";
                                                                                                existingFileIDs.insert(CurrentFileID);
                                                                                                existingProjectIDs.insert(CurrentProjID);
                                                                                            }
                                                                                        }
                                                                                        curl_easy_cleanup(curl);
                                                                                    }
                                                                                }
                                                                            }
                                                                            for (std::wstring& CurrentFile : getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\Downloads", false))
                                                                            {
                                                                                std::filesystem::path CurrentPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile;
                                                                                if (CurrentPath.extension() == ".zip" || CurrentPath.extension() == ".jar")
                                                                                {
                                                                                    std::vector<std::wstring> ArchiveItems;
                                                                                    try {
                                                                                        std::wstring FilPath = utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile;
                                                                                        bit7z::BitArchiveReader arc{ lib, wstring_to_utf8(FilPath), bit7z::BitFormat::Zip };
                                                                                        for (const auto& item : arc)
                                                                                        {
                                                                                            ArchiveItems.push_back(utf8_to_wstring(item.name()));
                                                                                        }
                                                                                    }
                                                                                    catch (const bit7z::BitException& ex) {
                                                                                        WriteToLog(std::string(ex.what()));
                                                                                    }
                                                                                    for (std::wstring& CurrentItem : ArchiveItems)
                                                                                    {
                                                                                        if (CurrentItem.find(L"fabric.mod.json") != std::wstring::npos ||
                                                                                            CurrentItem.find(L"mods.toml") != std::wstring::npos ||
                                                                                            CurrentItem.find(L"mcmod.info") != std::wstring::npos)
                                                                                        {
                                                                                            if (!PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + CurrentFile).c_str()))
                                                                                            {
                                                                                                if (fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".jar")
                                                                                                {
                                                                                                    CopyFileW((utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).c_str(), (utf8_to_wstring(ServerModsDir) + L"\\" + CurrentFile).c_str(), true);
                                                                                                }
                                                                                                break;
                                                                                            }
                                                                                        }
                                                                                        else if (CurrentItem.find(L"pack.mcmeta") != std::wstring::npos)
                                                                                        {
                                                                                            if (!PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentFile).c_str()))
                                                                                            {
                                                                                                if (fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".zip" || fs::path(utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).extension() == ".txt")
                                                                                                {
                                                                                                    CopyFileW((utf8_to_wstring(ModPackDir) + L"\\Downloads\\" + CurrentFile).c_str(), (utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentFile).c_str(), true);
                                                                                                }
                                                                                                break;
                                                                                            }
                                                                                        }
                                                                                    }
                                                                                }
                                                                            }
                                                                        }
                                                                        else if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\modrinth.index.json").c_str()))
                                                                        {
                                                                            std::ifstream ModpackMods(ModPackDir + "\\modrinth.index.json");
                                                                            json data = json::parse(ModpackMods);
                                                                            std::string DownloadNJ = "";
                                                                            std::wstring Path = L"";
                                                                            std::wstring FullPath = L"";
                                                                            std::wstring Name = L"";
                                                                            for (auto& file : data["files"])
                                                                            {
                                                                                if (file.contains("downloads") && file["downloads"].is_array()) {
                                                                                    for (auto& item : file["downloads"]) {
                                                                                        if (item.is_string()) {
                                                                                            DownloadNJ = item.get<std::string>();
                                                                                        }
                                                                                    }
                                                                                }
                                                                                if (file.contains("path") && file["path"].is_string())
                                                                                {
                                                                                    int pos = file.value("path", "").find("/");
                                                                                    Path = utf8_to_wstring(file.value("path", "").substr(0, pos));
                                                                                    Name = utf8_to_wstring(file.value("path", "").substr(pos + 1, file.value("path", "").size()));
                                                                                    FullPath = utf8_to_wstring(file.value("path", ""));
                                                                                }
                                                                                if (DownloadNJ != "" && Path != L"" && !fs::exists(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name))
                                                                                {
                                                                                    g_Status.currentFileName = L"Downloading asset: " + Name;
                                                                                    CURL* curl = curl_easy_init();
                                                                                    if (curl) {
                                                                                        curl_easy_setopt(curl, CURLOPT_URL, DownloadNJ.c_str());
                                                                                        FILE* fp = _wfopen((utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name).c_str(), L"wb");
                                                                                        if (fp == NULL) {
                                                                                            perror("File open failed");
                                                                                            return;
                                                                                        }
                                                                                        if (fp) {
                                                                                            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
                                                                                            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                                                                                            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
                                                                                            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                                                                                            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                                                                                            curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
                                                                                            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                                                                                            CURLcode res = curl_easy_perform(curl);
                                                                                            g_Status.isDownloading = true;
                                                                                            if (res != CURLE_OK) {
                                                                                                if (fs::exists(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name))
                                                                                                {
                                                                                                    DeleteFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\" + Path + L"\\" + Name).c_str());
                                                                                                }
                                                                                            }
                                                                                            fclose(fp);
                                                                                        }
                                                                                        curl_easy_cleanup(curl);
                                                                                    }
                                                                                }
                                                                            }
                                                                            for (std::wstring& CurrentFile : getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides"))
                                                                            {
                                                                                std::filesystem::path CurrentPath = utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFile;
                                                                                if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods").c_str()))
                                                                                {
                                                                                    std::vector<std::wstring> ModpackMods = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\mods", false);
                                                                                    for (std::wstring CurrentMod : ModpackMods)
                                                                                    {
                                                                                        if (!PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + CurrentMod).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerModsDir) + CurrentMod).c_str()))
                                                                                        {
                                                                                            CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\mods\\" + CurrentMod).c_str(), (utf8_to_wstring(ServerModsDir) + L"\\" + CurrentMod).c_str(), true);
                                                                                        }
                                                                                    }
                                                                                }
                                                                                if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks").c_str()))
                                                                                {
                                                                                    std::vector<std::wstring> ModpackResourcePacks = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks", false);
                                                                                    for (std::wstring CurrentResourcePack : ModpackResourcePacks)
                                                                                    {
                                                                                        if (!PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentResourcePack).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentResourcePack).c_str()))
                                                                                        {
                                                                                            CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\resourcepacks\\" + CurrentResourcePack).c_str(), (utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentResourcePack).c_str(), true);
                                                                                        }
                                                                                    }
                                                                                }
                                                                                if (PathFileExistsW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config").c_str()))
                                                                                {
                                                                                    std::vector<std::wstring> ModpackConfig = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\config", false);
                                                                                    for (std::wstring CurrentConfig : ModpackConfig)
                                                                                    {
                                                                                        if (!PathFileExistsW((utf8_to_wstring(ConfigDir) + L"\\Disabled Config\\" + CurrentConfig).c_str()) && !PathFileExistsW((utf8_to_wstring(ConfigDir) + L"\\" + CurrentConfig).c_str()))
                                                                                        {
                                                                                            CopyFileW((utf8_to_wstring(ModPackDir) + L"\\overrides\\config\\" + CurrentConfig).c_str(), (utf8_to_wstring(ConfigDir) + L"\\" + CurrentConfig).c_str(), true);
                                                                                        }
                                                                                    }
                                                                                }
                                                                                std::vector<std::wstring> ModpackFolders = getInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides", true);
                                                                                for (std::wstring CurrentFolder2 : ModpackFolders)
                                                                                {
                                                                                    if (!PathFileExistsW((utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[SelectedIndex]) + L"\\" + CurrentFolder2).c_str()))
                                                                                    {
                                                                                        fs::create_directory(utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[SelectedIndex]) + L"\\" + CurrentFolder2);
                                                                                    }
                                                                                    std::vector<std::wstring> CurrentFiles = getAllInDirectoryW(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2);
                                                                                    for (std::wstring CurrentFile : CurrentFiles)
                                                                                    {
                                                                                        if (!PathFileExistsW((utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile).c_str()))
                                                                                        {
                                                                                            try
                                                                                            {
                                                                                                fs::copy(utf8_to_wstring(ModPackDir) + L"\\overrides\\" + CurrentFolder2 + L"\\" + CurrentFile, utf8_to_wstring(ServerDir) + L"\\" + utf8_to_wstring(ServerList[SelectedIndex]) + L"\\" + CurrentFolder2 + L"\\" + CurrentFile);
                                                                                            }
                                                                                            catch (...)
                                                                                            {
                                                                                                //WriteToLog("Copying failure on " + ModPackDir + "\\overrides\\" + CurrentFolder2 + "\\" + CurrentFile + " !");
                                                                                            }
                                                                                        }
                                                                                    }
                                                                                }
                                                                            }
                                                                        }
                                                                    }
                                                                    CoTaskMemFree(pszFilePath);
                                                                    g_Status.isDownloading = false;
                                                                    g_Status.progress = 0.0f;
                                                                    UpdateServerModsList = true;
                                                                    UpdateServerResourcePackList = true;
                                                                }).detach();
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            ImGui::SameLine();
                            ImGui::Checkbox("Select per folder##ServerModpack", &ServerModPackFolderPick);
                            if (g_Status.isDownloading) {
                                ImGui::Text("%s", wstring_to_utf8(g_Status.currentFileName).c_str());
                                ImGui::ProgressBar(g_Status.progress);
                            }
                        }
                        else {
                            DisableServerInstalling = true;
                            //ShowServerInstall = false;
                            if (IsStoppingServer) {
                                ImGui::TextColored(ImVec4(1, 1, 0, 1), "Server is stopping....");
                            }
                            else if (!IsServerReady) {
                                ImGui::TextColored(ImVec4(0, 1, 1, 1), "Server is starting...");
                                ImGui::BeginDisabled();
                                ImGui::Button("Close server");
                                ImGui::EndDisabled();
                            }
                            else {
                                if (ImGui::Button("Close server")) {
                                    SendCommandToServer("stop\n");
                                    IsStoppingServer = true;
                                }
                                ImGui::Text("Server is running!");
                                ImGui::Text("Send command to server:");
                                ImGui::SameLine();
                                if (ImGui::InputText("##cmd", ServerCommand, IM_ARRAYSIZE(ServerCommand), ImGuiInputTextFlags_EnterReturnsTrue))
                                {
                                    if (strcmp(ServerCommand, "stop") != 0)
                                    {
                                        std::string SlashN = "\n";
                                        SendCommandToServer(ServerCommand + SlashN);
                                        ShowCloseServerError = false;
                                    }
                                    else
                                    {
                                        ShowCloseServerError = true;
                                    }
                                    strcpy(ServerCommand, "");
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Send"))
                                {
                                    SendCommandToServer(ServerCommand);
                                    strcpy(ServerCommand, "");
                                }
                                if (ShowCloseServerError)
                                {
                                    ImGui::Text("Please use the close server button!");
                                }
                            }
                        }
                        
                        if (Options.empty())
                        {
                            CIniReader RAMini(ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\" + "RAM.ini");
                            MinimumServerRAM = RAMini.ReadInteger("Server RAM", "MinimumRAM", 1);
                            MaximumServerRAM = RAMini.ReadInteger("Server RAM", "MaximumRAM", 2);
                            Unit2SelectedIndex = RAMini.ReadInteger("Server RAM", "SavedUnit", 2);
                            std::ifstream Opt(ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\" + "server.properties");
                            if (Opt.is_open())
                            {
                                std::string CLine = "";
                                while (std::getline(Opt, CLine))
                                {
                                    Options.push_back(CLine);
                                }
                                Opt.close();
                            }
                            std::string delimiter = "=";
                            for (auto& CurrOption : Options)
                            {
                                size_t pos = CurrOption.find(delimiter);
                                if (pos != std::string::npos)
                                {
                                    std::string ActualOption = CurrOption.substr(0, pos);
                                    std::string Value = CurrOption.substr(pos + delimiter.length());
                                    ServerOptions.push_back(ActualOption);
                                    ServerOptionValues.push_back(Value);
                                }
                            }
                            for (int i = 0; i < ServerOptions.size(); i++)
                            {
                                if (ServerOptions[i] == "allow-flight")
                                {
                                    NonNamedOptions.push_back(ServerOptions[i]);
                                    NamedOptions.push_back("Allow Flight:");
                                    NamedOptionValues.push_back(ServerOptionValues[i]);
                                }
                                if (ServerOptions[i] == "difficulty")
                                {
                                    NonNamedOptions.push_back(ServerOptions[i]);
                                    NamedOptions.push_back("Difficulty:");
                                    NamedOptionValues.push_back(ServerOptionValues[i]);
                                }
                                if (ServerOptions[i] == "gamemode")
                                {
                                    NonNamedOptions.push_back(ServerOptions[i]);
                                    NamedOptions.push_back("Game Mode:");
                                    NamedOptionValues.push_back(ServerOptionValues[i]);
                                }
                                if (ServerOptions[i] == "hardcore")
                                {
                                    NonNamedOptions.push_back(ServerOptions[i]);
                                    NamedOptions.push_back("Hardcore:");
                                    NamedOptionValues.push_back(ServerOptionValues[i]);
                                }
                                if (ServerOptions[i] == "max-players")
                                {
                                    NonNamedOptions.push_back(ServerOptions[i]);
                                    NamedOptions.push_back("Maximum allowed players:");
                                    NamedOptionValues.push_back(ServerOptionValues[i]);
                                }
                                if (ServerOptions[i] == "motd")
                                {
                                    NonNamedOptions.push_back(ServerOptions[i]);
                                    NamedOptions.push_back("Description:");
                                    NamedOptionValues.push_back(ServerOptionValues[i]);
                                }
                                if (ServerOptions[i] == "online-mode")
                                {
                                    NonNamedOptions.push_back(ServerOptions[i]);
                                    NamedOptions.push_back("Allow non-premium:");
                                    NamedOptionValues.push_back(ServerOptionValues[i]);
                                }
                                if (ServerOptions[i] == "server-ip")
                                {
                                    NonNamedOptions.push_back(ServerOptions[i]);
                                    NamedOptions.push_back("Server IP:");
                                    NamedOptionValues.push_back(ServerOptionValues[i]);
                                }
                                if (ServerOptions[i] == "spawn-protection")
                                {
                                    NonNamedOptions.push_back(ServerOptions[i]);
                                    NamedOptions.push_back("Spawn block protection:");
                                    NamedOptionValues.push_back(ServerOptionValues[i]);
                                }
                            }
                        }
                        if (!Options.empty())
                        {
                            if (ImGui::CollapsingHeader("Options"))
                            {
                                for (int i = 0; i < NamedOptions.size(); i++)
                                {
                                    ImGui::Text("%s", NamedOptions[i].c_str());
                                    ImGui::SameLine();
                                    char buf[260];
                                    strcpy(buf, NamedOptionValues[i].c_str());
                                    if (NamedOptionValues[i].size() < 10)
                                    {
                                        ImGui::SetNextItemWidth(100);
                                    }
                                    else
                                    {
                                        ImGui::SetNextItemWidth(NamedOptionValues[i].size() * 10);
                                    }
                                    if (ImGui::InputText(("##" + to_string(i)).c_str(), buf, IM_ARRAYSIZE(buf)))
                                    {
                                        NamedOptionValues[i] = buf;
                                    }
                                }
                                ImGui::Text("Minimum Server RAM:");
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(80);
                                ImGui::InputInt("##y", &MinimumServerRAM);
                                ImGui::SameLine();
                                ImGui::Text("Maximum Server RAM:");
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(80);
                                ImGui::InputInt("##w", &MaximumServerRAM);
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(90);
                                if (ImGui::BeginCombo("##Combo6", Unit2[Unit2SelectedIndex].c_str()))
                                {
                                    for (int i = 0; i < Unit2.size(); ++i)
                                    {
                                        const bool isSelected = (Unit2SelectedIndex == i);
                                        if (ImGui::Selectable(Unit2[i].c_str(), isSelected))
                                        {
                                            Unit2SelectedIndex = i;
                                        }
                                        if (isSelected)
                                        {
                                            ImGui::SetItemDefaultFocus();
                                        }
                                    }
                                    ImGui::EndCombo();
                                }
                                if (ImGui::Button("Save"))
                                {
                                    if (MinimumServerRAM > MaximumServerRAM)
                                    {
                                        ShowServerRAMError = true;
                                    }
                                    else
                                    {
                                        ShowServerRAMError = false;
                                    }
                                    if (!ShowServerRAMError)
                                    {
                                        CIniReader RAMini(ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\" + "RAM.ini");
                                        RAMini.WriteInteger("Server RAM", "MinimumRAM", MinimumServerRAM);
                                        RAMini.WriteInteger("Server RAM", "MaximumRAM", MaximumServerRAM);
                                        RAMini.WriteInteger("Server RAM", "SavedUnit", Unit2SelectedIndex);
                                        std::string propertiesPath = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\server.properties";
                                        std::vector<std::string> newFileContent;

                                        for (const std::string& line : Options)
                                        {
                                            size_t pos = line.find('=');
                                            if (pos != std::string::npos)
                                            {
                                                std::string key = line.substr(0, pos);
                                                bool found = false;
                                                for (int i = 0; i < NonNamedOptions.size(); i++)
                                                {
                                                    if (key == NonNamedOptions[i])
                                                    {
                                                        newFileContent.push_back(key + "=" + NamedOptionValues[i]);
                                                        found = true;
                                                        break;
                                                    }
                                                }

                                                if (!found) newFileContent.push_back(line);
                                            }
                                            else
                                            {
                                                newFileContent.push_back(line);
                                            }
                                        }
                                        std::ofstream outFile(propertiesPath, std::ios::trunc);
                                        if (outFile.is_open())
                                        {
                                            for (const auto& newLine : newFileContent)
                                            {
                                                outFile << newLine << std::endl;
                                            }
                                            outFile.close();
                                            Options = newFileContent;
                                        }
                                    }
                                }
                                if (ShowServerRAMError)
                                {
                                    ImGui::Text("Minimum RAM can't be more than the Maximum RAM!");
                                }
                            }
                        }
                        if (ImGui::CollapsingHeader("Mods##Server"))
                        {
                            if (ImGui::Button("Add Mods##Server"))
                            {
                                if (!FolderPick)
                                {
                                    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
                                    if (SUCCEEDED(hr))
                                    {
                                        CComPtr<IFileOpenDialog> pFileDlg;
                                        hr = pFileDlg.CoCreateInstance(CLSID_FileOpenDialog);

                                        if (SUCCEEDED(hr))
                                        {
                                            DWORD dwFlags;
                                            pFileDlg->GetOptions(&dwFlags);
                                            pFileDlg->SetOptions(dwFlags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);

                                            COMDLG_FILTERSPEC fileTypes[] = { { L"Java Mod Files", L"*.jar" } };
                                            pFileDlg->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);

                                            hr = pFileDlg->Show(NULL);
                                            if (SUCCEEDED(hr))
                                            {
                                                CComPtr<IShellItemArray> pResults;
                                                hr = pFileDlg->GetResults(&pResults);

                                                if (SUCCEEDED(hr))
                                                {
                                                    DWORD dwCount = 0;
                                                    pResults->GetCount(&dwCount);

                                                    for (DWORD i = 0; i < dwCount; i++)
                                                    {
                                                        CComPtr<IShellItem> pItem;
                                                        pResults->GetItemAt(i, &pItem);

                                                        LPWSTR pszFilePath = NULL;
                                                        pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);

                                                        if (pszFilePath)
                                                        {
                                                            std::wstring fullPath = pszFilePath;
                                                            std::wstring filename = fs::path(fullPath).filename().wstring();
                                                            std::wstring destination = utf8_to_wstring(ServerModsDir + "\\") + filename;
                                                            std::wstring DisabledDest = utf8_to_wstring(ServerModsDir + "\\Disabled Mods\\") + filename;
                                                            if (!PathFileExistsW(destination.c_str()) && !PathFileExistsW(DisabledDest.c_str()))
                                                            {
                                                                CopyFileW(fullPath.c_str(), destination.c_str(), FALSE);
                                                            }
                                                            CoTaskMemFree(pszFilePath);
                                                        }
                                                    }
                                                    UpdateServerModsList = true;
                                                }
                                            }
                                        }
                                        CoUninitialize();
                                    }
                                }
                                else
                                {
                                    struct ComInit {
                                        ComInit() { CoInitialize(nullptr); }
                                        ~ComInit() { CoUninitialize(); }
                                    };
                                    ComInit com;
                                    CComPtr<IFileOpenDialog> pFolderDlg;
                                    HRESULT hr = CoCreateInstance(
                                        CLSID_FileOpenDialog,
                                        NULL,
                                        CLSCTX_ALL,
                                        IID_IFileOpenDialog,
                                        (void**)&pFolderDlg
                                    );

                                    if (SUCCEEDED(hr)) {
                                        DWORD options;
                                        pFolderDlg->GetOptions(&options);
                                        pFolderDlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                                        hr = pFolderDlg->Show(NULL);

                                        if (SUCCEEDED(hr)) {
                                            CComPtr<IShellItem> pItem;
                                            hr = pFolderDlg->GetResult(&pItem);

                                            if (SUCCEEDED(hr)) {
                                                LPWSTR path;
                                                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);

                                                if (SUCCEEDED(hr)) {
                                                    std::wstring PickedFolder = path;
                                                    std::vector<std::wstring> FilesToCopy = getInDirectoryW(PickedFolder, false);
                                                    for (std::wstring CurrentFile : FilesToCopy)
                                                    {
                                                        if (!PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW(((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + CurrentFile).c_str())))
                                                        {
                                                            CopyFileW(
                                                                (PickedFolder + L"\\" + CurrentFile).c_str(),
                                                                (utf8_to_wstring(ServerModsDir) + L"\\" + CurrentFile).c_str(), true);
                                                        }
                                                    }
                                                    UpdateServerModsList = true;
                                                    CoTaskMemFree(path);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            ImGui::SameLine();
                            ImGui::Checkbox("Select per folder##Server", &ServerFolderPick);
                            if (ImGui::Button("Open Modrinth Page##Server"))
                            {
                                std::string url = "https://modrinth.com/discover/mods?g=categories:fabric&v=" + ServerUsedVersions[SelectedIndex];
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Open CurseForge Page##Server"))
                            {
                                std::string baselink = "https://www.curseforge.com/minecraft/search?page=1&pageSize=20&sortBy=relevancy";
                                std::string url = baselink + "&version=" + ServerUsedVersions[SelectedIndex] + "&gameVersionTypeId=4";
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            if (!ServerModListAndEnabled.empty())
                            {
                                if (ImGui::Button("Remove all mods##Server"))
                                {
                                    for (int i = 0; i < ServerModListAndEnabled.size(); i++)
                                    {
                                        if (ServerModListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ServerModsDir) + L"\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                    }
                                    ServerModList.clear();
                                    ServerDisabledModList.clear();
                                    ServerModListAndEnabled.clear();
                                    ServerDisplayModListAndEnabled.clear();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Disable all mods##Server"))
                                {
                                    for (int i = 0; i < ServerModListAndEnabled.size(); i++)
                                    {
                                        ServerModListAndEnabled[i].second = false;
                                        if (PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\" + ServerModListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileW((utf8_to_wstring(ServerModsDir) + L"\\" + ServerModListAndEnabled[i].first).c_str(), (utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str());
                                        }
                                    }
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Enable all mods##Server"))
                                {
                                    for (int i = 0; i < ServerModListAndEnabled.size(); i++)
                                    {
                                        ServerModListAndEnabled[i].second = true;
                                        if (PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str(), (utf8_to_wstring(ServerModsDir) + L"\\" + ServerModListAndEnabled[i].first).c_str());
                                        }
                                    }
                                }
                                ImGui::Text("Mod list:");
                                ImGui::Separator();
                                for (int i = 0; i < ServerModListAndEnabled.size(); i++)
                                {
                                    ImGui::TextWrapped("%s", wstring_to_utf8(ServerDisplayModListAndEnabled[i].first).c_str());
                                    ImGui::SameLine();
                                    if (ImGui::Checkbox(("Enabled##Server" + to_string(i)).c_str(), reinterpret_cast<bool*>(&ServerModListAndEnabled[i].second)))
                                    {
                                        if (!ServerModListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                MoveFileW((utf8_to_wstring(ServerModsDir) + L"\\" + ServerModListAndEnabled[i].first).c_str(), (utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                MoveFileW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str(), (utf8_to_wstring(ServerModsDir) + L"\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button(("Remove##Server" + to_string(i)).c_str()))
                                    {
                                        if (ModListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ServerModsDir) + L"\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        ServerModListAndEnabled.erase(ServerModListAndEnabled.begin() + i);
                                        ServerDisplayModListAndEnabled.erase(ServerDisplayModListAndEnabled.begin() + i);
                                        if (i != 0)
                                        {
                                            i--;
                                        }
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button(("Details##Server" + to_string(i)).c_str()))
                                    {
                                        ImGui::OpenPopup(("DetailsPopup##Server" + to_string(i)).c_str());
                                    }
                                    if (ImGui::BeginPopup(("DetailsPopup##Server" + to_string(i)).c_str()))
                                    {
                                        ImGui::Text("Description: %s##Server", wstring_to_utf8(ServerModDescriptions[i]).c_str());
                                        ImGui::Text("Version: %s##Server", wstring_to_utf8(ServerModVersions[i]).c_str());
                                        ImGui::Text("Author(s): %s##Server", wstring_to_utf8(ServerModAuthors[i]).c_str());
                                        ImGui::EndPopup();
                                    }
                                    ImGui::Separator();
                                }
                            }
                        }
                        if (ImGui::CollapsingHeader("Resource Packs##Server"))
                        {
                            if (ImGui::Button("Add Resource Pack##Server"))
                            {
                                if (!ServerResourcePackFolderPick)
                                {
                                    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
                                    if (SUCCEEDED(hr))
                                    {
                                        CComPtr<IFileOpenDialog> pFileDlg;
                                        hr = pFileDlg.CoCreateInstance(CLSID_FileOpenDialog);

                                        if (SUCCEEDED(hr))
                                        {
                                            DWORD dwFlags;
                                            pFileDlg->GetOptions(&dwFlags);
                                            pFileDlg->SetOptions(dwFlags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);

                                            COMDLG_FILTERSPEC fileTypes[] = { { L"Resource Pack Files", L"*.zip" } };
                                            pFileDlg->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);

                                            hr = pFileDlg->Show(NULL);
                                            if (SUCCEEDED(hr))
                                            {
                                                CComPtr<IShellItemArray> pResults;
                                                hr = pFileDlg->GetResults(&pResults);

                                                if (SUCCEEDED(hr))
                                                {
                                                    DWORD dwCount = 0;
                                                    pResults->GetCount(&dwCount);

                                                    for (DWORD i = 0; i < dwCount; i++)
                                                    {
                                                        CComPtr<IShellItem> pItem;
                                                        pResults->GetItemAt(i, &pItem);

                                                        LPWSTR pszFilePath = NULL;
                                                        pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);

                                                        if (pszFilePath)
                                                        {
                                                            std::wstring fullPath = pszFilePath;
                                                            std::wstring filename = fs::path(fullPath).filename().wstring();
                                                            std::wstring destination = utf8_to_wstring(ServerResourcePackDir + "\\") + filename;
                                                            std::wstring DisabledDest = utf8_to_wstring(ServerResourcePackDir + "\\Disabled Resource Packs\\") + filename;
                                                            if (!PathFileExistsW(destination.c_str()) && !PathFileExistsW(DisabledDest.c_str()))
                                                            {
                                                                CopyFileW(fullPath.c_str(), destination.c_str(), FALSE);
                                                            }
                                                            CoTaskMemFree(pszFilePath);
                                                        }
                                                    }
                                                    UpdateServerResourcePackList = true;
                                                }
                                            }
                                        }
                                        CoUninitialize();
                                    }
                                }
                                else
                                {
                                    struct ComInit {
                                        ComInit() { CoInitialize(nullptr); }
                                        ~ComInit() { CoUninitialize(); }
                                    };
                                    ComInit com;
                                    CComPtr<IFileOpenDialog> pFolderDlg;
                                    HRESULT hr = CoCreateInstance(
                                        CLSID_FileOpenDialog,
                                        NULL,
                                        CLSCTX_ALL,
                                        IID_IFileOpenDialog,
                                        (void**)&pFolderDlg
                                    );

                                    if (SUCCEEDED(hr)) {
                                        DWORD options;
                                        pFolderDlg->GetOptions(&options);
                                        pFolderDlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                                        hr = pFolderDlg->Show(NULL);

                                        if (SUCCEEDED(hr)) {
                                            CComPtr<IShellItem> pItem;
                                            hr = pFolderDlg->GetResult(&pItem);

                                            if (SUCCEEDED(hr)) {
                                                LPWSTR path;
                                                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);

                                                if (SUCCEEDED(hr)) {
                                                    std::wstring PickedFolder = path;
                                                    std::vector<std::wstring> FilesToCopy = getInDirectoryW(PickedFolder, false);
                                                    for (std::wstring CurrentFile : FilesToCopy)
                                                    {
                                                        if (!PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentFile).c_str()) && !PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + CurrentFile).c_str()))
                                                        {
                                                            CopyFileW(
                                                                (PickedFolder + L"\\" + CurrentFile).c_str(),
                                                                (utf8_to_wstring(ServerResourcePackDir) + L"\\" + CurrentFile).c_str(), true);
                                                        }
                                                    }
                                                    UpdateServerResourcePackList = true;
                                                    CoTaskMemFree(path);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            ImGui::SameLine();
                            ImGui::Checkbox("Select per folder##ServerResourcePack", &ServerResourcePackFolderPick);
                            if (ImGui::Button("Open Modrinth Page##ServerResourcePack"))
                            {
                                std::string url = "https://modrinth.com/discover/resourcepacks?v=" + ServerUsedVersions[SelectedIndex];
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Open CurseForge Page##ServerResourcePack"))
                            {
                                std::string url = "https://www.curseforge.com/minecraft/search?page=1&pageSize=20&sortBy=relevancy&class=texture-packs&version=" + ServerUsedVersions[SelectedIndex];
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            if (!ServerResourcePackListAndEnabled.empty())
                            {
                                if (ImGui::Button("Remove all##ServerResource Packs"))
                                {
                                    for (int i = 0; i < ServerResourcePackListAndEnabled.size(); i++)
                                    {
                                        if (ServerResourcePackListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + ServerResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + ServerResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + ServerResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + ServerResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                    }
                                    ServerResourcePackList.clear();
                                    ServerDisplayResourcePackListAndEnabled.clear();
                                    ServerResourcePackListAndEnabled.clear();
                                    ServerDisabledResourcePackList.clear();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Disable all##Resource Packs"))
                                {
                                    for (int i = 0; i < ServerResourcePackListAndEnabled.size(); i++)
                                    {
                                        ServerResourcePackListAndEnabled[i].second = false;
                                        if (PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + ServerResourcePackListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + ServerResourcePackListAndEnabled[i].first).c_str(), (utf8_to_wstring(ResourcePackDir) + L"\\Disabled Resource Packs\\" + ResourcePackListAndEnabled[i].first).c_str());
                                        }
                                    }
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Enable all##Resource Packs"))
                                {
                                    for (int i = 0; i < ServerResourcePackListAndEnabled.size(); i++)
                                    {
                                        ServerResourcePackListAndEnabled[i].second = true;
                                        if (PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + ServerResourcePackListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + ServerResourcePackListAndEnabled[i].first).c_str(), (utf8_to_wstring(ServerResourcePackDir) + L"\\" + ServerResourcePackListAndEnabled[i].first).c_str());
                                        }
                                    }
                                }
                                ImGui::Text("Resource Pack List:");
                                ImGui::Separator();
                                for (int i = 0; i < ServerResourcePackListAndEnabled.size(); i++)
                                {
                                    ImGui::TextWrapped("%s", wstring_to_utf8(ServerDisplayResourcePackListAndEnabled[i].first).c_str());
                                    ImGui::SameLine();
                                    if (ImGui::Checkbox(("Enabled##ResourcePack" + to_string(i)).c_str(), reinterpret_cast<bool*>(&ServerResourcePackListAndEnabled[i].second)))
                                    {
                                        if (!ServerResourcePackListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + ServerResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                MoveFileW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + ServerResourcePackListAndEnabled[i].first).c_str(), (utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + ServerResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + ServerResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                MoveFileW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + ServerResourcePackListAndEnabled[i].first).c_str(), (utf8_to_wstring(ServerResourcePackDir) + L"\\" + ServerResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button(("Remove##ResourcePack" + to_string(i)).c_str()))
                                    {
                                        if (ServerResourcePackListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + ServerResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ServerResourcePackDir) + L"\\" + ServerResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + ServerResourcePackListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileW((utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\" + ServerResourcePackListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        ServerResourcePackListAndEnabled.erase(ServerResourcePackListAndEnabled.begin() + i);
                                        ServerDisplayResourcePackListAndEnabled.erase(ServerDisplayResourcePackListAndEnabled.begin() + i);
                                        if (i != 0)
                                        {
                                            i--;
                                        }
                                    }
                                    ImGui::Separator();
                                }
                            }
                        }
                        if (UpdateServerModsList)
                        {
                            if (!ServerList.empty())
                            {
                                ServerDisplayModListAndEnabled.clear();
                                ServerModAuthors.clear();
                                ServerModDescriptions.clear();
                                ServerModVersions.clear();
                                bit7z::Bit7zLibrary lib{ "7z.dll" };
                                std::vector<bit7z::byte_t> ModBuffer;
                                ServerModsDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods";
                                ServerModList = getInDirectoryW(utf8_to_wstring(ServerModsDir), false);
                                for (int i = 0; i < ServerModList.size(); i++)
                                {
                                    if (fs::path(utf8_to_wstring(ServerModsDir) + L"\\" + ServerModList[i]).extension() == ".jar")
                                    {
                                        std::wstring Name = L"";
                                        std::wstring Description = L"";
                                        std::wstring Version = L"";
                                        std::vector<std::string> AuthorStr;
                                        std::string AuthorNJ = "";
                                        json Author = json::array();
                                        bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                                        extractor.extractMatching(ServerModsDir + "\\" + wstring_to_utf8(ServerModList[i]), "fabric.mod.json", ModBuffer);
                                        if (!ModBuffer.empty())
                                        {
                                            try
                                            {
                                                json mod = json::parse(ModBuffer);
                                                Name = utf8_to_wstring(mod.value("name", ""));
                                                Description = utf8_to_wstring(mod.value("description", ""));
                                                Version = utf8_to_wstring(mod.value("version", ""));
                                                if (mod.contains("authors") && mod["authors"].is_array()) {
                                                    for (auto& item : mod["authors"]) {
                                                        if (item.is_string()) {
                                                            AuthorStr.push_back(item.get<std::string>());
                                                        }
                                                        else if (item.is_object() && item.contains("name")) {
                                                            AuthorStr.push_back(item["name"].get<std::string>());
                                                        }
                                                    }
                                                }
                                                for (size_t i = 0; i < AuthorStr.size(); ++i) {
                                                    AuthorNJ += AuthorStr[i];
                                                    if (i < AuthorStr.size() - 1) {
                                                        AuthorNJ += ", ";
                                                    }
                                                }
                                            }
                                            catch (json::exception& except)
                                            {
                                                WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ServerModList[i]));
                                            }
                                        }
                                        if (Name != L"")
                                        {
                                            ServerDisplayModListAndEnabled.push_back(make_pair(Name, true));
                                        }
                                        else
                                        {
                                            ServerDisplayModListAndEnabled.push_back(make_pair(ServerModList[i], true));
                                        }
                                        ServerModAuthors.push_back(utf8_to_wstring(AuthorNJ));
                                        ServerModDescriptions.push_back(Description);
                                        ServerModVersions.push_back(Version);
                                        ServerModListAndEnabled.push_back(make_pair(ServerModList[i], true));
                                    }
                                }
                                ServerDisabledModList = getInDirectoryW(utf8_to_wstring(ServerModsDir) + L"\\Disabled Mods\\", false);
                                for (int i = 0; i < ServerDisabledModList.size(); i++)
                                {
                                    std::wstring Name = L"";
                                    std::wstring Description = L"";
                                    std::wstring Version = L"";
                                    std::vector<std::string> AuthorStr;
                                    std::string AuthorNJ = "";
                                    json Author = json::array();
                                    bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                                    extractor.extractMatching(ServerModsDir + "\\Disabled Mods\\" + wstring_to_utf8(ServerDisabledModList[i]), "fabric.mod.json", ModBuffer);
                                    if (!ModBuffer.empty())
                                    {
                                        try
                                        {
                                            json mod = json::parse(ModBuffer);
                                            Name = utf8_to_wstring(mod.value("name", ""));
                                            Description = utf8_to_wstring(mod.value("description", ""));
                                            Version = utf8_to_wstring(mod.value("version", ""));
                                            if (mod.contains("authors") && mod["authors"].is_array()) {
                                                for (auto& item : mod["authors"]) {
                                                    if (item.is_string()) {
                                                        AuthorStr.push_back(item.get<std::string>());
                                                    }
                                                    else if (item.is_object() && item.contains("name")) {
                                                        AuthorStr.push_back(item["name"].get<std::string>());
                                                    }
                                                }
                                            }
                                            for (size_t i = 0; i < AuthorStr.size(); ++i) {
                                                AuthorNJ += AuthorStr[i];
                                                if (i < AuthorStr.size() - 1) {
                                                    AuthorNJ += ", ";
                                                }
                                            }
                                        }
                                        catch (json::exception& except)
                                        {
                                            WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ServerDisabledModList[i]));
                                        }
                                    }
                                    if (Name != L"")
                                    {
                                        ServerDisplayModListAndEnabled.push_back(make_pair(Name, false));
                                    }
                                    else
                                    {
                                        ServerDisplayModListAndEnabled.push_back(make_pair(ServerDisabledModList[i], false));
                                    }
                                    ServerModAuthors.push_back(utf8_to_wstring(AuthorNJ));
                                    ServerModDescriptions.push_back(Description);
                                    ServerModVersions.push_back(Version);
                                    ServerModListAndEnabled.push_back(make_pair(DisabledModList[i], false));
                                }
                                std::sort(ServerDisplayModListAndEnabled.begin(), ServerDisplayModListAndEnabled.end());
                            }
                            UpdateServerModsList = false;
                        }
                        if (UpdateServerResourcePackList)
                        {
                            ServerResourcePackListAndEnabled.clear();
                            ServerDisplayResourcePackListAndEnabled.clear();
                            ServerDisabledResourcePackList.clear();
                            if (!ServerList.empty())
                            {
                                bit7z::Bit7zLibrary lib{ "7z.dll" };
                                ServerResourcePackList = getInDirectoryW(utf8_to_wstring(ServerResourcePackDir), false);
                                std::vector<bit7z::byte_t> ResourcePackBuffer;
                                for (int i = 0; i < ServerResourcePackList.size(); i++)
                                {
                                    std::wstring Name = L"";
                                    if (fs::path(ServerResourcePackList[i]).extension() == ".zip")
                                    {
                                        bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                                        try
                                        {
                                            extractor.extractMatching(ServerResourcePackDir + "\\" + wstring_to_utf8(ServerResourcePackList[i]), "pack.mcmeta", ResourcePackBuffer);
                                        }
                                        catch (const bit7z::BitException& except)
                                        {
                                            WriteToLog(except.what());
                                        }
                                        if (!ResourcePackBuffer.empty())
                                        {
                                            try
                                            {
                                                json resourcepack = json::parse(ResourcePackBuffer);
                                                Name = utf8_to_wstring(resourcepack["pack"].value("name", ""));
                                            }
                                            catch (json::exception& except)
                                            {
                                                WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ServerResourcePackList[i]));
                                            }
                                        }
                                        if (Name != L"")
                                        {
                                            ServerDisplayResourcePackListAndEnabled.push_back(make_pair(Name, true));
                                        }
                                        else
                                        {
                                            ServerDisplayResourcePackListAndEnabled.push_back(make_pair(ServerResourcePackList[i], true));
                                        }
                                        ServerResourcePackListAndEnabled.push_back(make_pair(ServerResourcePackList[i], true));
                                    }
                                }
                                ServerDisabledResourcePackList = getInDirectoryW(utf8_to_wstring(ServerResourcePackDir) + L"\\Disabled Resource Packs\\", false);
                                for (int i = 0; i < ServerDisabledResourcePackList.size(); i++)
                                {
                                    std::wstring Name = L"";
                                    if (fs::path(ServerDisabledResourcePackList[i]).extension() == ".zip")
                                    {
                                        bit7z::BitFileExtractor extractor{ lib, bit7z::BitFormat::Zip };
                                        try
                                        {
                                            extractor.extractMatching(ServerResourcePackDir + "\\Disabled Resource Packs\\" + wstring_to_utf8(ServerDisabledResourcePackList[i]), "pack.mcmeta", ResourcePackBuffer);
                                        }
                                        catch (bit7z::BitException& except)
                                        {
                                            WriteToLog(except.what());
                                        }
                                        if (!ResourcePackBuffer.empty())
                                        {
                                            try
                                            {
                                                json resourcepack = json::parse(ResourcePackBuffer);
                                                Name = utf8_to_wstring(resourcepack["pack"].value("name", ""));
                                            }
                                            catch (json::exception& except)
                                            {
                                                WriteToLog(except.what() + std::string(" At File: ") + wstring_to_utf8(ServerDisabledResourcePackList[i]));
                                            }
                                        }
                                        if (Name != L"")
                                        {
                                            ServerDisplayResourcePackListAndEnabled.push_back(make_pair(Name, false));
                                        }
                                        else
                                        {
                                            ServerDisplayResourcePackListAndEnabled.push_back(make_pair(ServerDisabledResourcePackList[i], false));
                                        }
                                        ServerResourcePackListAndEnabled.push_back(make_pair(DisabledResourcePackList[i], false));
                                    }
                                }
                                std::sort(ServerDisplayResourcePackListAndEnabled.begin(), ServerDisplayResourcePackListAndEnabled.end());
                            }
                            UpdateServerResourcePackList = false;
                        }
                        if (DisableServerSelection)
                        {
                            ImGui::EndDisabled();
                        }
                    }
                    else
                    {
                        ImGui::Text("No servers found!");
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Settings"))
                {
                    if (ImGui::Checkbox("Enable logs", &EnableLog))
                    {
                        iniReader.WriteBoolean("Settings", "EnableLogs", EnableLog);
                        if (PathFileExistsW((utf8_to_wstring(StrCurrentDirectory) + L"\\MMM.log").c_str()))
                        {
                            DeleteFileW((utf8_to_wstring(StrCurrentDirectory) + L"\\MMM.log").c_str());
                        }
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            
        }
        ImGui::End();
    }

#ifdef _WINDLL
    if (GetAsyncKeyState(VK_INSERT) & 1)
        bDraw = !bDraw;
#endif
}