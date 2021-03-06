#include "pch.h"
#include <Windows.h>
#include <process.h>

#include "TMProcess.h"
#include <wil/stl.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>
#include "string_extensions.h"

namespace Process
{
void Enumerate(std::function<EnumerateCallbackResult(PPROCESSENTRY32W)> callback)
{
    wil::unique_tool_help_snapshot processSnap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 956));
    FAIL_FAST_IF_MSG(!processSnap, "Failed to create processes snapshot");

    PROCESSENTRY32W pe32 {sizeof(PROCESSENTRY32W)};

    FAIL_FAST_IF_WIN32_BOOL_FALSE(::Process32FirstW(processSnap.get(), &pe32));

    do
    {
        if (callback(&pe32) == EnumerateCallbackResult::Cancel)
            break;
    } while (::Process32NextW(processSnap.get(), &pe32));
}

// A Windows service's parent process is named services.exe and running in session 0.
// See here how its done in .Net:
// https://github.com/dotnet/extensions/blob/f4066026ca06984b07e90e61a6390ac38152ba93/src/Hosting/WindowsServices/src/WindowsServiceHelpers.cs#L26-L31
// Or an equivalent go impl: https://github.com/golang/sys/blob/0d417f636930/windows/svc/security.go#L77
bool IsWindowsService()
{
    static INIT_ONCE g_init {};
    static bool      g_isWindowsService = false;

    wil::init_once(g_init, [] {
        DWORD myPid = ::GetCurrentProcessId();

        Enumerate([&](PPROCESSENTRY32W pe32) {
            if (pe32->th32ProcessID == myPid)
            {
                DWORD parentSessionId;
                if (::ProcessIdToSessionId(pe32->th32ParentProcessID, &parentSessionId) && parentSessionId == 0)
                {
                    // Opening the services.exe process requires admin, so if it fails we're anyhow not a service.
                    wil::unique_handle parent(
                        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pe32->th32ParentProcessID));
                    if (parent)
                    {
                        std::wstring imagePath;
                        if (SUCCEEDED_LOG(wil::QueryFullProcessImageNameW(parent.get(), 0, imagePath)))
                        {
                            std::filesystem::path p(imagePath);
                            g_isWindowsService = 0 == _wcsicmp(L"services.exe", p.filename().c_str());
                        }
                    }
                }
                return EnumerateCallbackResult::Cancel;
            }
            return EnumerateCallbackResult::Continue;
        });
    });

    return g_isWindowsService;
}

bool IsProtectedService()
{
    static INIT_ONCE g_init {};
    static bool      g_isProtectedService = false;

    wil::init_once(g_init, [] {
        if (IsWindowsService())
        {
            // https://docs.microsoft.com/en-us/windows/win32/procthread/isolated-user-mode--ium--processes

            // Alternatives:
            // https://docs.microsoft.com/en-us/windows/win32/procthread/zwqueryinformationprocess
            // ZwQueryInformationProcess(ProcessProtectionInformation)
            // or
            // https://docs.microsoft.com/en-us/windows/win32/procthread/isolated-user-mode--ium--processes
            // ZwQueryInformationProcess(ProcessBasicInformation,sizeof(PROCESS_EXTENDED_BASIC_INFORMATION ))
            PROCESS_PROTECTION_LEVEL_INFORMATION ppl {0};
            if (::GetProcessInformation(::GetCurrentProcess(), ProcessProtectionLevelInfo, &ppl, sizeof(ppl)) &&
                ppl.ProtectionLevel != PROTECTION_LEVEL_NONE)
                g_isProtectedService = true;
        }
        else
        {
            g_isProtectedService = false;
        }
    });

    return g_isProtectedService;
}

ParentProcessInfo ParentProcess()
{
    static INIT_ONCE         g_init {};
    static ParentProcessInfo g_parent {(DWORD)-1, (DWORD)-1};

    wil::init_once(g_init, [] {
        DWORD myPid = ::GetCurrentProcessId();

        Enumerate([&](PPROCESSENTRY32W pe32) {
            if (pe32->th32ProcessID == myPid)
            {
                g_parent.Pid       = pe32->th32ProcessID;
                g_parent.ParentPid = pe32->th32ParentProcessID;

                return EnumerateCallbackResult::Cancel;
            }
            return EnumerateCallbackResult::Continue;
        });

        FAIL_FAST_IF_MSG(g_parent.ParentPid == (DWORD)-1, "ParentProcess() failed to find ParentPid");

        Enumerate([&](PPROCESSENTRY32W pe32) {
            if (pe32->th32ProcessID == g_parent.ParentPid)
            {
                g_parent.ExePath = pe32->szExeFile;

                return EnumerateCallbackResult::Cancel;
            }
            return EnumerateCallbackResult::Continue;
        });
    });
    FAIL_FAST_IF_MSG(g_parent.ExePath.empty(), "ParentProcess() failed to find ExePath");
    return g_parent;
}

std::filesystem::path ImagePath()
{
    static INIT_ONCE             g_init {};
    static std::filesystem::path g_imageFullPath;

    wil::init_once(g_init, [] {
        std::wstring path;
        FAIL_FAST_IF_FAILED(wil::GetModuleFileNameW<std::wstring>(nullptr, path));
        g_imageFullPath = path;
    });
    return g_imageFullPath;
}

std::filesystem::path ImagePath(DWORD pid)
{
    wil::unique_handle parent(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid));
    if (parent)
    {
        std::wstring imagePath;
        if (SUCCEEDED_LOG(wil::QueryFullProcessImageNameW(parent.get(), 0, imagePath)))
        {
            return std::filesystem::path(imagePath);
        }
    }
    return std::filesystem::path();
}

std::wstring Name()
{
    static INIT_ONCE             g_init {};
    static std::filesystem::path g_imageBasename;

    wil::init_once(g_init, [] {
        std::wstring path;
        FAIL_FAST_IF_FAILED(wil::GetModuleFileNameW<std::wstring>(nullptr, path));
        std::filesystem::path p(path);
        g_imageBasename = p.stem();
    });
    return g_imageBasename;
}

namespace
{
void SetThreadNameViaException(PCSTR name)
{
#pragma pack(push, 8)
    struct tagTHREADNAME_INFO
    {
        DWORD  dwType;     // Must be 0x1000.
        LPCSTR szName;     // Pointer to name (in user addr space).
        DWORD  dwThreadID; // Thread ID (-1=caller thread).
        DWORD  dwFlags;    // Reserved for future use, must be zero.
    } info {0x1000, name, 0xffffffff, 0};
#pragma pack(pop)

    __try
    {
        ::RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
}

// https://docs.microsoft.com/en-us/visualstudio/debugger/how-to-set-a-thread-name-in-native-code
void SetThreadName(PCWSTR name)
{
    HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel)
    {
        auto getThreadDescription = (decltype(GetThreadDescription)*)::GetProcAddress(kernel, "GetThreadDescription");
        auto setThreadDescription = (decltype(SetThreadDescription)*)::GetProcAddress(kernel, "SetThreadDescription");

        if (getThreadDescription && setThreadDescription)
        {
            // Only change once
            PWSTR   desc = nullptr;
            HRESULT hr   = getThreadDescription(::GetCurrentThread(), &desc);
            if (SUCCEEDED(hr))
            {
                if (*desc == L'\0')
                    setThreadDescription(::GetCurrentThread(), name);
                ::LocalFree(desc);
            }
            else
            {
                setThreadDescription(::GetCurrentThread(), name);
            }
        }
        else
        {
            SetThreadNameViaException(Strings::ToUtf8(name).c_str());
        }
    }
}
std::wstring ThreadName()
{
    std::wstring name;
    HMODULE      kernel = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel)
    {
        auto getThreadDescription = (decltype(GetThreadDescription)*)::GetProcAddress(kernel, "GetThreadDescription");

        if (getThreadDescription)
        {
            // Only change once
            PWSTR   desc = nullptr;
            HRESULT hr   = getThreadDescription(::GetCurrentThread(), &desc);
            if (SUCCEEDED(hr))
            {
                name = desc;

                ::LocalFree(desc);
            }
            else
            {
                name = L"-";
            }
        }
        else
        {
        }
    }
    return name;
}
}
