#include <Winsock2.h>
#include <afxdlgs.h>
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
using json = nlohmann::json;
using namespace std;
namespace fs = std::filesystem;
LPCSTR Drawing::lpWindowName = "Minecraft Mod Manager";
CIniReader iniReader("mods.ini");
float x = iniReader.ReadFloat("Settings", "WindowXSize", 500);
float y = iniReader.ReadFloat("Settings", "WindowYSize", 500);
ImVec2 Drawing::vWindowSize = ImVec2(x, y);
ImGuiWindowFlags Drawing::WindowFlags = 0;
bool Drawing::bDraw = true;
bool UpdateTags = false;
void Drawing::Active() { bDraw = true; }
bool Drawing::isActive() { return bDraw; }
std::string wstring_to_utf8(const std::wstring& wstr) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(wstr);
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
bool UpdateServerModsList = false;
bool ShowSuccess = false;
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
HANDLE Execute(const std::string& cmdLine, const std::string& RunFrom, bool Silent, bool WaitTillFinish, bool CloseTheHandle)
{
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&g_hChildStd_IN_Rd, &g_hChildStd_IN_Wr, &saAttr, 0)) return NULL;

    SetHandleInformation(g_hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { 0 };
    si.cb = sizeof(si);
    si.hStdInput = g_hChildStd_IN_Rd;
    si.dwFlags |= STARTF_USESTDHANDLES;

    DWORD dwCreationFlags = 0;

    if (Silent)
    {
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        dwCreationFlags |= CREATE_NO_WINDOW;
    }
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &saAttr, 0);
    SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0);

    si.hStdOutput = g_hChildStd_OUT_Wr;
    si.hStdError = g_hChildStd_OUT_Wr; 

    char* cmdCStr = _strdup(cmdLine.c_str());

    BOOL success = CreateProcessA(
        nullptr,
        cmdCStr,
        NULL,
        NULL,
        TRUE,
        dwCreationFlags,
        NULL,
        RunFrom.c_str(),
        &si,
        &pi);

    free(cmdCStr);

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
std::vector<std::wstring> GetSelectedFiles(const wchar_t* buffer)
{
    std::vector<std::wstring> result;
    if (!buffer || *buffer == L'\0') return result;

    std::wstring_view dir(buffer);
    const wchar_t* p = buffer + dir.length() + 1;

    if (*p == L'\0')
    {
        result.push_back(std::wstring(dir));
        return result;
    }

    result.reserve(100);

    while (*p != L'\0')
    {
        std::wstring_view filename(p);

        std::wstring fullPath;
        fullPath.reserve(dir.length() + filename.length() + 1);
        fullPath.append(dir).append(L"\\").append(filename);

        result.push_back(std::move(fullPath));

        p += filename.length() + 1;
    }

    return result;
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
const char* comboPreviewValue = "";
std::vector<chrono::system_clock::time_point> parsedTime;
chrono::system_clock::time_point CurrentTime;
int MostRecentDate = 0;
bool Remove = false;
bool Rename = false;
bool Success = false;
bool ShowRAMError = false;
bool ShowCloseServerError = false;
bool ShowServerInstall = false;
bool DisableServerSelection = false;
bool DisableServerInstalling = false;
bool ServerReload = false;
char Namebuf[260] = "";
std::string MostRecentVersion = "";
std::string MostRecentProfile = "";
std::vector<std::string> ServerModList;
std::vector<std::string> ProfileNames;
std::vector<std::string> ProfilesWithVersions;
std::vector<std::string> UsedDates;
std::vector<std::string> NonModifiedUsedDates;
std::vector<std::string> UsedVersions;
std::vector<std::string> UsedProfiles;
std::vector<std::pair<std::string, int>> ModListAndEnabled;
std::vector<std::pair<std::string, int>> ServerModListAndEnabled;
std::string ServerModsDir = "";
std::vector<std::string> ServerModFileList;
std::vector<std::string> ServerDisabledModFileList;
std::vector<std::string> ModFileList;
std::vector<std::string> gameDirs;
std::vector<std::string> DisabledMods;
std::vector<std::string> CustomTags;
std::vector<std::string> ModLoader;
std::vector<std::string> ProfileIds;
std::vector<std::string> UsedProfileIds;
std::vector<std::string> FilesToCopy;
std::vector<std::string> ServerList;
std::vector<std::string> ServerVersions;
std::vector<std::string> FabricServerVersions;
std::vector<std::string> EulaLines;
std::string MostRecentFabricLoaderVersion;
std::string modsDir = "";
std::string ServerDir = "";
std::wstring DirToScanW = L"";
std::vector<std::string> FabricInstallerVersions;
std::string MostRecentFabricInstallerVersion = "";
std::ifstream tags("Tags.txt");
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
bool InitName = true;
bool ShowCloseButton = false;
std::vector<bool> EnableTextInput;
std::vector<bool> ServerEnableTextInput;
std::wstring string_to_wstring_winapi(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.length(), NULL, 0);
    if (len == 0) return L"";

    std::wstring ws(len, L' ');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.length(), &ws[0], len);
    return ws;
}
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
std::string PickedFolder = "";
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
char CurrentFolder[MAX_PATH];
char ServerName[260] = "";
int MinimumRAM = 1;
int MaximumRAM = 2;
std::vector<std::string> Unit = { "KB", "MB", "GB" };
std::vector<std::string> Unit2 = { "KB", "MB", "GB" };
int Unit2SelectedIndex = 2;
bool ClearIsEnabled = false;
std::string StrCurrentDirectory = "";
std::string profilesDir = "";
std::string CurrentProfileDir = "";
bool ServerInit = true;
bool NameAlreadyExists = false;
bool ShowNameChange = false;
bool ShowChangeServerNameButton = true;
bool RewriteProfiles = false;
bool Result = false;
bool UpdateLastUsed = false;
std::vector<std::string> DisabledModList;
void Drawing::Draw() {
    if (DoOnce)
    {
        ModFileList.clear();
        DisabledModList.clear();
        EnableTextInput.clear();
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
        ModFileList.clear();
        FormattedDates.clear();
        Times.clear();
        SelectedIndex = 0;
        PreSelectedIndex = 0;
        MostRecentProfile.clear();
        MostRecentDate = 0;
        parsedTime.clear();
        comboPreviewValue = "";
        DirToScanW.clear();
        std::string tag;
        if (tags.is_open())
        {
            while (std::getline(tags, tag))
            {
                CustomTags.push_back(tag);
            }
            tags.close();
        }
        GetCurrentDirectoryA(MAX_PATH, CurrentFolder);
        StrCurrentDirectory = CurrentFolder;
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
                for (const std::string& CurrentTag : CustomTags)
                {
                    if (!lastVersion.empty() &&
                        lastVersion.find(CurrentTag) != std::string::npos &&
                        CurrentTag != "")
                    {
                        UsedProfileIds.push_back(id);
                        ModLoader.push_back(CurrentTag);
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
                if (!PathFileExistsA((profilesDir + "\\" + CurrentProfile + "\\shaderpacks").c_str()))
                {
                    fs::create_directory(profilesDir + "\\" + CurrentProfile + "\\shaderpacks");
                }
            }
            comboPreviewValue = MostRecentProfile.c_str();
            if (!UsedProfiles.empty())
            {
                modsDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\mods";
            }
            else
            {
                modsDir = minecraftDir + "\\mods";
            }
            ModFileList = getInDirectoryA(modsDir, false);
            for (int i = 0; i < ModFileList.size(); i++)
            {
                ModListAndEnabled.push_back(make_pair(ModFileList[i], true));
            }
            DisabledModList = getInDirectoryA(modsDir + "\\Disabled Mods\\", false);
            for (int i = 0; i < DisabledModList.size(); i++)
            {
                ModListAndEnabled.push_back(make_pair(DisabledModList[i], false));
            }
            for (int i = 0; i < ModListAndEnabled.size(); i++)
            {
                if (EnableTextInput.size() != ModListAndEnabled.size())
                {
                    EnableTextInput.push_back(false);
                }
            }
            std::sort(ModListAndEnabled.begin(), ModListAndEnabled.end());
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
        ServerList.clear();
        ServerList = getInDirectoryA(ServerDir, true);
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
        Options.clear();
        ServerOptions.clear();
        ServerOptionValues.clear();
        NamedOptions.clear();
        NamedOptionValues.clear();
        MinimumRAM = 1;
        MaximumRAM = 2;
        Unit2SelectedIndex = 2;
        ShowNameChange = false;
        ShowChangeServerNameButton = true;
        ServerHandle = nullptr;
        g_hChildStd_IN_Wr = nullptr;
        g_hChildStd_IN_Rd = nullptr;
        IsStoppingServer = false;
        ShowCloseButton = false;
        ServerReload = false;
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
            if (!PathFileExistsA((ServerDir + "\\" + ServerList[i] + "\\config").c_str()))
            {
                fs::create_directory(ServerDir + "\\" + ServerList[i] + "\\config");
            }
        }
        if (!ServerList.empty())
        {
            ServerModsDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods";
            ServerModFileList = getInDirectoryA(ServerModsDir, false);
            for (int i = 0; i < ServerModFileList.size(); i++)
            {
                ServerModListAndEnabled.push_back(make_pair(ServerModFileList[i], true));
            }
            ServerDisabledModFileList = getInDirectoryA(ServerModsDir + "\\Disabled Mods\\", false);
            for (int i = 0; i < ServerDisabledModFileList.size(); i++)
            {
                ServerModListAndEnabled.push_back(make_pair(ServerDisabledModFileList[i], false));
            }
            for (int i = 0; i < ServerModListAndEnabled.size(); i++)
            {
                if (ServerEnableTextInput.size() != ServerModListAndEnabled.size())
                {
                    ServerEnableTextInput.push_back(false);
                }
            }
            std::sort(ServerModListAndEnabled.begin(), ServerModListAndEnabled.end());
            for (int i = 0; i < ServerList.size(); i++)
            {
                if (!fs::exists(ServerDir + "\\" + ServerList[i] + "\\mods\\"))
                {
                    fs::create_directory(ServerDir + "\\" + ServerList[i] + "\\mods\\");
                }
                if (!fs::exists(ServerDir + "\\" + ServerList[i] + "\\mods\\Disabled Mods\\"))
                {
                    fs::create_directory(ServerDir + "\\" + ServerList[i] + "\\mods\\Disabled Mods\\");
                }
            }
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
                        DoOnce = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Open Fabric Installer (downloads it if it doesn't exist)"))
                    {
                        if (!PathFileExistsA("index.html"))
                        {
                            Execute("wget https://maven.fabricmc.net/net/fabricmc/fabric-installer/", StrCurrentDirectory, true, true, true);
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
                                Execute("wget https://maven.fabricmc.net/net/fabricmc/fabric-installer/" + VersionToInstall + "/fabric-installer-" + VersionToInstall + ".exe", StrCurrentDirectory, true, true, true);
                            }
                            Execute((FabricInstaller).c_str(), StrCurrentDirectory, false, false, true);
                        }
                        DeleteFileA(".wget-hsts");
                        DeleteFileA("index.html");
                    }
                    if (!UsedProfiles.empty())
                    {
                        ImGui::Text("Current Modded Profile:");
                        ImGui::SameLine();
                        if (ImGui::BeginCombo("##Combo", comboPreviewValue)) {
                            for (int i = 0; i < ProfilesWithVersions.size(); ++i)
                            {
                                const bool isSelected = (SelectedIndex == i);
                                if (ImGui::Selectable(ProfilesWithVersions[i].c_str(), isSelected))
                                {
                                    SelectedIndex = i;
                                    comboPreviewValue = ProfilesWithVersions[SelectedIndex].c_str();
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
                            UpdateLastUsed = true;
                            PreSelectedIndex = SelectedIndex;
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
                                                            std::wstring destination = string_to_wstring_winapi(modsDir + "\\") + filename;
                                                            std::wstring DisabledDest = string_to_wstring_winapi(modsDir + "\\Disabled Mods\\") + filename;
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
                                                    PickedFolder = wstring_to_utf8(path);
                                                    FilesToCopy = getInDirectoryA(PickedFolder, false);
                                                    for (std::string CurrentFile : FilesToCopy)
                                                    {
                                                        if (!PathFileExistsA((modsDir + "\\" + CurrentFile).c_str()))
                                                        {
                                                            CopyFileA(
                                                                (PickedFolder + "\\" + CurrentFile).c_str(),
                                                                (modsDir + "\\" + CurrentFile).c_str(), true);
                                                        }
                                                    }
                                                    UpdateModsList = true;
                                                    CoTaskMemFree(path);
                                                }
                                            }
                                        }
                                    }
                                }
                                
                                UpdateModsList = true;
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
                                std::string baselink = "https://www.curseforge.com/minecraft/search?page=1&pageSize=20&sortBy=relevancy";
                                std::string url = baselink + "&version=" + UsedVersions[SelectedIndex] + "&gameVersionTypeId=" + to_string(gameVersionTypeId);
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                            
                            if (!ModListAndEnabled.empty())
                            {
                                if (ImGui::Button("Remove all mods"))
                                {
                                    for (int i = 0; i < ModListAndEnabled.size(); i++)
                                    {
                                        if (ModListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsA((modsDir + "\\" + ModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileA((modsDir + "\\" + ModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsA((modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileA((modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                    }
                                    ModFileList.clear();
                                    ModListAndEnabled.clear();
                                    DisabledModList.clear();
                                    EnableTextInput.clear();
                                    UpdateLastUsed = true;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Disable all mods"))
                                {
                                    for (int i = 0; i < ModListAndEnabled.size(); i++)
                                    {
                                        ModListAndEnabled[i].second = false;
                                        if (PathFileExistsA((modsDir + "\\" + ModListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileA((modsDir + "\\" + ModListAndEnabled[i].first).c_str(), (modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str());
                                        }
                                    } 
                                    UpdateLastUsed = true;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Enable all mods"))
                                {
                                    for (int i = 0; i < ModListAndEnabled.size(); i++)
                                    {
                                        ModListAndEnabled[i].second = true;
                                        if (PathFileExistsA((modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileA((modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str(), (modsDir + "\\" + ModListAndEnabled[i].first).c_str());
                                        }
                                    }
                                    UpdateLastUsed = true;
                                }
                                ImGui::Text("Mod List:");
                                ImGui::Separator();
                                for (int i = 0; i < ModListAndEnabled.size(); i++)
                                {
                                    ImGui::Text("%s", ModListAndEnabled[i].first.c_str());
                                    ImGui::SameLine();
                                    if (ImGui::Checkbox(("Enabled##" + to_string(i)).c_str(), reinterpret_cast<bool*>(&ModListAndEnabled[i].second)))
                                    {
                                            if (!ModListAndEnabled[i].second)
                                            {
                                                if (PathFileExistsA((modsDir + "\\" + ModListAndEnabled[i].first).c_str()))
                                                {
                                                    MoveFileA((modsDir + "\\" + ModListAndEnabled[i].first).c_str(), (modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str());
                                                }
                                            }
                                            else
                                            {
                                                if (PathFileExistsA((modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str()))
                                                {
                                                    MoveFileA((modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str(), (modsDir + "\\" + ModListAndEnabled[i].first).c_str());
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
                                            if (PathFileExistsA((modsDir + "\\" + ModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileA((modsDir + "\\" + ModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsA((modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileA((modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        ModListAndEnabled.erase(ModListAndEnabled.begin() + i);
                                        EnableTextInput.erase(EnableTextInput.begin() + i);
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button(("Rename##" + to_string(i)).c_str()))
                                    {
                                        EnableTextInput[i] = true;
                                    }
                                    if (EnableTextInput[i] == true)
                                    {
                                        std::string CustomNameInit = { ModListAndEnabled[i].first.c_str() };
                                        int pos = CustomNameInit.find(".jar");
                                        std::string NoExtensionName = CustomNameInit;
                                        if (pos != std::string::npos)
                                        {
                                            NoExtensionName = CustomNameInit.substr(0, pos);
                                        }
                                        char CustomName[260];
                                        for (int i = 0; i < 260; i++)
                                        {
                                            CustomName[i] = NULL;
                                        }
                                        for (int i = 0; i < NoExtensionName.length(); i++)
                                        {
                                            CustomName[i] = NoExtensionName[i];
                                        }
                                        if (ImGui::InputText((".jar##" + to_string(i)).c_str(), CustomName, IM_ARRAYSIZE(CustomName), ImGuiInputTextFlags_EnterReturnsTrue))
                                        {
                                            if (ModListAndEnabled[i].second)
                                            {
                                                MoveFileA((modsDir + "\\" + ModListAndEnabled[i].first).c_str(), (modsDir + "\\" + CustomName + ".jar").c_str());
                                            }
                                            else
                                            {
                                                MoveFileA((modsDir + "\\Disabled Mods\\" + ModListAndEnabled[i].first).c_str(), (modsDir + "\\DisabledMods\\" + CustomName + ".jar").c_str());
                                            }
                                            ModListAndEnabled[i].first = CustomName + std::string(".jar");
                                            EnableTextInput[i] = false;
                                        }
                                    }
                                    ImGui::Separator();
                                }
                            }
                            if (UpdateLastUsed)
                            {
                                auto CurrentTime = std::chrono::system_clock::now();
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
                            if (UpdateModsList)
                            {
                                ModFileList.clear();
                                ModListAndEnabled.clear();
                                DisabledModList.clear();
                                EnableTextInput.clear();
                                modsDir = profilesDir + "\\" + UsedProfiles[SelectedIndex] + "\\mods";
                                ModFileList = getInDirectoryA(modsDir, false);
                                for (int i = 0; i < ModFileList.size(); i++)
                                {
                                    ModListAndEnabled.push_back(make_pair(ModFileList[i], true));
                                }
                                DisabledModList = getInDirectoryA(modsDir + "\\Disabled Mods\\", false);
                                for (int i = 0; i < DisabledModList.size(); i++)
                                {
                                    ModListAndEnabled.push_back(make_pair(DisabledModList[i], false));
                                }
                                for (int i = 0; i < ModListAndEnabled.size(); i++)
                                {
                                    if (EnableTextInput.size() != ModListAndEnabled.size())
                                    {
                                        EnableTextInput.push_back(false);
                                    }
                                }
                                std::sort(ModListAndEnabled.begin(), ModListAndEnabled.end());
                                UpdateModsList = false;
                            }
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
                    if (ImGui::Button("Reload servers") || ServerReload)
                    {
                        ServerList.clear();
                        ServerList = getInDirectoryA(ServerDir, true);
                        ServerModFileList.clear();
                        ServerModsDir = "";
                        ServerModListAndEnabled.clear();
                        ServerEnableTextInput.clear();
                        ServerDisabledModFileList.clear();
                        if (!ServerList.empty())
                        {
                            ServerModsDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods";
                            ServerModFileList = getInDirectoryA(ServerModsDir, false);
                            for (int i = 0; i < ServerModFileList.size(); i++)
                            {
                                ServerModListAndEnabled.push_back(make_pair(ServerModFileList[i], true));
                            }
                            ServerDisabledModFileList = getInDirectoryA(ServerModsDir + "\\Disabled Mods\\", false);
                            for (int i = 0; i < ServerDisabledModFileList.size(); i++)
                            {
                                ServerModListAndEnabled.push_back(make_pair(ServerDisabledModFileList[i], false));
                            }
                            for (int i = 0; i < ServerModListAndEnabled.size(); i++)
                            {
                                if (ServerEnableTextInput.size() != ServerModListAndEnabled.size())
                                {
                                    ServerEnableTextInput.push_back(false);
                                }
                            }
                            std::sort(ServerModListAndEnabled.begin(), ServerModListAndEnabled.end());
                        }
                        ServerReload = false;
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
                                Execute("wget https://meta.fabricmc.net/v2/versions/loader", StrCurrentDirectory, true, true, true);
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
                                    Execute("wget https://meta.fabricmc.net/v2/versions", StrCurrentDirectory, true, true, true);
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
                                        DeleteFileA((StrCurrentDirectory + "\\" + ".wget-hsts").c_str());
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
                            ImGui::SetNextItemWidth(1200);
                            ImGui::InputText("##a", ServerName, IM_ARRAYSIZE(ServerName));
                            ImGui::Text("Minimum Server RAM:");
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(300);
                            ImGui::InputInt("##b", &MinimumRAM);
                            ImGui::SameLine();
                            ImGui::Text("Maximum Server RAM:");
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(300);
                            ImGui::InputInt("##c", &MaximumRAM);
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(200);
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
                                        Execute("wget https://meta.fabricmc.net/v2/versions/installer", StrCurrentDirectory, true, true, true);
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
                                            DeleteFileA((StrCurrentDirectory + "\\" + ".wget-hsts").c_str());
                                        }
                                    }
                                    if (!PathFileExistsA((ServerDir + "\\" + ServerName).c_str()))
                                    {
                                        fs::create_directory((ServerDir + "\\" + ServerName).c_str());
                                    }
                                    if (!PathFileExistsA((ServerDir + "\\" + ServerName + "\\" + "jar").c_str()))
                                    {
                                        Execute("wget --directory-prefix " + ServerDir + "\\" + ServerName + " https://meta.fabricmc.net/v2/versions/loader/" + FabricServerVersions[ServerVersionsSelectedIndex] + "/" + MostRecentFabricLoaderVersion + "/" + MostRecentFabricInstallerVersion + "/server/jar", StrCurrentDirectory, true, true, true);
                                    }
                                    if (PathFileExistsA((ServerDir + "\\" + ServerName + "\\" + "jar").c_str()) &&
                                        !PathFileExistsA((ServerDir + "\\" + ServerName + "\\" + "server.jar").c_str()))
                                    {
                                        MoveFileA((ServerDir + "\\" + ServerName + "\\" + "jar").c_str(), (ServerDir + "\\" + ServerName + "\\" + "server.jar").c_str());
                                    }
                                    if (PathFileExistsA((ServerDir + "\\" + ServerName + "\\" + "server.jar").c_str()))
                                    {
                                        std::string SelectedUnit = Unit[UnitSelectedIndex].substr(0, 1);
                                        std::string passtoshell = "java -jar -Xms" + to_string(MinimumRAM) + SelectedUnit + " -Xmx" + to_string(MaximumRAM) + SelectedUnit + " " + ServerDir + "\\" + ServerName + "\\" + "server.jar nogui";
                                        ServerHandle = Execute(passtoshell.c_str(), ServerDir + "\\" + ServerName, true, false, false);
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
                        ServerReload = true;
                    }
                    if (ShowSuccess)
                    {
                        ImGui::Text("Server successfully installed!");
                    }
                    if (PreServerSelectedIndex != ServerSelectedIndex)
                    {
                        if (!fs::exists(ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods\\"))
                        {
                            fs::create_directory(ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods\\");
                        }
                        if (!fs::exists(ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods\\Disabled Mods\\"))
                        {
                            fs::create_directory(ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods\\Disabled Mods\\");
                        }
                        ShowDeletionConfirmation = false;
                        ServerModsDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods";
                        iniReader.WriteString("Server", "Most Recent Server", ServerList[ServerSelectedIndex]);
                        UpdateServerModsList = true;
                        Options.clear();
                        ServerOptions.clear();
                        ServerOptionValues.clear();
                        NamedOptions.clear();
                        NamedOptionValues.clear();
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
                        if (ImGui::BeginCombo("##Combo5", ServerList[ServerSelectedIndex].c_str())) {
                            for (int i = 0; i < ServerList.size(); ++i)
                            {
                                const bool isSelected = (ServerSelectedIndex == i);
                                if (ImGui::Selectable(ServerList[i].c_str(), isSelected))
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
                                    ServerList = getInDirectoryA(ServerDir, true);
                                    for (int i = 0; i < ServerList.size(); i++)
                                    {
                                        if (ServerList[i] == Namebuf)
                                        {
                                            ServerSelectedIndex = i;
                                            break;
                                        }
                                    }
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
                                std::string passtoshell = "java -jar -Xms" + to_string(MinimumServerRAM) + SelectedUnit + " -Xmx" + to_string(MaximumServerRAM) + SelectedUnit + " " + ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\" + "server.jar nogui";
                                ServerHandle = Execute(passtoshell.c_str(), ServerDir + "\\" + ServerList[ServerSelectedIndex], true, false, false);
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
                                            ServerReload = true;
                                            if (ServerSelectedIndex != 0)
                                            {
                                                ServerSelectedIndex = ServerSelectedIndex - 1;
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
                                    if (ServerCommand != "stop")
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
                                    ImGui::SetNextItemWidth(1200);
                                    char buf[260];
                                    strcpy(buf, NamedOptionValues[i].c_str());
                                    if (ImGui::InputText(("##" + to_string(i)).c_str(), buf, IM_ARRAYSIZE(buf)))
                                    {
                                        NamedOptionValues[i] = buf;
                                    }
                                }
                                ImGui::Text("Minimum Server RAM:");
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(300);
                                ImGui::InputInt("##y", &MinimumServerRAM);
                                ImGui::SameLine();
                                ImGui::Text("Maximum Server RAM:");
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(300);
                                ImGui::InputInt("##w", &MaximumServerRAM);
                                ImGui::SameLine();
                                ImGui::SetNextItemWidth(200);
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
                                                            std::wstring destination = string_to_wstring_winapi(ServerModsDir + "\\") + filename;
                                                            std::wstring DisabledDest = string_to_wstring_winapi(ServerModsDir + "\\Disabled Mods\\") + filename;
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
                                                    PickedFolder = wstring_to_utf8(path);
                                                    FilesToCopy = getInDirectoryA(PickedFolder, false);
                                                    for (std::string CurrentFile : FilesToCopy)
                                                    {
                                                        if (!PathFileExistsA((modsDir + "\\" + CurrentFile).c_str()))
                                                        {
                                                            CopyFileA(
                                                                (PickedFolder + "\\" + CurrentFile).c_str(),
                                                                (modsDir + "\\" + CurrentFile).c_str(), true);
                                                        }
                                                    }
                                                    UpdateServerModsList = true;
                                                    CoTaskMemFree(path);
                                                }
                                            }
                                        }
                                    }
                                }

                                UpdateServerModsList = true;
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
                                std::string baselink = "https://www.curseforge.com/minecraft/search?page=1&pageSize=20&sortBy=relevancy";
                                std::string url = baselink + "&version=" + UsedVersions[SelectedIndex] + "&gameVersionTypeId=" + to_string(gameVersionTypeId);
                                ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }

                            if (!ServerModListAndEnabled.empty())
                            {
                                if (ImGui::Button("Remove all mods"))
                                {
                                    for (int i = 0; i < ServerModListAndEnabled.size(); i++)
                                    {
                                        if (ServerModListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsA((ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileA((ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsA((ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileA((ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                    }
                                    ServerModFileList.clear();
                                    ServerDisabledModFileList.clear();
                                    ServerModListAndEnabled.clear();
                                    ServerEnableTextInput.clear();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Disable all mods"))
                                {
                                    for (int i = 0; i < ServerModListAndEnabled.size(); i++)
                                    {
                                        ServerModListAndEnabled[i].second = false;
                                        if (PathFileExistsA((ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileA((ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str(), (ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str());
                                        }
                                    }
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Enable all mods"))
                                {
                                    for (int i = 0; i < ModListAndEnabled.size(); i++)
                                    {
                                        ModListAndEnabled[i].second = true;
                                        if (PathFileExistsA((ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str()))
                                        {
                                            MoveFileA((ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str(), (ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str());
                                        }
                                    }
                                }
                                ImGui::Text("Mod List:");
                                ImGui::Separator();
                                for (int i = 0; i < ServerModListAndEnabled.size(); i++)
                                {
                                    ImGui::Text("%s", ServerModListAndEnabled[i].first.c_str());
                                    ImGui::SameLine();
                                    if (ImGui::Checkbox(("Enabled##" + to_string(i)).c_str(), reinterpret_cast<bool*>(&ServerModListAndEnabled[i].second)))
                                    {
                                        if (!ServerModListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsA((ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                MoveFileA((ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str(), (ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsA((ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                MoveFileA((ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str(), (ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button(("Remove##" + to_string(i)).c_str()))
                                    {
                                        if (ServerModListAndEnabled[i].second)
                                        {
                                            if (PathFileExistsA((ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileA((ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        else
                                        {
                                            if (PathFileExistsA((ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str()))
                                            {
                                                DeleteFileA((ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str());
                                            }
                                        }
                                        ServerModListAndEnabled.erase(ServerModListAndEnabled.begin() + i);
                                        ServerEnableTextInput.erase(ServerEnableTextInput.begin() + i);
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button(("Rename##" + to_string(i)).c_str()))
                                    {
                                        ServerEnableTextInput[i] = true;
                                    }
                                    if (ServerEnableTextInput[i] == true)
                                    {
                                        std::string CustomNameInit = { ServerModListAndEnabled[i].first.c_str() };
                                        int pos = CustomNameInit.find(".jar");
                                        std::string NoExtensionName = CustomNameInit;
                                        if (pos != std::string::npos)
                                        {
                                            NoExtensionName = CustomNameInit.substr(0, pos);
                                        }
                                        char CustomName[260];
                                        for (int i = 0; i < 260; i++)
                                        {
                                            CustomName[i] = NULL;
                                        }
                                        for (int i = 0; i < NoExtensionName.length(); i++)
                                        {
                                            CustomName[i] = NoExtensionName[i];
                                        }
                                        if (ImGui::InputText((".jar##" + to_string(i)).c_str(), CustomName, IM_ARRAYSIZE(CustomName), ImGuiInputTextFlags_EnterReturnsTrue))
                                        {
                                            if (ServerModListAndEnabled[i].second)
                                            {
                                                MoveFileA((ServerModsDir + "\\" + ServerModListAndEnabled[i].first).c_str(), (ServerModsDir + "\\" + CustomName + ".jar").c_str());
                                            }
                                            else
                                            {
                                                MoveFileA((ServerModsDir + "\\Disabled Mods\\" + ServerModListAndEnabled[i].first).c_str(), (ServerModsDir + "\\DisabledMods\\" + CustomName + ".jar").c_str());
                                            }
                                            ServerModListAndEnabled[i].first = CustomName + std::string(".jar");
                                            ServerEnableTextInput[i] = false;
                                        }
                                    }
                                    ImGui::Separator();
                                }
                            }
                            if (UpdateServerModsList)
                            {
                                ServerModFileList.clear();
                                ServerModListAndEnabled.clear();
                                ServerDisabledModFileList.clear();
                                ServerEnableTextInput.clear();
                                ServerModsDir = ServerDir + "\\" + ServerList[ServerSelectedIndex] + "\\mods";
                                ServerModFileList = getInDirectoryA(ServerModsDir, false);
                                for (int i = 0; i < ServerModFileList.size(); i++)
                                {
                                    ServerModListAndEnabled.push_back(make_pair(ServerModFileList[i], true));
                                }
                                ServerDisabledModFileList = getInDirectoryA(ServerModsDir + "\\Disabled Mods\\", false);
                                for (int i = 0; i < ServerDisabledModFileList.size(); i++)
                                {
                                    ServerModListAndEnabled.push_back(make_pair(ServerDisabledModFileList[i], false));
                                }
                                for (int i = 0; i < ServerModListAndEnabled.size(); i++)
                                {
                                    if (ServerEnableTextInput.size() != ServerModListAndEnabled.size())
                                    {
                                        ServerEnableTextInput.push_back(false);
                                    }
                                }
                                std::sort(ServerModListAndEnabled.begin(), ServerModListAndEnabled.end());
                                UpdateServerModsList = false;
                            }
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
                    if (ImGui::CollapsingHeader("Tags"))
                    {
                        ImGui::Text("Tags are used to recognize the profiles with modded versions and their mod loaders.");
                        ImGui::Text("Usually the tags \"fabric\" and \"forge\" are good enough but you might have another mod loader installed.");
                        ImGui::Text("This might help you with that :)");
                        for (int i = 0; i < CustomTags.size(); i++)
                        {
                            char CustomTag2[260];
                            std::string CurrentTag = { CustomTags[i].c_str() };
                            for (int i = 0; i < 260; i++)
                            {
                                CustomTag2[i] = NULL;
                            }
                            for (int i = 0; i < CurrentTag.length(); i++)
                            {
                                CustomTag2[i] = CurrentTag[i];
                            }
                            if (ImGui::InputText(("##" + to_string(i)).c_str(), CustomTag2, IM_ARRAYSIZE(CustomTag2)))
                            {
                                CustomTags[i] = CustomTag2;
                                UpdateTags = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button(("Remove##" + to_string(i)).c_str()))
                            {
                                CustomTags.erase(CustomTags.begin() + i);
                                UpdateTags = true;
                            }
                        }
                        if (ImGui::Button("Add Tag"))
                        {
                            CustomTags.push_back("");
                        }
                        if (UpdateTags)
                        {
                            std::ofstream outtags("Tags.txt", std::ios::trunc);
                            if (outtags.is_open())
                            {
                                for (std::string tag : CustomTags)
                                {
                                    if (tag != "")
                                    {
                                        outtags << tag << std::endl;
                                    }
                                }
                                outtags.close();
                            }
                            UpdateTags = false;
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