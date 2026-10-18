#include "Core.h"
#include <vector>
#include <string>

#pragma comment(lib, "shlwapi.lib")

// ============================================================
// Helpers 
// ============================================================
static bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

static std::wstring EnsureTrailingSlash(const std::wstring& s) {
    if (s.empty()) return s;
    if (s.back() == L'\\' || s.back() == L'/') return s;
    return s + L'\\';
}

// Try UTF-8 first; fall back to ANSI if the bytes aren't valid UTF-8.
static std::wstring BytesToWide(const std::string& bytes) {
    if (bytes.empty()) return std::wstring();

    // Try UTF-8 
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.c_str(), (int)bytes.size(),
        NULL, 0);
    if (wlen > 0) {
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            bytes.c_str(), (int)bytes.size(),
            &w[0], wlen);
        return w;
    }

    // Fall back to ANSI (GBK)
    wlen = MultiByteToWideChar(CP_ACP, 0, bytes.c_str(), (int)bytes.size(), NULL, 0);
    if (wlen > 0) {
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, bytes.c_str(), (int)bytes.size(),
            &w[0], wlen);
        return w;
    }
    return std::wstring();
}

// Reads a REG_SZ value from HKLM under both 64-bit and 32-bit views.
static std::wstring ReadRegString(HKEY root, const std::wstring& subKey,
    const wchar_t* valueName) {
    HKEY hKey = NULL;
    REGSAM views[] = { KEY_READ | KEY_WOW64_64KEY, KEY_READ | KEY_WOW64_32KEY };

    for (REGSAM view : views) {
        if (RegOpenKeyExW(root, subKey.c_str(), 0, view, &hKey) == ERROR_SUCCESS) {
            wchar_t buf[1024] = { 0 };
            DWORD size = sizeof(buf);
            DWORD type = 0;
            LSTATUS st = RegQueryValueExW(hKey, valueName, NULL, &type,
                (LPBYTE)buf, &size);
            RegCloseKey(hKey);
            if (st == ERROR_SUCCESS && type == REG_SZ) {
                return std::wstring(buf);
            }
        }
    }
    return L"";
}

// ============================================================
// VdiskLocator 
// ============================================================
std::wstring VdiskLocator::Locate() {
    std::vector<std::wstring> candidates;

    // 1. Registry: VMware Workstation (64-bit view)
    {
        std::wstring p = ReadRegString(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\VMware, Inc.\\VMware Workstation",
            L"InstallPath");
        if (!p.empty())
            candidates.push_back(EnsureTrailingSlash(p) + L"vmware-vdiskmanager.exe");
    }

    // 2. Registry: VMware Workstation (32-bit view)
    {
        std::wstring p = ReadRegString(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\WOW6432Node\\VMware, Inc.\\VMware Workstation",
            L"InstallPath");
        if (!p.empty())
            candidates.push_back(EnsureTrailingSlash(p) + L"vmware-vdiskmanager.exe");
    }

    // 3. Registry: VMware Player (64-bit view)
    {
        std::wstring p = ReadRegString(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\VMware, Inc.\\VMware Player",
            L"InstallPath");
        if (!p.empty())
            candidates.push_back(EnsureTrailingSlash(p) + L"vmware-vdiskmanager.exe");
    }

    // 4. Registry: VMware Player (32-bit view)
    {
        std::wstring p = ReadRegString(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\WOW6432Node\\VMware, Inc.\\VMware Player",
            L"InstallPath");
        if (!p.empty())
            candidates.push_back(EnsureTrailingSlash(p) + L"vmware-vdiskmanager.exe");
    }

    // 5. Default install paths 
    {
        wchar_t pf[MAX_PATH] = { 0 };
        wchar_t pfx86[MAX_PATH] = { 0 };
        GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
        GetEnvironmentVariableW(L"ProgramFiles(x86)", pfx86, MAX_PATH);

        if (pf[0]) {
            std::wstring base(pf);
            candidates.push_back(base + L"\\VMware\\VMware Workstation\\vmware-vdiskmanager.exe");
            candidates.push_back(base + L"\\VMware\\VMware Player\\vmware-vdiskmanager.exe");
        }
        if (pfx86[0]) {
            std::wstring base(pfx86);
            candidates.push_back(base + L"\\VMware\\VMware Workstation\\vmware-vdiskmanager.exe");
            candidates.push_back(base + L"\\VMware\\VMware Player\\vmware-vdiskmanager.exe");
        }
    }

    // 6. PATH 
    {
        wchar_t pathEnv[32768] = { 0 };
        if (GetEnvironmentVariableW(L"PATH", pathEnv, 32768)) {
            std::wstring paths(pathEnv);
            size_t start = 0;
            while (start <= paths.size()) {
                size_t end = paths.find(L';', start);
                std::wstring one = (end == std::wstring::npos)
                    ? paths.substr(start)
                    : paths.substr(start, end - start);

                while (!one.empty() && (one.front() == L'"' || one.front() == L' '))
                    one.erase(one.begin());
                while (!one.empty() && (one.back() == L'"' || one.back() == L' '))
                    one.pop_back();

                if (!one.empty())
                    candidates.push_back(EnsureTrailingSlash(one) + L"vmware-vdiskmanager.exe");

                if (end == std::wstring::npos) break;
                start = end + 1;
            }
        }
    }

    // 7. CWD 
    {
        wchar_t cwd[MAX_PATH] = { 0 };
        if (GetCurrentDirectoryW(MAX_PATH, cwd)) {
            candidates.push_back(std::wstring(cwd) + L"\\vmware-vdiskmanager.exe");
        }
    }

    for (const auto& c : candidates) {
        if (FileExists(c)) return c;
    }
    return L"";
}

// ============================================================
// ProcessRunner（CreateProcessW + UTF-8/ANSI 智能解码）
// ============================================================
bool ProcessRunner::RunCommand(
    const std::wstring& cmdLine,
    const std::wstring& workDir,
    std::function<void(const std::wstring&)> onOutput,
    std::function<bool()> onCancel)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa) };
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = NULL;
    HANDLE hWritePipe = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return false;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = { 0 };

    // CreateProcessW requires a writable wide buffer 
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(
        NULL,
        cmdBuf.data(),
        NULL, NULL, TRUE,
        CREATE_NO_WINDOW,
        NULL,
        workDir.empty() ? NULL : workDir.c_str(),
        &si, &pi);

    CloseHandle(hWritePipe);

    if (!ok) {
        CloseHandle(hReadPipe);
        return false;
    }

    std::string lineBuf;
    char readBuf[4096];
    DWORD bytesRead = 0;
    bool canceled = false;

    auto emitLine = [&](const std::string& line) {
        if (!onOutput || line.empty()) return;
        std::wstring w = BytesToWide(line);
        if (!w.empty()) onOutput(w);
        };

    while (true) {
        if (onCancel && onCancel()) {
            TerminateProcess(pi.hProcess, 1);
            canceled = true;
            break;
        }

        DWORD avail = 0;
        if (!PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL)) {
            break;
        }

        if (avail > 0) {
            if (ReadFile(hReadPipe, readBuf, sizeof(readBuf), &bytesRead, NULL)
                && bytesRead > 0) {
                lineBuf.append(readBuf, bytesRead);

                size_t pos = 0;
                while ((pos = lineBuf.find('\n')) != std::string::npos) {
                    std::string line = lineBuf.substr(0, pos);
                    lineBuf.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    emitLine(line);
                }
            }
        }
        else {
            DWORD exitCode = STILL_ACTIVE;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            if (exitCode != STILL_ACTIVE) {
                while (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                    if (ReadFile(hReadPipe, readBuf, sizeof(readBuf), &bytesRead, NULL)
                        && bytesRead > 0) {
                        lineBuf.append(readBuf, bytesRead);
                    }
                    else {
                        break;
                    }
                }
                break;
            }
            Sleep(50);
        }
    }

    if (!lineBuf.empty()) {
        if (lineBuf.back() == '\r') lineBuf.pop_back();
        emitLine(lineBuf);
    }

    WaitForSingleObject(pi.hProcess, 5000);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    return (exitCode == 0) && !canceled;
}