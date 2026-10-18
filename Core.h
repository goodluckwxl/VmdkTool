#pragma once 

#include <windows.h>
#include <string>
#include <functional>

// Locates vmware-vdiskmanager.exe on the local system.
class VdiskLocator {
public:
    // Returns the full path to vmware-vdiskmanager.exe, or empty string if not found.
    static std::wstring Locate();
};

// Runs an external process and captures its combined stdout/stderr output.
class ProcessRunner {
public:
    // Runs the given command line in the specified working directory.
    // Each complete output line is passed to onOutput (may be null).
    // If onCancel is not null and returns true, the process is terminated.
    // Returns true if the process exits with code 0.
    static bool RunCommand(
        const std::wstring& cmdLine,
        const std::wstring& workDir,
        std::function<void(const std::wstring&)> onOutput,
        std::function<bool()> onCancel);
};