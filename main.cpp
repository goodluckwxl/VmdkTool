#define _CRT_SECURE_NO_WARNINGS 
#include "Core.h"
#include "resource.h"
#include <windows.h>
#include <winioctl.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <shellapi.h>
#include <VersionHelpers.h>
#include <cstdarg>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <atomic>
#include <thread>
#include <mutex>
#include <sstream>
#include <functional>
#include <fstream>
#include <set>
#include <algorithm>
#include <cwctype>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef IDC_BTN_EXPORT 
#define IDC_BTN_EXPORT          1075 
#endif 

#define IDM_ACCEL_SEARCH_FOCUS  2001 
#define IDM_ACCEL_SEARCH_START  2002 
#define IDM_ACCEL_OPEN_VMDK     2003 
#define IDM_ACCEL_EXPORT        2004 
#define IDM_ACCEL_CLEAR_LOG     2005 
#define IDM_ACCEL_SELECT_ALL    2006 
#define IDM_ACCEL_CANCEL        2007 

#define IDM_LIST_OPEN_FOLDER    3001 
#define IDM_LIST_COPY_PATH      3002 
#define IDM_LIST_SELECT_ALL     3003 
#define IDM_LIST_DELETE         3004 

#define WM_TRAY_NOTIFY          (WM_APP + 30)
#define WM_APP_MAINT_RESULT     (WM_APP + 40)
#define IDC_TRAY_ICON           1 

#define IDC_GRP_SEARCH          2100 
#define IDC_GRP_STEPS           2101 
#define IDC_GRP_ADVANCED        2102 
#define IDC_TITLE               2103 

HWND g_hEditVmdk, g_hBtnBrowse, g_hBtnSelTool;
HWND g_hToolPath, g_hStaticInfo;
HWND g_hCheckE, g_hCheckR, g_hCheckD, g_hCheckK;
HWND g_hBtnStart, g_hBtnCancel, g_hProgBar;
HWND g_hLogList, g_hBtnClearLog;
HWND g_hBtnDisableLog;
HWND g_hEditSearchDir, g_hBtnBrowseDir, g_hEditPattern, g_hBtnSearch;
HWND g_hListResults, g_hBtnSelAll, g_hBtnSelNone, g_hBtnSelInvert, g_hBtnClearResults;
HWND g_hBtnExport = NULL;

HWND g_hMainWnd = NULL;
HACCEL g_hAccel = NULL;
HINSTANCE g_hInst = NULL;
NOTIFYICONDATAW g_nid = {};
bool g_trayInited = false;

std::wstring g_vdiskmanagerPath;
std::atomic<bool> g_cancel{ false };
std::atomic<bool> g_searching{ false };
std::atomic<bool> g_searchCancel{ false };
std::atomic<int>  g_foundCount{ 0 };
std::atomic<bool> g_toolDetecting{ false };
std::atomic<ULONGLONG> g_totalSavedBytes{ 0 };
std::atomic<int>       g_completedCount{ 0 };
std::mutex g_logMutex;
std::wstring g_iniPath;

int g_snapWarnChoice = 0;

HANDLE g_hLogFile = INVALID_HANDLE_VALUE;

const int MAX_LOG_LINES = 5000;
const int TRIM_LOG_LINES = 1000;

struct AppConfig {
    std::wstring lastVmdkPath;
    std::wstring lastToolPath;
    std::wstring lastSearchDir;
    std::wstring lastPattern;
    int winX = -1, winY = -1;
    bool checkE = true, checkR = true, checkD = true, checkK = true;
    bool autoRelease = false;
};
AppConfig g_cfg;

struct Job {
    std::wstring vmdkPath;
    int steps;
};

struct SearchResult {
    std::wstring path;
    ULONGLONG size;
};

struct ToolLocateResult {
    bool success;
    std::wstring path;
};

struct MaintenanceResult {
    std::wstring path;
    ULONGLONG beforeSize;
    ULONGLONG afterSize;
    bool success;
};

enum class DiskKind { Unknown = 0, HDD, SataSsd, NvmeSsd, Raid, Virtual };

const wchar_t* DiskKindName(DiskKind k) {
    switch (k) {
    case DiskKind::HDD:     return L"机械硬盘 HDD";
    case DiskKind::SataSsd: return L"SATA SSD";
    case DiskKind::NvmeSsd: return L"NVMe M.2 SSD";
    case DiskKind::Raid:    return L"RAID 阵列";
    case DiskKind::Virtual: return L"虚拟磁盘";
    default:                return L"未知";
    }
}

double DiskKindFactor(DiskKind k) {
    switch (k) {
    case DiskKind::NvmeSsd: return 1.0;
    case DiskKind::SataSsd: return 0.6;
    case DiskKind::Raid:    return 0.5;
    case DiskKind::HDD:     return 0.25;
    case DiskKind::Virtual: return 0.8;
    default:                return 0.5;
    }
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring TrimTrailingSlash(const std::wstring& p) {
    if (p.size() <= 3) return p;
    std::wstring r = p;
    while (r.size() > 3 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    return r;
}

std::wstring FormatSize(ULONGLONG size) {
    wchar_t buf[64];
    if (size >= 1024ULL * 1024 * 1024) {
        swprintf_s(buf, L"%.2f GB", size / 1073741824.0);
    }
    else if (size >= 1024ULL * 1024) {
        swprintf_s(buf, L"%.2f MB", size / 1048576.0);
    }
    else if (size >= 1024ULL) {
        swprintf_s(buf, L"%.2f KB", size / 1024.0);
    }
    else {
        swprintf_s(buf, L"%llu B", size);
    }
    return buf;
}

std::wstring ToLowerStr(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = towlower(c);
    return r;
}

bool IsSnapshotDisk(const std::wstring& filePath) {
    size_t slash = filePath.find_last_of(L'\\');
    std::wstring name = (slash == std::wstring::npos) ? filePath : filePath.substr(slash + 1);
    std::wstring lower = name;
    for (auto& c : lower) c = towlower(c);
    if (lower.size() < 10) return false;
    if (lower.substr(lower.size() - 5) != L".vmdk") return false;
    std::wstring stem = lower.substr(0, lower.size() - 5);
    for (size_t i = 0; i + 7 <= stem.size(); i++) {
        if (stem[i] == L'-' && iswdigit(stem[i + 1]) && iswdigit(stem[i + 2]) && iswdigit(stem[i + 3]) && iswdigit(stem[i + 4]) && iswdigit(stem[i + 5]) && iswdigit(stem[i + 6])) {
            if (i + 7 == stem.size() || stem[i + 7] == L'-' || stem[i + 7] == L' ' || stem[i + 7] == L'.') return true;
        }
    }
    return false;
}

bool HasSnapshotDisk(const std::wstring& baseDiskPath) {
    if (IsSnapshotDisk(baseDiskPath)) return false;
    size_t slash = baseDiskPath.find_last_of(L'\\');
    std::wstring dir = (slash == std::wstring::npos) ? L"" : baseDiskPath.substr(0, slash + 1);
    std::wstring name = (slash == std::wstring::npos) ? baseDiskPath : baseDiskPath.substr(slash + 1);
    std::wstring lower = name;
    for (auto& c : lower) c = towlower(c);
    if (lower.size() < 10) return false;
    if (lower.substr(lower.size() - 5) != L".vmdk") return false;
    std::wstring prefix = name.substr(0, name.size() - 5);
    std::wstring lowerPrefix = lower.substr(0, lower.size() - 5);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + prefix + L"-*.vmdk").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring foundLower = fd.cFileName;
        for (auto& c : foundLower) c = towlower(c);
        if (foundLower.size() < lowerPrefix.size() + 1 + 6 + 5) continue;
        if (foundLower.substr(0, lowerPrefix.size()) != lowerPrefix) continue;
        if (foundLower[lowerPrefix.size()] != L'-') continue;
        size_t numStart = lowerPrefix.size() + 1;
        bool allDigits = true;
        for (int i = 0; i < 6; i++) {
            if (numStart + i >= foundLower.size() || !iswdigit(foundLower[numStart + i])) { allDigits = false; break; }
        }
        if (!allDigits) continue;
        if (foundLower.substr(foundLower.size() - 5) != L".vmdk") continue;
        found = true;
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

std::wstring FindLatestSnapshotDisk(const std::wstring& baseDiskPath) {
    if (IsSnapshotDisk(baseDiskPath)) return baseDiskPath;
    size_t slash = baseDiskPath.find_last_of(L'\\');
    std::wstring dir = (slash == std::wstring::npos) ? L"" : baseDiskPath.substr(0, slash + 1);
    std::wstring name = (slash == std::wstring::npos) ? baseDiskPath : baseDiskPath.substr(slash + 1);
    std::wstring lower = name;
    for (auto& c : lower) c = towlower(c);
    if (lower.size() < 10) return L"";
    if (lower.substr(lower.size() - 5) != L".vmdk") return L"";
    std::wstring prefix = name.substr(0, name.size() - 5);
    std::wstring lowerPrefix = lower.substr(0, lower.size() - 5);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + prefix + L"-*.vmdk").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    int maxNum = -1;
    std::wstring latestName;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring foundName = fd.cFileName;
        std::wstring foundLower = foundName;
        for (auto& c : foundLower) c = towlower(c);
        if (foundLower.size() < lowerPrefix.size() + 1 + 6 + 5) continue;
        if (foundLower.substr(0, lowerPrefix.size()) != lowerPrefix) continue;
        if (foundLower[lowerPrefix.size()] != L'-') continue;
        size_t numStart = lowerPrefix.size() + 1;
        int num = 0;
        bool allDigits = true;
        for (int i = 0; i < 6; i++) {
            if (numStart + i >= foundLower.size() || !iswdigit(foundLower[numStart + i])) { allDigits = false; break; }
            num = num * 10 + (foundLower[numStart + i] - L'0');
        }
        if (!allDigits) continue;
        if (foundLower.substr(foundLower.size() - 5) != L".vmdk") continue;
        if (num > maxNum) { maxNum = num; latestName = foundName; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (latestName.empty()) return L"";
    return dir + latestName;
}

void AddLog(const wchar_t* fmt, ...);
void SetUIEnabled(bool enabled);
bool LocateVdiskManager();
void LoadConfig();
void SaveConfig();
bool ExtractResourceToFile(UINT resId, const std::wstring& outPath);
bool ModifyVmxFile(const std::wstring& vmxPath, const std::vector<std::pair<std::wstring, std::wstring>>& keyValues);
void DisableVmLogging();
void AddResultToList(const std::wstring& filePath, ULONGLONG size);
void UpdateMaintenanceResult(const std::wstring& path, ULONGLONG beforeSize, ULONGLONG afterSize, bool success);
void SetListItemText(int row, int subItem, const std::wstring& text);
void UpdateListItemStatus(const std::wstring& filePath, const std::wstring& status);
void SearchThreadProc(std::wstring dir, std::wstring pattern);
void SearchRecursiveAsync(const std::wstring& dir, const std::wstring& pattern, int depth);
void ToolLocateThreadProc();
void InitLogFile();
void CloseLogFile();
void WriteLogFileLine(const std::wstring& text);
void ExportResultsToCsv();
bool IsVmRunning(const std::wstring& vmxPath);
std::wstring GetIniPath();
std::wstring GetGlobalSettingsPath();
std::wstring GetPlayerSettingsPath();
void GetSystemResources(ULONGLONG& totalMemMB, ULONGLONG& availMemMB, DWORD& cpuCount);
int  CalculateMaxConcurrency(const std::vector<Job>& jobs);
void WorkerThread(std::vector<Job> jobs);
void SetProgress(int percent);
ULONGLONG GetFileSizeBytes(const std::wstring& path);
ULONGLONG GetFreeSpaceBytes(const std::wstring& anyPathInside);
ULONGLONG GetVmdkActualSize(const std::wstring& vmdkPath);
DiskKind DetectDiskKind(const std::wstring& path);
void DoSearch();
void DoSelectVmdkFile();
void DoExport();
void DoClearLog();
void DoSelectAll();
void ShowContextMenu(int itemIndex, int screenX, int screenY);
void InitTrayIcon();
void RemoveTrayIcon();
void ShowTrayNotification(const wchar_t* title, const wchar_t* message);
std::wstring ExtractPortableFilesPath();

std::wstring GetIniPath() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    return std::wstring(exe) + L"\\VmdkTool.ini";
}

static std::wstring IniReadString(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024] = { 0 };
    GetPrivateProfileStringW(sec, key, def ? def : L"", buf, 1024, g_iniPath.c_str());
    return buf;
}
static int IniReadInt(const wchar_t* sec, const wchar_t* key, int def) {
    return GetPrivateProfileIntW(sec, key, def, g_iniPath.c_str());
}
static void IniWriteString(const wchar_t* sec, const wchar_t* key, const std::wstring& val) {
    WritePrivateProfileStringW(sec, key, val.c_str(), g_iniPath.c_str());
}
static void IniWriteInt(const wchar_t* sec, const wchar_t* key, int val) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", val);
    WritePrivateProfileStringW(sec, key, buf, g_iniPath.c_str());
}

void LoadConfig() {
    g_cfg.lastVmdkPath = IniReadString(L"Paths", L"Vmdk", L"");
    g_cfg.lastToolPath = IniReadString(L"Paths", L"Tool", L"");
    g_cfg.lastSearchDir = IniReadString(L"Paths", L"SearchDir", L"");
    g_cfg.lastPattern = IniReadString(L"Paths", L"Pattern", L"*.vmdk");
    g_cfg.winX = IniReadInt(L"Window", L"X", -1);
    g_cfg.winY = IniReadInt(L"Window", L"Y", -1);
    g_cfg.checkE = IniReadInt(L"Steps", L"E", 1) != 0;
    g_cfg.checkR = IniReadInt(L"Steps", L"R", 1) != 0;
    g_cfg.checkD = IniReadInt(L"Steps", L"D", 1) != 0;
    g_cfg.checkK = IniReadInt(L"Steps", L"K", 1) != 0;
    g_cfg.autoRelease = IniReadInt(L"Options", L"AutoRelease", 0) != 0;
}

void SaveConfig() {
    IniWriteString(L"Paths", L"Vmdk", g_cfg.lastVmdkPath);
    IniWriteString(L"Paths", L"Tool", g_cfg.lastToolPath);
    IniWriteString(L"Paths", L"SearchDir", g_cfg.lastSearchDir);
    IniWriteString(L"Paths", L"Pattern", g_cfg.lastPattern);
    HWND hMain = g_hMainWnd;
    if (hMain) {
        RECT r; GetWindowRect(hMain, &r);
        g_cfg.winX = r.left; g_cfg.winY = r.top;
    }
    IniWriteInt(L"Window", L"X", g_cfg.winX);
    IniWriteInt(L"Window", L"Y", g_cfg.winY);
    IniWriteInt(L"Steps", L"E", g_cfg.checkE ? 1 : 0);
    IniWriteInt(L"Steps", L"R", g_cfg.checkR ? 1 : 0);
    IniWriteInt(L"Steps", L"D", g_cfg.checkD ? 1 : 0);
    IniWriteInt(L"Steps", L"K", g_cfg.checkK ? 1 : 0);
    IniWriteInt(L"Options", L"AutoRelease", g_cfg.autoRelease ? 1 : 0);
}

void InitLogFile() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    std::wstring logPath = std::wstring(exe) + L"\\VmdkTool.log";
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fad)) {
        ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (size > 2ULL * 1024 * 1024) { std::wstring bakPath = logPath + L".1"; DeleteFileW(bakPath.c_str()); MoveFileW(logPath.c_str(), bakPath.c_str()); }
    }
    g_hLogFile = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_hLogFile == INVALID_HANDLE_VALUE) return;
    DWORD fsize = GetFileSize(g_hLogFile, NULL);
    if (fsize == 0) { const BYTE bom[] = { 0xEF, 0xBB, 0xBF }; DWORD written = 0; WriteFile(g_hLogFile, bom, 3, &written, NULL); }
}

void CloseLogFile() {
    if (g_hLogFile != INVALID_HANDLE_VALUE) { FlushFileBuffers(g_hLogFile); CloseHandle(g_hLogFile); g_hLogFile = INVALID_HANDLE_VALUE; }
}

void WriteLogFileLine(const std::wstring& text) {
    if (g_hLogFile == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t prefix[64]; swprintf_s(prefix, L"[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(prefix) + text;
    if (line.empty() || line.back() != L'\n') line += L"\r\n";
    int u8len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), NULL, 0, NULL, NULL);
    if (u8len <= 0) return;
    std::string utf8(u8len, 0);
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &utf8[0], u8len, NULL, NULL);
    DWORD written = 0; WriteFile(g_hLogFile, utf8.data(), (DWORD)utf8.size(), &written, NULL);
}

void AddLog(const wchar_t* fmt, ...) {
    if (!g_hLogList) return;
    va_list args; va_start(args, fmt);
    wchar_t buf[4096]; StringCchVPrintfW(buf, 4096, fmt, args); va_end(args);
    std::lock_guard<std::mutex> lock(g_logMutex);
    WriteLogFileLine(std::wstring(buf));
    int len = GetWindowTextLengthW(g_hLogList);
    SendMessageW(g_hLogList, EM_SETSEL, len, len);
    SendMessageW(g_hLogList, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageW(g_hLogList, EM_LINESCROLL, 0, 0x7FFFFFFF);
    SendMessageW(g_hLogList, WM_VSCROLL, SB_BOTTOM, 0);
    SendMessageW(g_hLogList, EM_SCROLLCARET, 0, 0);
    int lineCount = (int)SendMessageW(g_hLogList, EM_GETLINECOUNT, 0, 0);
    if (lineCount > MAX_LOG_LINES) {
        int charIdx = (int)SendMessageW(g_hLogList, EM_LINEINDEX, TRIM_LOG_LINES, 0);
        if (charIdx > 0) { SendMessageW(g_hLogList, EM_SETSEL, 0, charIdx); SendMessageW(g_hLogList, EM_REPLACESEL, FALSE, (LPARAM)L""); }
    }
}

DiskKind DetectDiskKind(const std::wstring& path) {
    if (path.size() < 2 || path[1] != L':') return DiskKind::Unknown;
    wchar_t volumePath[8] = { L'\\', L'\\', L'.', L'\\', (wchar_t)towupper(path[0]), L':', 0 };
    HANDLE hDevice = CreateFileW(volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) return DiskKind::Unknown;
    DiskKind kind = DiskKind::Unknown; DWORD bytes = 0;
    STORAGE_PROPERTY_QUERY query = {}; query.PropertyId = StorageDeviceProperty; query.QueryType = PropertyStandardQuery;
    STORAGE_DESCRIPTOR_HEADER header = {};
    if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &header, sizeof(header), &bytes, NULL) && header.Size > 0) {
        std::vector<BYTE> buf(header.Size);
        if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buf.data(), (DWORD)buf.size(), &bytes, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR* desc = (STORAGE_DEVICE_DESCRIPTOR*)buf.data();
            STORAGE_BUS_TYPE bt = desc->BusType;
            if ((int)bt == 0x11) kind = DiskKind::NvmeSsd;
            else if (bt == BusTypeRAID) kind = DiskKind::Raid;
            else if (bt == BusTypeSata || bt == BusTypeAta) kind = DiskKind::SataSsd;
            else if (bt == BusTypeVirtual || bt == BusTypeFileBackedVirtual) kind = DiskKind::Virtual;
        }
    }
    STORAGE_PROPERTY_QUERY seekQuery = {}; seekQuery.PropertyId = StorageDeviceSeekPenaltyProperty; seekQuery.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seekDesc = {};
    if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &seekQuery, sizeof(seekQuery), &seekDesc, sizeof(seekDesc), &bytes, NULL)) {
        if (seekDesc.IncursSeekPenalty) kind = DiskKind::HDD;
        else if (kind == DiskKind::Unknown) kind = DiskKind::SataSsd;
    }
    CloseHandle(hDevice);
    return kind;
}

bool ExtractResourceToFile(UINT resId, const std::wstring& outPath) {
    HRSRC hRsrc = FindResourceW(NULL, MAKEINTRESOURCE(resId), RT_RCDATA);
    if (!hRsrc) return false;
    HGLOBAL hGlobal = LoadResource(NULL, hRsrc);
    if (!hGlobal) return false;
    DWORD dwSize = SizeofResource(NULL, hRsrc);
    LPVOID pData = LockResource(hGlobal);
    if (!pData || dwSize == 0) return false;
    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; WriteFile(hFile, pData, dwSize, &written, NULL); CloseHandle(hFile);
    return true;
}

std::wstring ExtractPortableFilesPath() {
    wchar_t tempDir[MAX_PATH] = { 0 }; GetTempPathW(MAX_PATH, tempDir);
    std::wstring portableDir = std::wstring(tempDir) + L"VmdkToolPortable\\";
    CreateDirectoryW(portableDir.c_str(), NULL);
    bool ok = true;
    ok &= ExtractResourceToFile(IDR_VDISKMGR, portableDir + L"vmware-vdiskmanager.exe");
    ok &= ExtractResourceToFile(IDR_VDISKMGR_BASE, portableDir + L"libeay32.dll");
    ok &= ExtractResourceToFile(IDR_VDISKMGR_CRYPTO, portableDir + L"ssleay32.dll");
    ok &= ExtractResourceToFile(IDR_VDISKMGR_SSL, portableDir + L"zlib1.dll");
    ok &= ExtractResourceToFile(IDR_VMWAREBASE, portableDir + L"vmwarebase.dll");
    if (ok && GetFileAttributesW((portableDir + L"vmware-vdiskmanager.exe").c_str()) != INVALID_FILE_ATTRIBUTES) {
        return portableDir + L"vmware-vdiskmanager.exe";
    }
    return L"";
}

bool LocateVdiskManager() {
    if (g_cfg.autoRelease) {
        g_vdiskmanagerPath = ExtractPortableFilesPath();
        return !g_vdiskmanagerPath.empty();
    }
    g_vdiskmanagerPath = VdiskLocator::Locate();
    if (!g_vdiskmanagerPath.empty()) return true;
    g_vdiskmanagerPath = ExtractPortableFilesPath();
    return !g_vdiskmanagerPath.empty();
}

void ToolLocateThreadProc() {
    g_toolDetecting = true;
    std::wstring path;
    if (g_cfg.autoRelease) {
        AddLog(L"[INFO] 自释放模式已开启，正在释放内置工具...\r\n");
        path = ExtractPortableFilesPath();
        if (!path.empty()) AddLog(L"OK 已释放到：%s\r\n", path.c_str());
    }
    else {
        path = VdiskLocator::Locate();
        if (path.empty()) {
            AddLog(L"[INFO] 系统未安装 VMware，尝试释放内置工具...\r\n");
            path = ExtractPortableFilesPath();
        }
    }
    ToolLocateResult* r = new ToolLocateResult(); r->success = !path.empty(); r->path = path; g_toolDetecting = false;
    if (!PostMessageW(g_hMainWnd, WM_APP + 20, 0, (LPARAM)r)) { delete r; }
}

bool IsVmRunning(const std::wstring& vmxPath) {
    std::wstring lckDir = vmxPath + L".lck";
    DWORD attr = GetFileAttributesW(lckDir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    return false;
}

bool ModifyVmxFile(const std::wstring& vmxPath, const std::vector<std::pair<std::wstring, std::wstring>>& keyValues)
{
    HANDLE hFile = CreateFileW(vmxPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(hFile, NULL);
    std::string utf8(size, 0); DWORD read = 0;
    if (size > 0) ReadFile(hFile, &utf8[0], size, &read, NULL);
    CloseHandle(hFile);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), NULL, 0);
    std::wstring content(wlen, 0);
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &content[0], wlen);
    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find(L'\n', pos);
        std::wstring line = (nl == std::wstring::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    for (auto& kv : keyValues) {
        bool found = false;
        for (auto& line : lines) {
            if (line.empty() || line[0] == L'#') continue;
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = line.substr(0, eq);
            while (!key.empty() && (key.front() == L' ' || key.front() == L'\t')) key.erase(key.begin());
            while (!key.empty() && (key.back() == L' ' || key.back() == L'\t')) key.pop_back();
            if (_wcsicmp(key.c_str(), kv.first.c_str()) == 0) { line = kv.first + L" = \"" + kv.second + L"\""; found = true; break; }
        }
        if (!found) lines.push_back(kv.first + L" = \"" + kv.second + L"\"");
    }
    std::wstring newContent;
    for (auto& line : lines) { newContent += line; newContent += L"\r\n"; }
    std::wstring backupPath = vmxPath + L".bak";
    CopyFileW(vmxPath.c_str(), backupPath.c_str(), FALSE);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, newContent.c_str(), (int)newContent.size(), NULL, 0, NULL, NULL);
    std::string newUtf8(u8len, 0);
    WideCharToMultiByte(CP_UTF8, 0, newContent.c_str(), (int)newContent.size(), &newUtf8[0], u8len, NULL, NULL);
    hFile = CreateFileW(vmxPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; WriteFile(hFile, newUtf8.data(), (DWORD)newUtf8.size(), &written, NULL); CloseHandle(hFile);
    return true;
}

std::wstring GetGlobalSettingsPath() {
    wchar_t programData[MAX_PATH] = { 0 };
    DWORD len = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"C:\\ProgramData\\VMware\\VMware Workstation\\settings.ini";
    return std::wstring(programData) + L"\\VMware\\VMware Workstation\\settings.ini";
}

std::wstring GetPlayerSettingsPath() {
    wchar_t programData[MAX_PATH] = { 0 };
    DWORD len = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"C:\\ProgramData\\VMware\\VMware Player\\settings.ini";
    return std::wstring(programData) + L"\\VMware\\VMware Player\\settings.ini";
}

static void ProcessGlobalSettings(const std::wstring& globalIni, const wchar_t* label) {
    if (GetFileAttributesW(globalIni.c_str()) == INVALID_FILE_ATTRIBUTES) { AddLog(L"[%s] 未找到 %s，跳过。\r\n", label, globalIni.c_str()); return; }
    HANDLE hf = CreateFileW(globalIni.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { AddLog(L"[%s] 无法打开 %s\r\n", label, globalIni.c_str()); return; }
    DWORD sz = GetFileSize(hf, NULL); std::string raw(sz, 0); DWORD rd = 0;
    if (sz > 0) ReadFile(hf, &raw[0], sz, &rd, NULL);
    CloseHandle(hf);
    int wl = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), NULL, 0);
    std::wstring wc(wl, 0);
    if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &wc[0], wl);
    bool hasLogging = false;
    size_t p = 0;
    while (p < wc.size()) {
        size_t nl = wc.find(L'\n', p);
        std::wstring line = (nl == std::wstring::npos) ? wc.substr(p) : wc.substr(p, nl - p);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t s = line.find_first_not_of(L" \t");
        if (s != std::wstring::npos && line.compare(s, 7, L"logging") == 0) { hasLogging = true; break; }
        if (nl == std::wstring::npos) break;
        p = nl + 1;
    }
    if (hasLogging) { AddLog(L"[%s] 已存在 logging 配置，跳过。\r\n", label); return; }
    std::wstring newContent = wc;
    if (!newContent.empty() && newContent.back() != L'\n') newContent += L"\r\n";
    newContent += L"logging = \"FALSE\"\r\n";
    CopyFileW(globalIni.c_str(), (globalIni + L".bak").c_str(), FALSE);
    int u8 = WideCharToMultiByte(CP_UTF8, 0, newContent.c_str(), (int)newContent.size(), NULL, 0, NULL, NULL);
    std::string utf8(u8, 0);
    WideCharToMultiByte(CP_UTF8, 0, newContent.c_str(), (int)newContent.size(), &utf8[0], u8, NULL, NULL);
    HANDLE hw = CreateFileW(globalIni.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hw != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(hw, utf8.data(), (DWORD)utf8.size(), &wr, NULL); CloseHandle(hw); AddLog(L"[%s] OK 已追加 logging = FALSE。\r\n", label); }
}

void DisableVmLogging() {
    AddLog(L"\r\n=== 禁用 VMware 日志操作开始 ===\r\n");
    std::set<std::wstring> dirs;
    int cnt = ListView_GetItemCount(g_hListResults);
    for (int i = 0; i < cnt; i++) {
        if (ListView_GetCheckState(g_hListResults, i)) {
            wchar_t path[MAX_PATH] = { 0 };
            ListView_GetItemText(g_hListResults, i, 0, path, MAX_PATH);
            std::wstring p = path;
            size_t slash = p.find_last_of(L'\\');
            if (slash != std::wstring::npos) dirs.insert(p.substr(0, slash));
        }
    }
    if (dirs.empty()) {
        AddLog(L"未勾选 VMDK，仅执行全局禁用日志（不修改任何 .vmx，不删除日志文件）。\r\n");
        ProcessGlobalSettings(GetGlobalSettingsPath(), L"Workstation");
        ProcessGlobalSettings(GetPlayerSettingsPath(), L"Player");
        AddLog(L"=== 全局禁用日志操作完成 ===\r\n");
        return;
    }
    AddLog(L"已勾选 VMDK，将对同目录 .vmx 禁用日志并删除 *.log，同时修改全局设置。\r\n");
    int totalVmx = 0, totalLog = 0, totalSkip = 0;
    for (auto& dir : dirs) {
        AddLog(L"[目录] %s\r\n", dir.c_str());
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(JoinPath(dir, L"*.vmx").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring vmxPath = JoinPath(dir, fd.cFileName);
                if (IsVmRunning(vmxPath)) { AddLog(L"  [SKIP] VM 正在运行中，跳过: %s\r\n", fd.cFileName); totalSkip++; continue; }
                std::vector<std::pair<std::wstring, std::wstring>> ops = { { L"logging", L"FALSE" } };
                if (ModifyVmxFile(vmxPath, ops)) { AddLog(L"  [OK] 已修改: %s\r\n", fd.cFileName); totalVmx++; }
                else { AddLog(L"  [FAIL] 修改失败: %s\r\n", fd.cFileName); }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        h = FindFirstFileW(JoinPath(dir, L"*.log").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring logPath = JoinPath(dir, fd.cFileName);
                DWORD attr = GetFileAttributesW(logPath.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(logPath.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
                if (DeleteFileW(logPath.c_str())) { AddLog(L"  [OK] 已删除: %s\r\n", fd.cFileName); totalLog++; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    AddLog(L"共修改 %d 个 VMX，删除 %d 个日志，跳过 %d 个运行中的 VM。\r\n", totalVmx, totalLog, totalSkip);
    AddLog(L"\r\n--- 修改全局设置 ---\r\n");
    ProcessGlobalSettings(GetGlobalSettingsPath(), L"Workstation");
    ProcessGlobalSettings(GetPlayerSettingsPath(), L"Player");
    AddLog(L"=== 禁用日志操作完成 ===\r\n");
}

ULONGLONG GetFileSizeBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return 0;
}

ULONGLONG GetFreeSpaceBytes(const std::wstring& anyPathInside) {
    std::wstring dir = anyPathInside;
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);
    ULARGE_INTEGER freeBytesAvailable = { 0 };
    if (GetDiskFreeSpaceExW(dir.c_str(), &freeBytesAvailable, NULL, NULL)) return freeBytesAvailable.QuadPart;
    return 0;
}

ULONGLONG GetVmdkActualSize(const std::wstring& vmdkPath)
{
    HANDLE h = CreateFileW(vmdkPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char head[1024] = { 0 }; DWORD read = 0; ReadFile(h, head, sizeof(head) - 1, &read, NULL);
    bool isDescriptor = (strstr(head, "# Disk DescriptorFile") != NULL);
    if (!isDescriptor) { LARGE_INTEGER fsize; GetFileSizeEx(h, &fsize); CloseHandle(h); return (ULONGLONG)fsize.QuadPart; }
    LARGE_INTEGER fsize; GetFileSizeEx(h, &fsize);
    if (fsize.QuadPart > 1024 * 1024) { CloseHandle(h); return 0; }
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    std::string content; content.reserve((size_t)fsize.QuadPart + 1);
    char buf[4096];
    while (ReadFile(h, buf, sizeof(buf), &read, NULL) && read > 0) content.append(buf, read);
    CloseHandle(h);
    std::wstring dir; size_t slash = vmdkPath.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir = vmdkPath.substr(0, slash + 1);
    ULONGLONG totalBytes = 0; size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start != std::string::npos) {
            std::string trimmed = line.substr(start);
            bool isExtent = (trimmed.compare(0, 2, "RW") == 0) || (trimmed.compare(0, 6, "RDONLY") == 0) || (trimmed.compare(0, 8, "NOACCESS") == 0);
            if (isExtent) {
                std::istringstream iss(trimmed); std::string type, subType, fileName; ULONGLONG sectors = 0;
                iss >> type >> sectors >> subType >> fileName;
                if (fileName.size() >= 2 && fileName.front() == '"' && fileName.back() == '"') fileName = fileName.substr(1, fileName.size() - 2);
                if (sectors > 0) totalBytes += sectors * 512ULL;
                else if (!fileName.empty()) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), -1, NULL, 0);
                    if (wlen > 0) { std::wstring wfileName(wlen - 1, L'\0'); MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), -1, &wfileName[0], wlen); std::wstring fullPath = dir + wfileName; totalBytes += GetFileSizeBytes(fullPath); }
                }
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return totalBytes;
}

void ExportResultsToCsv() {
    int n = ListView_GetItemCount(g_hListResults);
    if (n == 0) { MessageBoxW(g_hMainWnd, L"列表为空，没有可导出的内容。", L"提示", MB_OK | MB_ICONINFORMATION); return; }
    wchar_t file[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.lpstrTitle = L"导出搜索结果"; ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    HANDLE hf = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { MessageBoxW(g_hMainWnd, L"无法创建文件。", L"错误", MB_OK | MB_ICONERROR); return; }
    const BYTE bom[] = { 0xEF, 0xBB, 0xBF }; DWORD written = 0; WriteFile(hf, bom, 3, &written, NULL);
    auto writeUtf8 = [&](const std::wstring& s) {
        if (s.empty()) return;
        int u8len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
        if (u8len <= 0) return;
        std::string u8(u8len, 0); WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], u8len, NULL, NULL);
        DWORD w = 0; WriteFile(hf, u8.data(), (DWORD)u8.size(), &w, NULL);
        };
    auto escapeCsv = [](const std::wstring& s) -> std::wstring {
        bool needQuote = false;
        for (wchar_t c : s) { if (c == L',' || c == L'"' || c == L'\n' || c == L'\r') { needQuote = true; break; } }
        if (!needQuote) return s;
        std::wstring r = L"\"";
        for (wchar_t c : s) { if (c == L'"') r += L"\"\""; else r += c; }
        r += L"\"";
        return r;
        };
    writeUtf8(L"序号,路径,维护前,维护后,节省空间,状态,是否勾选\r\n");
    for (int i = 0; i < n; i++) {
        wchar_t path[MAX_PATH] = { 0 }; wchar_t before[64] = { 0 }; wchar_t after[64] = { 0 }; wchar_t saved[128] = { 0 }; wchar_t status[64] = { 0 };
        ListView_GetItemText(g_hListResults, i, 0, path, MAX_PATH); ListView_GetItemText(g_hListResults, i, 1, before, 64); ListView_GetItemText(g_hListResults, i, 2, after, 64); ListView_GetItemText(g_hListResults, i, 3, saved, 128); ListView_GetItemText(g_hListResults, i, 4, status, 64);
        BOOL checked = ListView_GetCheckState(g_hListResults, i);
        std::wstring line;
        line += std::to_wstring(i + 1); line += L","; line += escapeCsv(path); line += L","; line += before; line += L","; line += after; line += L","; line += escapeCsv(saved); line += L","; line += escapeCsv(status); line += L","; line += (checked ? L"是" : L"否"); line += L"\r\n";
        writeUtf8(line);
    }
    CloseHandle(hf);
    wchar_t msg[600]; swprintf_s(msg, L"已成功导出 %d 条记录到：\n%s", n, file);
    MessageBoxW(g_hMainWnd, msg, L"导出成功", MB_OK | MB_ICONINFORMATION);
    AddLog(L"OK 已导出 %d 条记录到：%s\r\n", n, file);
}void ShowContextMenu(int itemIndex, int screenX, int screenY) {
    if (itemIndex < 0) return;
    wchar_t path[MAX_PATH] = { 0 };
    ListView_GetItemText(g_hListResults, itemIndex, 0, path, MAX_PATH);
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_OPEN_FOLDER, L"打开所在文件夹");
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_COPY_PATH, L"复制路径");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_SELECT_ALL, L"全选列表");
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_DELETE, L"从列表删除");
    SetMenuDefaultItem(hMenu, IDM_LIST_OPEN_FOLDER, FALSE);
    int cmd = (int)TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenX, screenY, 0, g_hMainWnd, NULL);
    DestroyMenu(hMenu);
    switch (cmd) {
    case IDM_LIST_OPEN_FOLDER: {
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { AddLog(L"[WARN] 文件不存在：%s\r\n", path); break; }
        std::wstring fullPath; fullPath.reserve(wcslen(path));
        for (size_t i = 0; i < wcslen(path); i++) {
            if (path[i] == L'\\' && i + 1 < wcslen(path) && path[i + 1] == L'\\') { fullPath += L'\\'; i++; }
            else { fullPath += path[i]; }
        }
        std::wstring folderPath = fullPath;
        size_t slash = folderPath.find_last_of(L'\\');
        if (slash != std::wstring::npos) folderPath = folderPath.substr(0, slash);
        AddLog(L"[INFO] 目标文件：%s\r\n", fullPath.c_str());
        bool success = false;
        {
            PIDLIST_ABSOLUTE pidlFull = nullptr; SFGAOF sfgao = 0;
            HRESULT hr = SHParseDisplayName(fullPath.c_str(), nullptr, &pidlFull, 0, &sfgao);
            if (SUCCEEDED(hr) && pidlFull) {
                PIDLIST_ABSOLUTE pidlParent = ILCloneFull(pidlFull);
                if (pidlParent) {
                    ILRemoveLastID(pidlParent);
                    PCUITEMID_CHILD child = ILFindChild(pidlParent, pidlFull);
                    if (child) {
                        hr = SHOpenFolderAndSelectItems(pidlParent, 1, &child, 0);
                        if (SUCCEEDED(hr)) { AddLog(L"[OK] Shell API 定位成功。\r\n"); success = true; }
                        else { AddLog(L"[FAIL] SHOpenFolderAndSelectItems=0x%08X\r\n", (unsigned)hr); }
                    }
                    CoTaskMemFree(pidlParent);
                }
                CoTaskMemFree(pidlFull);
            }
            else { AddLog(L"[FAIL] SHParseDisplayName=0x%08X\r\n", (unsigned)hr); }
        }
        if (!success) {
            wchar_t winDir[MAX_PATH] = { 0 }; GetWindowsDirectoryW(winDir, MAX_PATH);
            std::wstring explorerPath = std::wstring(winDir) + L"\\explorer.exe";
            std::wstring params = L"-n /select,\"" + fullPath + L"\"";
            std::wstring cmdLine = L"\"" + explorerPath + L"\" " + params;
            STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = { 0 };
            BOOL ok = CreateProcessW(explorerPath.c_str(), &cmdLine[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
            if (ok) { AddLog(L"[OK] explorer -n 启动成功：%s\r\n", fullPath.c_str()); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); success = true; }
            else { AddLog(L"[FAIL] CreateProcess explorer 错误码=%u\r\n", GetLastError()); }
        }
        if (!success) {
            AddLog(L"[INFO] 兜底：打开父目录 %s\r\n", folderPath.c_str());
            HINSTANCE hRet = ShellExecuteW(NULL, L"open", folderPath.c_str(), NULL, NULL, SW_SHOW);
            if ((INT_PTR)hRet > 32) AddLog(L"[OK] 已打开父目录\r\n");
            else AddLog(L"[FAIL] 打开父目录错误码=%d\r\n", (INT_PTR)hRet);
        }
        break;
    }
    case IDM_LIST_COPY_PATH: {
        if (OpenClipboard(g_hMainWnd)) {
            EmptyClipboard();
            size_t len = wcslen(path);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
            if (hMem) { memcpy(GlobalLock(hMem), path, (len + 1) * sizeof(wchar_t)); GlobalUnlock(hMem); SetClipboardData(CF_UNICODETEXT, hMem); }
            CloseClipboard();
        }
        break;
    }
    case IDM_LIST_SELECT_ALL: {
        int n = ListView_GetItemCount(g_hListResults);
        for (int i = 0; i < n; i++) ListView_SetCheckState(g_hListResults, i, TRUE);
        break;
    }
    case IDM_LIST_DELETE: {
        std::vector<int> selectedItems;
        int n = ListView_GetItemCount(g_hListResults);
        for (int i = 0; i < n; i++) {
            if (ListView_GetItemState(g_hListResults, i, LVIS_SELECTED)) selectedItems.push_back(i);
        }
        if (selectedItems.empty() && itemIndex >= 0) selectedItems.push_back(itemIndex);
        for (auto it = selectedItems.rbegin(); it != selectedItems.rend(); ++it) {
            wchar_t delPath[MAX_PATH] = { 0 };
            ListView_GetItemText(g_hListResults, *it, 0, delPath, MAX_PATH);
            ListView_DeleteItem(g_hListResults, *it);
            AddLog(L"已从列表删除: %s\r\n", delPath);
        }
        break;
    }
    }
}
void InitTrayIcon() {
    if (g_trayInited) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_hMainWnd; g_nid.uID = IDC_TRAY_ICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAY_NOTIFY;
    g_nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCE(IDI_MAIN));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"VMware 虚拟磁盘维护工具");
    g_trayInited = Shell_NotifyIconW(NIM_ADD, &g_nid);
}
void RemoveTrayIcon() {
    if (g_trayInited) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_trayInited = false; }
}
void ShowTrayNotification(const wchar_t* title, const wchar_t* message) {
    if (!g_trayInited) return;
    g_nid.uFlags |= NIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, title); wcscpy_s(g_nid.szInfo, message);
    g_nid.uTimeout = 5000; g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;
}
void SetListItemText(int row, int subItem, const std::wstring& text) {
    LVITEMW lv = { 0 };
    lv.mask = LVIF_TEXT; lv.iItem = row; lv.iSubItem = subItem; lv.pszText = (LPWSTR)text.c_str();
    SendMessageW(g_hListResults, LVM_SETITEMW, 0, (LPARAM)&lv);
}
void UpdateListItemStatus(const std::wstring& filePath, const std::wstring& status) {
    int n = ListView_GetItemCount(g_hListResults);
    for (int i = 0; i < n; i++) {
        wchar_t buf[MAX_PATH] = { 0 };
        ListView_GetItemText(g_hListResults, i, 0, buf, MAX_PATH);
        if (_wcsicmp(buf, filePath.c_str()) == 0) { SetListItemText(i, 4, status); return; }
    }
}
void AddResultToList(const std::wstring& filePath, ULONGLONG size) {
    std::wstring sizeStr = FormatSize(size);
    int idx = ListView_GetItemCount(g_hListResults);
    LVITEMW lvi = { 0 };
    lvi.mask = LVIF_TEXT; lvi.iItem = idx; lvi.iSubItem = 0; lvi.pszText = (LPWSTR)filePath.c_str();
    int inserted = (int)SendMessageW(g_hListResults, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
    if (inserted < 0) return;
    SetListItemText(inserted, 1, sizeStr);
    SetListItemText(inserted, 2, L"—");
    SetListItemText(inserted, 3, L"—");
    if (IsSnapshotDisk(filePath)) SetListItemText(inserted, 4, L"⚠ 快照盘");
    else if (HasSnapshotDisk(filePath)) SetListItemText(inserted, 4, L"只读(有快照)");
    else SetListItemText(inserted, 4, L"待处理");
    ListView_SetCheckState(g_hListResults, inserted, TRUE);
}
void UpdateMaintenanceResult(const std::wstring& path, ULONGLONG beforeSize, ULONGLONG afterSize, bool success)
{
    int n = ListView_GetItemCount(g_hListResults);
    for (int i = 0; i < n; i++) {
        wchar_t buf[MAX_PATH] = { 0 };
        ListView_GetItemText(g_hListResults, i, 0, buf, MAX_PATH);
        if (_wcsicmp(buf, path.c_str()) == 0) {
            SetListItemText(i, 2, FormatSize(afterSize));
            if (beforeSize > afterSize && beforeSize > 0) {
                ULONGLONG saved = beforeSize - afterSize;
                double pct = (double)saved * 100.0 / (double)beforeSize;
                std::wstring savedStr = FormatSize(saved);
                wchar_t tmp[160]; swprintf_s(tmp, L"%s (%.1f%%)", savedStr.c_str(), pct);
                SetListItemText(i, 3, tmp);
                g_totalSavedBytes += saved;
            }
            else { SetListItemText(i, 3, L"0 B (0.0%)"); }
            SetListItemText(i, 4, success ? L"完成" : L"跳过");
            g_completedCount++;
            break;
        }
    }
}
void SearchRecursiveAsync(const std::wstring& dir, const std::wstring& pattern, int depth) {
    if (depth > 8) return;
    if (g_searchCancel) return;
    static const std::set<std::wstring> ignoreDirs = {
        L"Windows", L"Program Files", L"Program Files (x86)", L"$Recycle.Bin",
        L"System Volume Information", L"Recovery", L"PerfLogs"
    };
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (g_searchCancel) break;
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (ignoreDirs.find(name) != ignoreDirs.end()) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            SearchRecursiveAsync(JoinPath(dir, name), pattern, depth + 1);
        }
        else {
            if (PathMatchSpecW(name.c_str(), pattern.c_str())) {
                std::wstring full = JoinPath(dir, name);
                ULONGLONG size = GetVmdkActualSize(full);
                SearchResult* r = new SearchResult{ full, size };
                if (!PostMessageW(g_hMainWnd, WM_APP + 10, 0, (LPARAM)r)) delete r;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
void SearchThreadProc(std::wstring dir, std::wstring pattern) {
    g_searchCancel = false;
    g_foundCount = 0;
    dir = TrimTrailingSlash(dir);
    if (dir.empty()) {
        DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (g_searchCancel) break;
            if (drives & (1 << i)) {
                wchar_t root[] = { (wchar_t)(L'A' + i), L':', 0 };
                if (GetDriveTypeW((std::wstring(root) + L"\\").c_str()) == DRIVE_FIXED) {
                    std::wstring* pRoot = new std::wstring(root);
                    if (!PostMessageW(g_hMainWnd, WM_APP + 12, 0, (LPARAM)pRoot)) delete pRoot;
                    SearchRecursiveAsync(root, pattern, 0);
                }
            }
        }
    }
    else { SearchRecursiveAsync(dir, pattern, 0); }
    PostMessageW(g_hMainWnd, WM_APP + 11, 0, 0);
}
void GetSystemResources(ULONGLONG& totalMemMB, ULONGLONG& availMemMB, DWORD& cpuCount) {
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    totalMemMB = ms.ullTotalPhys / (1024 * 1024);
    availMemMB = ms.ullAvailPhys / (1024 * 1024);
    SYSTEM_INFO si; GetSystemInfo(&si); cpuCount = si.dwNumberOfProcessors;
}
int CalculateMaxConcurrency(const std::vector<Job>& jobs)
{
    ULONGLONG totalMem = 0, availMem = 0; DWORD cpuCount = 0;
    GetSystemResources(totalMem, availMem, cpuCount);
    int memLimit = (int)(availMem / 300); if (memLimit < 1) memLimit = 1;
    int cpuLimit = (int)(cpuCount > 8 ? 8 : cpuCount); if (cpuLimit < 1) cpuLimit = 1;
    std::set<wchar_t> drives;
    for (auto& j : jobs) { if (j.vmdkPath.size() >= 2 && j.vmdkPath[1] == L':') drives.insert((wchar_t)towupper(j.vmdkPath[0])); }
    double minFactor = 1.0; DiskKind slowestKind = DiskKind::NvmeSsd;
    std::map<wchar_t, DiskKind> driveKindMap;
    for (wchar_t d : drives) {
        std::wstring probe; probe += d; probe += L":\\";
        DiskKind k = DetectDiskKind(probe);
        driveKindMap[d] = k;
        double f = DiskKindFactor(k);
        if (f < minFactor) { minFactor = f; slowestKind = k; }
    }
    int diskLimit = (int)(8 * minFactor + 0.5);
    if (diskLimit < 1) diskLimit = 1; if (diskLimit > 8) diskLimit = 8;
    AddLog(L"\r\n[资源评估] CPU 核心=%u，总内存=%llu MB，可用内存=%llu MB\r\n", cpuCount, totalMem, availMem);
    for (auto& kv : driveKindMap) AddLog(L"[磁盘检测] %c: -> %s（并发因子 %.2f）\r\n", kv.first, DiskKindName(kv.second), DiskKindFactor(kv.second));
    if (!driveKindMap.empty()) AddLog(L"[磁盘检测] 最慢磁盘类型：%s，磁盘并发上限：%d\r\n", DiskKindName(slowestKind), diskLimit);
    int conc = memLimit;
    if (cpuLimit < conc) conc = cpuLimit;
    if (diskLimit < conc) conc = diskLimit;
    if (conc > 8) conc = 8;
    if (conc < 1) conc = 1;
    AddLog(L"[并发决策] 内存限制=%d，CPU限制=%d，磁盘限制=%d -> 最终并发=%d\r\n", memLimit, cpuLimit, diskLimit, conc);
    return conc;
}
void SetUIEnabled(bool enabled) {
    EnableWindow(g_hBtnStart, enabled && !g_vdiskmanagerPath.empty());
    EnableWindow(g_hBtnBrowse, enabled);
    EnableWindow(g_hBtnSelTool, enabled);
    EnableWindow(g_hEditVmdk, enabled);
    EnableWindow(g_hCheckE, enabled);
    EnableWindow(g_hCheckR, enabled);
    EnableWindow(g_hCheckD, enabled);
    EnableWindow(g_hCheckK, enabled);
    EnableWindow(g_hBtnCancel, !enabled);
    EnableWindow(g_hBtnDisableLog, enabled);
    EnableWindow(g_hBtnBrowseDir, enabled);
    EnableWindow(g_hEditSearchDir, enabled);
    EnableWindow(g_hEditPattern, enabled);
    EnableWindow(g_hBtnSearch, enabled);
    EnableWindow(g_hBtnSelAll, enabled);
    EnableWindow(g_hBtnSelNone, enabled);
    EnableWindow(g_hBtnSelInvert, enabled);
    EnableWindow(g_hBtnClearResults, enabled);
    EnableWindow(g_hBtnExport, enabled);
}
void SetUISearching(bool searching) {
    EnableWindow(g_hBtnStart, !searching);
    EnableWindow(g_hBtnBrowse, !searching);
    EnableWindow(g_hBtnSelTool, !searching);
    EnableWindow(g_hBtnDisableLog, !searching);
    EnableWindow(g_hBtnBrowseDir, !searching);
    EnableWindow(g_hBtnSelAll, !searching);
    EnableWindow(g_hBtnSelNone, !searching);
    EnableWindow(g_hBtnSelInvert, !searching);
    EnableWindow(g_hBtnClearResults, !searching);
    EnableWindow(g_hBtnExport, !searching);
    EnableWindow(g_hBtnSearch, TRUE);
}
void SetProgress(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    SendMessageW(g_hProgBar, PBM_SETPOS, (WPARAM)percent, 0);
    wchar_t buf[64]; StringCchPrintfW(buf, 64, L"总进度：%d%%", percent);
    SetWindowTextW(g_hStaticInfo, buf);
}
void WorkerThread(std::vector<Job> jobs)
{
    SetUIEnabled(false);
    g_cancel = false;
    g_totalSavedBytes = 0;
    g_completedCount = 0;
    if (jobs.empty()) { PostMessageW(g_hMainWnd, WM_APP + 1, 0, 0); return; }
    struct StepInfo { const wchar_t* name; const wchar_t* arg; };
    StepInfo stepsInfo[] = {
        { L"检查磁盘一致性", L" -e \"" },
        { L"检查并修复磁盘", L" -R \"" },
        { L"整理磁盘碎片",   L" -d \"" },
        { L"回收未使用空间", L" -k \"" }
    };
    int totalStepsAll = 0;
    for (auto& job : jobs) for (int i = 0; i < 4; i++) if (job.steps & (1 << i)) totalStepsAll++;
    if (totalStepsAll == 0) totalStepsAll = 1;
    std::atomic<int> doneSteps{ 0 };
    std::atomic<int> activeThreads{ 0 };
    std::atomic<int> skippedJobs{ 0 };
    std::deque<std::pair<int, Job>> queue;
    std::mutex queueMutex;
    for (size_t i = 0; i < jobs.size(); i++) queue.push_back({ (int)i, jobs[i] });
    AddLog(L"\r\n=== 批量并发处理开始 ===\r\n");
    AddLog(L"任务数量：%d 个\r\n", (int)jobs.size());
    int maxConcurrency = CalculateMaxConcurrency(jobs);
    if (maxConcurrency > (int)jobs.size()) maxConcurrency = (int)jobs.size();
    if (maxConcurrency < 1) maxConcurrency = 1;
    AddLog(L"并发线程：%d 个\r\n", maxConcurrency);
    AddLog(L"执行模式：智能并发调度（NVMe/SSD/HDD 自适应）\r\n");
    AddLog(L"取消模式：软取消（当前步骤跑完即停，保护 VMDK 完整）\r\n");
    auto worker = [&]() {
        while (true) {
            if (g_cancel) break;
            std::pair<int, Job> jobPair;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (queue.empty()) break;
                jobPair = queue.front();
                queue.pop_front();
            }
            int idx = jobPair.first;
            const Job& job = jobPair.second;
            const std::wstring& vmdkPath = job.vmdkPath;
            int steps = job.steps;
            int active = ++activeThreads;
            AddLog(L"\r\n──── 任务 %d/%d 开始（当前并发 %d/%d）────\r\n", idx + 1, (int)jobs.size(), active, maxConcurrency);
            AddLog(L"文件：%s\r\n", vmdkPath.c_str());
            ULONGLONG beforeSize = GetVmdkActualSize(vmdkPath);
            ULONGLONG freeSpace = GetFreeSpaceBytes(vmdkPath);
            ULONGLONG required = (ULONGLONG)(beforeSize * 2.0);
            AddLog(L"[磁盘检测] 维护前实际大小：%s，剩余空间：%s，需要：%s\r\n", FormatSize(beforeSize).c_str(), FormatSize(freeSpace).c_str(), FormatSize(required).c_str());
            bool needSpace = (steps & 0x04) || (steps & 0x08);
            if (needSpace && freeSpace < required) {
                AddLog(L"✘ 磁盘空间不足，跳过此任务。\r\n");
                for (int i = 0; i < 4; i++) {
                    if (steps & (1 << i)) { int d = ++doneSteps; int pct = (d * 100) / totalStepsAll; SetProgress(pct); }
                }
                skippedJobs++;
                MaintenanceResult* mr = new MaintenanceResult{ vmdkPath, beforeSize, beforeSize, false };
                if (!PostMessageW(g_hMainWnd, WM_APP_MAINT_RESULT, 0, (LPARAM)mr)) delete mr;
                --activeThreads;
                continue;
            }
            else if (needSpace) { AddLog(L"✔ 磁盘空间充足，可以安全执行。\r\n"); }
            bool allOk = true;
            for (int i = 0; i < 4; i++) {
                if (g_cancel) { AddLog(L"[取消] 用户请求取消，剩余步骤不再执行。\r\n"); allOk = false; break; }
                if (!(steps & (1 << i))) continue;
                std::wstring cmd = L"\"" + g_vdiskmanagerPath + L"\"" + stepsInfo[i].arg + vmdkPath + L"\"";
                AddLog(L"\r\n▶ 任务 %d [%s]\r\n", idx + 1, stepsInfo[i].name);
                AddLog(L"命令：%s\r\n", cmd.c_str());
                std::wstring workDir = vmdkPath.substr(0, vmdkPath.find_last_of(L'\\'));
                bool ok = ProcessRunner::RunCommand(cmd, workDir,
                    [](const std::wstring& line) { AddLog(L"%s\r\n", line.c_str()); }, nullptr);
                if (ok) AddLog(L"✔ 任务 %d [%s] 完成。\r\n", idx + 1, stepsInfo[i].name);
                else { AddLog(L"✘ 任务 %d [%s] 失败。\r\n", idx + 1, stepsInfo[i].name); allOk = false; }
                int d = ++doneSteps; int pct = (d * 100) / totalStepsAll; SetProgress(pct);
            }
            ULONGLONG afterSize = GetVmdkActualSize(vmdkPath);
            if (beforeSize > afterSize) {
                ULONGLONG saved = beforeSize - afterSize;
                double pct = (double)saved * 100.0 / (double)beforeSize;
                AddLog(L"[结果] 维护前：%s → 维护后：%s，节省：%s (%.1f%%)\r\n", FormatSize(beforeSize).c_str(), FormatSize(afterSize).c_str(), FormatSize(saved).c_str(), pct);
            }
            else {
                AddLog(L"[结果] 维护前：%s → 维护后：%s，未减小。\r\n", FormatSize(beforeSize).c_str(), FormatSize(afterSize).c_str());
            }
            MaintenanceResult* mr = new MaintenanceResult{ vmdkPath, beforeSize, afterSize, allOk };
            if (!PostMessageW(g_hMainWnd, WM_APP_MAINT_RESULT, 0, (LPARAM)mr)) delete mr;
            --activeThreads;
            AddLog(L"──── 任务 %d/%d 结束（剩余活动线程 %d）────\r\n", idx + 1, (int)jobs.size(), activeThreads.load());
        }
        };
    std::vector<std::thread> pool;
    for (int i = 0; i < maxConcurrency; i++) pool.emplace_back(worker);
    for (auto& t : pool) if (t.joinable()) t.join();
    SetProgress(100);
    AddLog(L"\r\n=== 批量并发处理结束 ===\r\n");
    ULONGLONG totalSaved = g_totalSavedBytes.load();
    int doneCount = g_completedCount.load();
    AddLog(L"完成任务：%d 个，跳过：%d 个。\r\n", (int)jobs.size() - skippedJobs.load(), skippedJobs.load());
    AddLog(L"累计节省空间：%s\r\n", FormatSize(totalSaved).c_str());
    wchar_t msg[160]; swprintf_s(msg, L"共处理 %d 个，节省 %s", doneCount, FormatSize(totalSaved).c_str());
    ShowTrayNotification(L"批量处理完成", msg);
    g_cancel = false;
    PostMessageW(g_hMainWnd, WM_APP + 1, 0, 0);
}
void DoSearch() { SendMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_SEARCH, BN_CLICKED), 0); }
void DoSelectVmdkFile() { SendMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_BROWSE_VMDK, BN_CLICKED), 0); }
void DoExport() { ExportResultsToCsv(); }
void DoClearLog() { SendMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_CLEAR_LOG, BN_CLICKED), 0); }
void DoSelectAll() {
    HWND hFocus = GetFocus();
    if (hFocus == g_hListResults) SendMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_SEL_ALL, BN_CLICKED), 0);
    else SendMessageW(hFocus, EM_SETSEL, 0, -1);
}
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hMainWnd = hWnd;
        g_hInst = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);

        CreateWindowW(L"STATIC", L"VMware 虚拟磁盘维护工具（VMDK 减肥专家）", WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 8, 800, 28, hWnd, (HMENU)IDC_TITLE, NULL, NULL);
        CreateWindowW(L"STATIC", L"VMDK 文件:", WS_CHILD | WS_VISIBLE, 10, 45, 80, 20, hWnd, (HMENU)2001, NULL, NULL);
        g_hEditVmdk = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 100, 42, 600, 24, hWnd, NULL, NULL, NULL);
        g_hBtnBrowse = CreateWindowW(L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 710, 42, 100, 24, hWnd, (HMENU)IDC_BTN_BROWSE_VMDK, NULL, NULL);
        CreateWindowW(L"STATIC", L"工具位置:", WS_CHILD | WS_VISIBLE, 10, 80, 80, 20, hWnd, (HMENU)2002, NULL, NULL);
        g_hToolPath = CreateWindowW(L"STATIC", L"后台检测中...", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 100, 80, 600, 20, hWnd, NULL, NULL, NULL);
        g_hBtnSelTool = CreateWindowW(L"BUTTON", L"手动指定...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 710, 77, 100, 24, hWnd, (HMENU)IDC_BTN_CHOOSE_TOOL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"搜索 VMDK 文件（目录留空 = 全盘搜索）", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 108, 800, 72, hWnd, (HMENU)IDC_GRP_SEARCH, NULL, NULL);
        CreateWindowW(L"STATIC", L"搜索目录:", WS_CHILD | WS_VISIBLE, 22, 130, 60, 20, hWnd, (HMENU)2003, NULL, NULL);
        g_hEditSearchDir = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 85, 128, 470, 24, hWnd, NULL, NULL, NULL);
        g_hBtnBrowseDir = CreateWindowW(L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 565, 128, 80, 24, hWnd, (HMENU)IDC_BTN_BROWSE_DIR, NULL, NULL);
        CreateWindowW(L"STATIC", L"通配符:", WS_CHILD | WS_VISIBLE, 22, 158, 60, 20, hWnd, (HMENU)2004, NULL, NULL);
        g_hEditPattern = CreateWindowW(L"EDIT", L"*.vmdk", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 85, 156, 200, 24, hWnd, NULL, NULL, NULL);
        g_hBtnSearch = CreateWindowW(L"BUTTON", L"开始搜索", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 295, 156, 100, 24, hWnd, (HMENU)IDC_BTN_SEARCH, NULL, NULL);
        g_hListResults = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 10, 188, 800, 260, hWnd, (HMENU)IDC_LIST_RESULTS, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_hListResults, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);
        LVCOLUMNW lvc = { 0 }; lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.pszText = (LPWSTR)L"VMDK 文件路径"; lvc.cx = 390; SendMessageW(g_hListResults, LVM_INSERTCOLUMNW, 0, (LPARAM)&lvc);
        lvc.pszText = (LPWSTR)L"维护前"; lvc.cx = 95; SendMessageW(g_hListResults, LVM_INSERTCOLUMNW, 1, (LPARAM)&lvc);
        lvc.pszText = (LPWSTR)L"维护后"; lvc.cx = 95; SendMessageW(g_hListResults, LVM_INSERTCOLUMNW, 2, (LPARAM)&lvc);
        lvc.pszText = (LPWSTR)L"节省空间"; lvc.cx = 110; SendMessageW(g_hListResults, LVM_INSERTCOLUMNW, 3, (LPARAM)&lvc);
        lvc.pszText = (LPWSTR)L"状态"; lvc.cx = 110; SendMessageW(g_hListResults, LVM_INSERTCOLUMNW, 4, (LPARAM)&lvc);
        g_hBtnSelAll = CreateWindowW(L"BUTTON", L"全选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 456, 70, 26, hWnd, (HMENU)IDC_BTN_SEL_ALL, NULL, NULL);
        g_hBtnSelNone = CreateWindowW(L"BUTTON", L"全不选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 85, 456, 70, 26, hWnd, (HMENU)IDC_BTN_SEL_NONE, NULL, NULL);
        g_hBtnSelInvert = CreateWindowW(L"BUTTON", L"反选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 160, 456, 70, 26, hWnd, (HMENU)IDC_BTN_SEL_INVERT, NULL, NULL);
        g_hBtnClearResults = CreateWindowW(L"BUTTON", L"清空列表", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 235, 456, 90, 26, hWnd, (HMENU)IDC_BTN_CLEAR_RESULTS, NULL, NULL);
        g_hBtnExport = CreateWindowW(L"BUTTON", L"导出列表", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 330, 456, 90, 26, hWnd, (HMENU)IDC_BTN_EXPORT, NULL, NULL);
        CreateWindowW(L"BUTTON", L"操作步骤:", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 490, 800, 62, hWnd, (HMENU)IDC_GRP_STEPS, NULL, NULL);
        g_hCheckE = CreateWindowW(L"BUTTON", L"检查磁盘一致性 (-e)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 512, 200, 20, hWnd, (HMENU)IDC_CHECK_E, NULL, NULL);
        g_hCheckR = CreateWindowW(L"BUTTON", L"检查并修复 (-R)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 240, 512, 180, 20, hWnd, (HMENU)IDC_CHECK_R, NULL, NULL);
        g_hCheckD = CreateWindowW(L"BUTTON", L"整理碎片 (-d)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 430, 512, 180, 20, hWnd, (HMENU)IDC_CHECK_D, NULL, NULL);
        g_hCheckK = CreateWindowW(L"BUTTON", L"回收空间 (-k)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 534, 200, 20, hWnd, (HMENU)IDC_CHECK_K, NULL, NULL);
        g_hBtnStart = CreateWindowW(L"BUTTON", L"开始执行", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 562, 110, 30, hWnd, (HMENU)IDC_BTN_START, NULL, NULL);
        g_hBtnCancel = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 130, 562, 80, 30, hWnd, (HMENU)IDC_BTN_CANCEL, NULL, NULL);
        g_hProgBar = CreateWindowW(L"msctls_progress32", L"", WS_CHILD | WS_VISIBLE, 220, 567, 590, 20, hWnd, NULL, NULL, NULL);
        g_hStaticInfo = CreateWindowW(L"STATIC", L"就绪", WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 600, 800, 18, hWnd, NULL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"高级功能", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 624, 800, 62, hWnd, (HMENU)IDC_GRP_ADVANCED, NULL, NULL);
        g_hBtnDisableLog = CreateWindowW(L"BUTTON", L"禁用日志", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 646, 110, 30, hWnd, (HMENU)IDC_BTN_DISABLE_LOG, NULL, NULL);
        g_hLogList = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL | ES_NOHIDESEL, 10, 694, 800, 200, hWnd, NULL, NULL, NULL);
        g_hBtnClearLog = CreateWindowW(L"BUTTON", L"清空日志", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 720, 902, 90, 24, hWnd, (HMENU)IDC_BTN_CLEAR_LOG, NULL, NULL);
        HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(g_hLogList, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_hEditVmdk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_hEditSearchDir, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_hEditPattern, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_hListResults, WM_SETFONT, (WPARAM)hFont, TRUE);
        g_iniPath = GetIniPath();
        LoadConfig();
        SetWindowTextW(g_hEditVmdk, g_cfg.lastVmdkPath.c_str());
        SetWindowTextW(g_hEditSearchDir, g_cfg.lastSearchDir.c_str());
        SetWindowTextW(g_hEditPattern, g_cfg.lastPattern.empty() ? L"*.vmdk" : g_cfg.lastPattern.c_str());
        SendMessageW(g_hCheckE, BM_SETCHECK, g_cfg.checkE ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_hCheckR, BM_SETCHECK, g_cfg.checkR ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_hCheckD, BM_SETCHECK, g_cfg.checkD ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_hCheckK, BM_SETCHECK, g_cfg.checkK ? BST_CHECKED : BST_UNCHECKED, 0);
        DragAcceptFiles(hWnd, TRUE);
        InitTrayIcon();
        AddLog(L"====================================================\r\n");
        AddLog(L"  VMware 虚拟磁盘维护工具（VMDK 减肥专家）\r\n");
        AddLog(L"  基于 VMware 官方 vmware-vdiskmanager 命令\r\n");
        AddLog(L"  安全稳定，操作简单，可放心使用\r\n");
        AddLog(L"====================================================\r\n\r\n");
        AddLog(L"核心功能：为 VMDK 虚拟磁盘减肥，回收未使用空间。\r\n");
        AddLog(L"搜索结果列表会显示【维护前/维护后/节省空间/状态】。\r\n");
        AddLog(L"建议同时勾选 -d（整理碎片）和 -k（回收空间）以获得最佳效果。\r\n");
        AddLog(L"\r\n");
        AddLog(L"【INI 配置】\r\n");
        AddLog(L"  - [Options] AutoRelease=1 时，强制从内置资源释放工具文件\r\n");
        AddLog(L"  - 手动指定工具时，默认打开 portable 目录\r\n");
        AddLog(L"\r\n");
        AddLog(L"【快照盘处理逻辑】\r\n");
        AddLog(L"  - 无快照 VM：直接处理基础盘（如 XXX.vmdk）\r\n");
        AddLog(L"  - 有快照 VM：基础盘只读，自动转为处理最新快照盘（XXX-000001.vmdk）\r\n");
        AddLog(L"  - 仅当您显式勾选快照盘时才会弹出警告，程序自动转换的快照盘不弹窗\r\n");
        AddLog(L"  - 弹窗选择在本次运行中记忆，第二次起不再重复弹窗\r\n");
        AddLog(L"      【是】：按勾选完整执行（含 -k，由 vmware-vdiskmanager 自行判断）\r\n");
        AddLog(L"      【否】：降级为仅检查/修复（-e/-R）\r\n");
        AddLog(L"      【X 关闭】：终止执行，不处理任何磁盘\r\n");
        AddLog(L"\r\n");
        AddLog(L"【高级功能 - 禁用日志】\r\n");
        AddLog(L"  - 未勾选 VMDK：仅修改全局设置（settings.ini 添加 logging=FALSE）\r\n");
        AddLog(L"  - 已勾选 VMDK：对同目录 .vmx 添加 logging=FALSE，删除 *.log，同时修改全局设置\r\n");
        AddLog(L"  - 跳过正在运行的虚拟机\r\n");
        AddLog(L"\r\n");
        AddLog(L"快捷键：Ctrl+F 聚焦搜索 | F5 搜索 | Ctrl+O 选文件 | Ctrl+E 导出 | Ctrl+L 清日志\r\n");
        AddLog(L"\r\n");
        if (!g_cfg.lastToolPath.empty() && GetFileAttributesW(g_cfg.lastToolPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_vdiskmanagerPath = g_cfg.lastToolPath;
            SetWindowTextW(g_hToolPath, g_vdiskmanagerPath.c_str());
            AddLog(L"OK 使用上次指定的工具：%s\r\n", g_vdiskmanagerPath.c_str());
            SetUIEnabled(true);
        }
        else {
            SetWindowTextW(g_hToolPath, L"后台检测中...");
            AddLog(L"正在后台检测 VMware 虚拟磁盘工具，界面可正常使用...\r\n");
            std::thread(ToolLocateThreadProc).detach();
            SetUIEnabled(true);
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        wchar_t file[MAX_PATH] = { 0 };
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
        if (count > 0) {
            DragQueryFileW(hDrop, 0, file, MAX_PATH);
            std::wstring f = file;
            size_t dot = f.find_last_of(L'.');
            if (dot != std::wstring::npos) {
                std::wstring ext = ToLowerStr(f.substr(dot));
                if (ext == L".vmdk") { SetWindowTextW(g_hEditVmdk, file); g_cfg.lastVmdkPath = file; SaveConfig(); AddLog(L"OK 拖拽添加 VMDK：%s\r\n", file); }
                else { AddLog(L"[WARN] 拖入的不是 VMDK 文件：%s\r\n", file); }
            }
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_TRAY_NOTIFY: {
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 4001, L"打开主窗口");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, 4002, L"退出");
            SetForegroundWindow(hWnd);
            int cmd = (int)TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);
            if (cmd == 4001) { ShowWindow(hWnd, SW_RESTORE); SetForegroundWindow(hWnd); }
            else if (cmd == 4002) SendMessageW(hWnd, WM_CLOSE, 0, 0);
        }
        else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) { ShowWindow(hWnd, SW_RESTORE); SetForegroundWindow(hWnd); }
        return 0;
    }
    case WM_APP_MAINT_RESULT: {
        MaintenanceResult* mr = (MaintenanceResult*)lParam;
        if (mr) { UpdateMaintenanceResult(mr->path, mr->beforeSize, mr->afterSize, mr->success); delete mr; }
        return 0;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        switch (id) {
        case IDC_BTN_BROWSE_VMDK: {
            OPENFILENAMEW ofn = { sizeof(ofn) };
            wchar_t file[MAX_PATH] = { 0 };
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"VMDK 文件 (*.vmdk)\0*.vmdk\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            ofn.lpstrTitle = L"选择 VMDK 虚拟磁盘文件";
            if (GetOpenFileNameW(&ofn)) { SetWindowTextW(g_hEditVmdk, file); g_cfg.lastVmdkPath = file; SaveConfig(); }
            break;
        }
        case IDC_BTN_CHOOSE_TOOL: {
            OPENFILENAMEW ofn = { sizeof(ofn) };
            wchar_t file[MAX_PATH] = { 0 };
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"vmware-vdiskmanager.exe\0vmware-vdiskmanager.exe\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            ofn.lpstrTitle = L"手动选择 vmware-vdiskmanager.exe";
            std::wstring portableDir;
            wchar_t tempDir[MAX_PATH] = { 0 };
            GetTempPathW(MAX_PATH, tempDir);
            portableDir = std::wstring(tempDir) + L"VmdkToolPortable";
            if (GetFileAttributesW(portableDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ofn.lpstrInitialDir = portableDir.c_str();
            }
            else {
                ofn.lpstrInitialDir = L"";
            }
            if (GetOpenFileNameW(&ofn)) {
                g_vdiskmanagerPath = file;
                SetWindowTextW(g_hToolPath, file);
                g_cfg.lastToolPath = file;
                SaveConfig();
                AddLog(L"OK 已手动指定工具：%s\r\n", file);
                SetUIEnabled(true);
            }
            break;
        }
        case IDC_BTN_BROWSE_DIR: {
            BROWSEINFOW bi = { 0 };
            bi.hwndOwner = hWnd;
            bi.lpszTitle = L"选择搜索目录";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH] = { 0 };
                if (SHGetPathFromIDListW(pidl, path)) { SetWindowTextW(g_hEditSearchDir, path); g_cfg.lastSearchDir = path; SaveConfig(); }
                CoTaskMemFree(pidl);
            }
            break;
        }
        case IDC_BTN_SEARCH: {
            if (g_searching) {
                g_searchCancel = true;
                SetWindowTextW(g_hBtnSearch, L"取消中...");
                AddLog(L"\r\n[取消] 正在停止搜索，请稍候...\r\n");
            }
            else {
                wchar_t dirBuf[MAX_PATH] = { 0 };
                wchar_t patBuf[256] = { 0 };
                GetWindowTextW(g_hEditSearchDir, dirBuf, MAX_PATH);
                GetWindowTextW(g_hEditPattern, patBuf, 256);
                if (patBuf[0] == 0) { wcscpy_s(patBuf, L"*.vmdk"); SetWindowTextW(g_hEditPattern, patBuf); }
                g_cfg.lastPattern = patBuf;
                g_cfg.lastSearchDir = dirBuf;
                SaveConfig();
                ListView_DeleteAllItems(g_hListResults);
                g_searching = true;
                g_searchCancel = false;
                g_foundCount = 0;
                SetWindowTextW(g_hBtnSearch, L"取消搜索");
                SetUISearching(true);
                AddLog(L"\r\n=== 开始搜索 ===\r\n");
                if (dirBuf[0] == 0) AddLog(L"范围：全盘搜索\r\n");
                else AddLog(L"范围：%s\r\n", dirBuf);
                AddLog(L"通配符：%s\r\n", patBuf);
                std::thread t(SearchThreadProc, std::wstring(dirBuf), std::wstring(patBuf));
                t.detach();
            }
            break;
        }
        case IDC_BTN_SEL_ALL: {
            int n = ListView_GetItemCount(g_hListResults);
            for (int i = 0; i < n; i++) ListView_SetCheckState(g_hListResults, i, TRUE);
            break;
        }
        case IDC_BTN_SEL_NONE: {
            int n = ListView_GetItemCount(g_hListResults);
            for (int i = 0; i < n; i++) ListView_SetCheckState(g_hListResults, i, FALSE);
            break;
        }
        case IDC_BTN_SEL_INVERT: {
            int n = ListView_GetItemCount(g_hListResults);
            for (int i = 0; i < n; i++) ListView_SetCheckState(g_hListResults, i, !ListView_GetCheckState(g_hListResults, i));
            break;
        }
        case IDC_BTN_CLEAR_RESULTS: {
            ListView_DeleteAllItems(g_hListResults);
            AddLog(L"已清空搜索结果列表。\r\n");
            break;
        }
        case IDC_BTN_EXPORT: { ExportResultsToCsv(); break; }
        case IDC_BTN_START: {
            std::vector<Job> jobs;
            int steps = 0;
            if (SendMessageW(g_hCheckE, BM_GETCHECK, 0, 0) == BST_CHECKED) steps |= 1;
            if (SendMessageW(g_hCheckR, BM_GETCHECK, 0, 0) == BST_CHECKED) steps |= 2;
            if (SendMessageW(g_hCheckD, BM_GETCHECK, 0, 0) == BST_CHECKED) steps |= 4;
            if (SendMessageW(g_hCheckK, BM_GETCHECK, 0, 0) == BST_CHECKED) steps |= 8;
            if (steps == 0) { MessageBoxW(hWnd, L"请至少勾选一个操作步骤。", L"提示", MB_OK | MB_ICONWARNING); break; }
            std::set<std::wstring> seen;
            std::vector<std::wstring> allChecked;
            int cnt = ListView_GetItemCount(g_hListResults);
            for (int i = 0; i < cnt; i++) {
                if (ListView_GetCheckState(g_hListResults, i)) {
                    wchar_t path[MAX_PATH] = { 0 };
                    ListView_GetItemText(g_hListResults, i, 0, path, MAX_PATH);
                    std::wstring lower = ToLowerStr(path);
                    if (seen.count(lower) == 0) { seen.insert(lower); allChecked.push_back(path); }
                }
            }
            if (allChecked.empty()) {
                wchar_t vmdkPath[MAX_PATH] = { 0 };
                GetWindowTextW(g_hEditVmdk, vmdkPath, MAX_PATH);
                if (vmdkPath[0] != 0) {
                    std::wstring lower = ToLowerStr(vmdkPath);
                    if (seen.count(lower) == 0) { seen.insert(lower); allChecked.push_back(vmdkPath); }
                }
            }
            if (allChecked.empty()) { MessageBoxW(hWnd, L"请勾选搜索结果，或在顶部选择 VMDK 文件。", L"提示", MB_OK | MB_ICONWARNING); break; }
            if (g_vdiskmanagerPath.empty()) { MessageBoxW(hWnd, L"未找到 vdiskmanager 工具，请手动指定。", L"提示", MB_OK | MB_ICONWARNING); break; }
            std::vector<std::wstring> baseDisks;
            std::vector<std::wstring> snapDisks;
            for (auto& p : allChecked) {
                if (IsSnapshotDisk(p)) snapDisks.push_back(p);
                else baseDisks.push_back(p);
            }
            std::vector<std::wstring> baseToProcess;
            bool hasAutoConvertedSnap = false;
            for (auto& p : baseDisks) {
                if (HasSnapshotDisk(p)) {
                    std::wstring latestSnap = FindLatestSnapshotDisk(p);
                    if (!latestSnap.empty()) {
                        bool exists = false;
                        for (auto& s : snapDisks) {
                            if (_wcsicmp(s.c_str(), latestSnap.c_str()) == 0) { exists = true; break; }
                        }
                        if (!exists) snapDisks.push_back(latestSnap);
                        hasAutoConvertedSnap = true;
                        UpdateListItemStatus(p, L"只读(已转快照盘)");
                        AddLog(L"[INFO] 基础盘有快照，只读，自动转为处理快照盘：\r\n");
                        AddLog(L"       基础盘：%s\r\n", p.c_str());
                        AddLog(L"       最新快照盘：%s\r\n", latestSnap.c_str());
                    }
                    else {
                        UpdateListItemStatus(p, L"跳过(快照盘缺失)");
                        AddLog(L"[WARN] 基础盘有快照引用但未找到快照盘：%s\r\n", p.c_str());
                    }
                }
                else {
                    baseToProcess.push_back(p);
                }
            }
            int snapSteps = steps;
            bool hasSnap = !snapDisks.empty();
            bool hasDAK = (steps & 0x0C) != 0;
            bool shouldWarn = hasSnap && hasDAK && hasAutoConvertedSnap;
            if (shouldWarn && g_snapWarnChoice == 0) {
                std::wstring msg = L"检测到 " + std::to_wstring(snapDisks.size()) +
                    L" 个快照盘需要维护（其中部分由基础盘自动转换）。\n\n"
                    L"维护快照盘需要快照链结构完整，否则可能出现错误。\n\n"
                    L"请选择处理方式：\n\n"
                    L"  【是】：按勾选完整执行（含 -k，由 vmware-vdiskmanager 自行判断）\n"
                    L"  【否】：降级为仅执行检查/修复（-e/-R）\n"
                    L"  【X 关闭】：终止执行，不处理任何磁盘\n";
                int ret = MessageBoxW(hWnd, msg.c_str(), L"⚠ 快照盘警告", MB_YESNOCANCEL | MB_ICONWARNING);
                if (ret == IDYES) {
                    g_snapWarnChoice = 1;
                    snapSteps = steps;
                    AddLog(L"[INFO] 用户选择【是】：按勾选完整执行快照盘（含 -k）。\r\n");
                }
                else if (ret == IDNO) {
                    g_snapWarnChoice = 2;
                    snapSteps = steps & 0x03;
                    if (snapSteps == 0) {
                        AddLog(L"[SKIP] 用户选择【否】，但未勾选 -e/-R，跳过快照盘。\r\n");
                        for (auto& p : snapDisks) UpdateListItemStatus(p, L"跳过(仅-e/-R未勾选)");
                        snapDisks.clear();
                    }
                    else {
                        AddLog(L"[INFO] 用户选择【否】：快照盘降级为仅 -e/-R。\r\n");
                    }
                }
                else {
                    g_snapWarnChoice = 3;
                    AddLog(L"[取消] 用户选择【X 关闭】：终止执行，不处理任何磁盘。\r\n");
                    SetWindowTextW(g_hStaticInfo, L"已取消执行。");
                    break;
                }
            }
            else if (shouldWarn && g_snapWarnChoice == 1) {
                snapSteps = steps;
                AddLog(L"[INFO] 本次运行已选择【是】，快照盘按完整执行（含 -k）。\r\n");
            }
            else if (shouldWarn && g_snapWarnChoice == 2) {
                snapSteps = steps & 0x03;
                if (snapSteps == 0) {
                    AddLog(L"[SKIP] 本次运行已选择【否】，但未勾选 -e/-R，跳过快照盘。\r\n");
                    for (auto& p : snapDisks) UpdateListItemStatus(p, L"跳过(仅-e/-R未勾选)");
                    snapDisks.clear();
                }
                else {
                    AddLog(L"[INFO] 本次运行已选择【否】：快照盘降级为仅 -e/-R。\r\n");
                }
            }
            else if (shouldWarn && g_snapWarnChoice == 3) {
                AddLog(L"[取消] 本次运行已选择【X 关闭】：终止执行，不处理任何磁盘。\r\n");
                SetWindowTextW(g_hStaticInfo, L"已取消执行。");
                break;
            }
            else {
                snapSteps = steps;
            }
            jobs.clear();
            for (auto& p : baseToProcess) jobs.push_back({ p, steps });
            for (auto& p : snapDisks) jobs.push_back({ p, snapSteps });
            if (jobs.empty()) { MessageBoxW(hWnd, L"没有可执行的任务（所有盘都被跳过）。\n请检查日志了解原因。", L"提示", MB_OK | MB_ICONINFORMATION); break; }
            AddLog(L"\r\n[任务构建] 基础盘(无快照)：%d 个，快照盘：%d 个\r\n", (int)baseToProcess.size(), (int)snapDisks.size());
            SendMessageW(g_hProgBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessageW(g_hProgBar, PBM_SETPOS, 0, 0);
            SetWindowTextW(g_hStaticInfo, L"开始执行...");
            std::thread t(WorkerThread, jobs);
            t.detach();
            break;
        }
        case IDC_BTN_CANCEL:
            if (!g_cancel) {
                g_cancel = true;
                AddLog(L"\r\n[取消] 已请求取消，当前正在执行的步骤会跑完后停止。\r\n");
                AddLog(L"       这是为了避免 VMDK 损坏，请耐心等待。\r\n");
            }
            break;
        case IDC_BTN_DISABLE_LOG: { DisableVmLogging(); break; }
        case IDC_BTN_CLEAR_LOG: {
            SetWindowTextW(g_hLogList, L"");
            AddLog(L"[INFO] 日志窗口已清空（磁盘日志文件不受影响）。\r\n");
            break;
        }
        case IDC_CHECK_E: g_cfg.checkE = (SendMessageW(g_hCheckE, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case IDC_CHECK_R: g_cfg.checkR = (SendMessageW(g_hCheckR, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case IDC_CHECK_D: g_cfg.checkD = (SendMessageW(g_hCheckD, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case IDC_CHECK_K: g_cfg.checkK = (SendMessageW(g_hCheckK, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case IDM_ACCEL_SEARCH_FOCUS:
            SetFocus(g_hEditSearchDir);
            SendMessageW(g_hEditSearchDir, EM_SETSEL, 0, -1);
            break;
        case IDM_ACCEL_SEARCH_START: DoSearch(); break;
        case IDM_ACCEL_OPEN_VMDK: DoSelectVmdkFile(); break;
        case IDM_ACCEL_EXPORT: DoExport(); break;
        case IDM_ACCEL_CLEAR_LOG: DoClearLog(); break;
        case IDM_ACCEL_SELECT_ALL: DoSelectAll(); break;
        case IDM_ACCEL_CANCEL:
            if (g_searching) { g_searchCancel = true; SetWindowTextW(g_hBtnSearch, L"取消中..."); }
            if (!g_cancel) g_cancel = true;
            break;
        }
        return 0;
    }
    case WM_APP + 10: {
        SearchResult* r = (SearchResult*)lParam;
        if (r) {
            AddResultToList(r->path, r->size);
            delete r;
            int n = ++g_foundCount;
            wchar_t buf[64]; swprintf_s(buf, L"搜索中... 已找到 %d 个文件", n);
            SetWindowTextW(g_hStaticInfo, buf);
        }
        return 0;
    }
    case WM_APP + 11: {
        g_searching = false;
        SetWindowTextW(g_hBtnSearch, L"开始搜索");
        SetUISearching(false);
        int n = g_foundCount.load();
        wchar_t buf[128];
        if (g_searchCancel) { swprintf_s(buf, L"搜索已取消，共找到 %d 个文件。", n); AddLog(L"\r\n[取消] 搜索已停止，共找到 %d 个文件。\r\n", n); }
        else { swprintf_s(buf, L"搜索完成，共找到 %d 个文件。", n); AddLog(L"\r\nOK 搜索完成，共找到 %d 个文件（已加入列表，默认勾选）。\r\n", n); }
        SetWindowTextW(g_hStaticInfo, buf);
        g_searchCancel = false;
        if (!g_searchCancel && n > 0) {
            wchar_t notif[64]; swprintf_s(notif, L"找到 %d 个 VMDK 文件", n);
            ShowTrayNotification(L"搜索完成", notif);
        }
        return 0;
    }
    case WM_APP + 12: {
        std::wstring* pRoot = (std::wstring*)lParam;
        if (pRoot) { AddLog(L"正在搜索 %s ...\r\n", pRoot->c_str()); delete pRoot; }
        return 0;
    }
    case WM_APP + 20: {
        ToolLocateResult* r = (ToolLocateResult*)lParam;
        if (r) {
            if (!g_vdiskmanagerPath.empty() && GetFileAttributesW(g_vdiskmanagerPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                AddLog(L"[INFO] 已使用手动指定的工具，忽略自动检测结果。\r\n");
            }
            else if (r->success) {
                g_vdiskmanagerPath = r->path;
                SetWindowTextW(g_hToolPath, r->path.c_str());
                g_cfg.lastToolPath = r->path;
                SaveConfig();
                AddLog(L"OK 找到工具：%s\r\n", r->path.c_str());
            }
            else {
                SetWindowTextW(g_hToolPath, L"未找到，请手动指定");
                AddLog(L"X 未找到 vmware-vdiskmanager.exe，请手动指定。\r\n");
            }
            delete r;
            SetUIEnabled(true);
        }
        return 0;
    }
    case WM_NOTIFY: {
        LPNMHDR phdr = (LPNMHDR)lParam;
        if (phdr->idFrom == IDC_LIST_RESULTS) {
            if (phdr->code == NM_DBLCLK) {
                LPNMITEMACTIVATE pia = (LPNMITEMACTIVATE)lParam;
                if (pia->iItem >= 0) {
                    wchar_t path[MAX_PATH] = { 0 };
                    ListView_GetItemText(g_hListResults, pia->iItem, 0, path, MAX_PATH);
                    SetWindowTextW(g_hEditVmdk, path);
                    g_cfg.lastVmdkPath = path;
                    SaveConfig();
                }
            }
            else if (phdr->code == NM_RCLICK) {
                int itemIndex = -1;
                LPNMITEMACTIVATE pia = (LPNMITEMACTIVATE)lParam;
                if (pia && pia->iItem >= 0) itemIndex = pia->iItem;
                if (itemIndex < 0) itemIndex = ListView_GetNextItem(g_hListResults, -1, LVNI_SELECTED);
                if (itemIndex >= 0) {
                    POINT pt; GetCursorPos(&pt);
                    ShowContextMenu(itemIndex, pt.x, pt.y);
                }
                return 0;
            }
        }
        return 0;
    }
    case WM_EXITSIZEMOVE:
        SaveConfig();
        return 0;
    case WM_APP + 1: {
        SetUIEnabled(true);
        ULONGLONG total = g_totalSavedBytes.load();
        int cnt = g_completedCount.load();
        if (cnt > 0) {
            wchar_t buf[256]; swprintf_s(buf, L"任务完成：共处理 %d 个文件，累计节省 %s", cnt, FormatSize(total).c_str());
            SetWindowTextW(g_hStaticInfo, buf);
        }
        return 0;
    }
    case WM_DESTROY:
        g_cancel = true;
        g_searchCancel = true;
        RemoveTrayIcon();
        SaveConfig();
        CloseLogFile();
        if (g_hAccel) { DestroyAcceleratorTable(g_hAccel); g_hAccel = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR pCmdLine, int nCmdShow) {
    g_hInst = hInstance;
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    g_iniPath = GetIniPath();
    LoadConfig();
    InitLogFile();
    ACCEL accelTable[] = {
        { FVIRTKEY | FCONTROL, 'F', IDM_ACCEL_SEARCH_FOCUS },
        { FVIRTKEY,             VK_F5, IDM_ACCEL_SEARCH_START },
        { FVIRTKEY | FCONTROL, 'O', IDM_ACCEL_OPEN_VMDK },
        { FVIRTKEY | FCONTROL, 'E', IDM_ACCEL_EXPORT },
        { FVIRTKEY | FCONTROL, 'L', IDM_ACCEL_CLEAR_LOG },
        { FVIRTKEY | FCONTROL, 'A', IDM_ACCEL_SELECT_ALL },
        { FVIRTKEY,             VK_ESCAPE, IDM_ACCEL_CANCEL },
    };
    g_hAccel = CreateAcceleratorTableW(accelTable, sizeof(accelTable) / sizeof(accelTable[0]));
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_MAIN));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VmdkToolWndClass";
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_MAIN));
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    int x = (g_cfg.winX >= 0) ? g_cfg.winX : CW_USEDEFAULT;
    int y = (g_cfg.winY >= 0) ? g_cfg.winY : CW_USEDEFAULT;
    int w = 840;
    int h = 940;
    HWND hWnd = CreateWindowExW(0, wc.lpszClassName, L"VMware 虚拟磁盘维护工具", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, x, y, w, h, NULL, NULL, hInstance, NULL);
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (g_hAccel && TranslateAcceleratorW(hWnd, g_hAccel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseLogFile();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return (int)msg.wParam;
}