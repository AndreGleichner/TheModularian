#include "pch.h"
#include "ServiceBase.h"
#include "TMProcess.h"
#include <Windows.h>
#include <dbt.h>

ServiceBase* ServiceBase::_this = nullptr;

ServiceBase::ServiceBase(const ServiceTraits& traits) : Traits(traits)
{
    Traits = traits;

    SPDLOG_TRACE(L"Creating service instance '{}' depending on '{}'", Traits.Name, Traits.Dependencies);
    FAIL_FAST_IF_MSG(_this != nullptr, "There shall be only one ServiceBase based service in a module.");

    _this = this;
}

#pragma region CmdlineActions

bool ServiceBase::CmdlineAction(PCWSTR commandLine, const ServiceTraits& traits, int& exitCode)
{
    std::wstring cmdLine(commandLine);

    struct Action
    {
        std::wstring                                               Arg;
        std::wstring                                               Option;
        std::function<HRESULT(const ServiceTraits&, std::wstring)> Func;
    };
    // clang-format off
    Action actions[] =
    {
        {L"register",   L"ppl", Register}, // e.g. "--register" or "--register --ppl"
        {L"unregister", L"",    Unregister},
        {L"start",      L"",    Start},
        {L"stop",       L"",    Stop},
        {L"restart",    L"",    Restart}
    };
    // clang-format on

    std::wsmatch match;
    for (auto a : actions)
    {
        std::wstring pat;
        if (a.Option.empty())
            pat = std::format(L"^[ ]*--{}[ ]*$", a.Arg);
        else
            pat = std::format(L"^[ ]*--{}([ ]+--{}|)[ ]*$", a.Arg, a.Option);

        std::wregex re(pat, std::regex::icase);
        if (std::regex_match(cmdLine, match, re) && (match.size() == 1 || match.size() == 2 || match.size() == 3))
        {
            HRESULT hres;
            if (a.Option.empty())
                hres = a.Func(traits, L"");
            else
                hres = a.Func(traits, match.size() == 2 ? L"0" : L"1");

            SPDLOG_INFO(L"CmdlineAction returned: {}", HResultToString(hres));
            if (FAILED(hres))
                exitCode = 1;

            return true;
        }
    }
    return false;
}

HRESULT ServiceBase::Register(const ServiceTraits& traits, const std::wstring& ppl)
{
    SPDLOG_TRACE(L"->");

    std::wstring imagePath(L"\"" + std::wstring(wil::GetModuleFileNameW().get()) + L"\"");

    wil::unique_schandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    RETURN_LAST_ERROR_IF_NULL_MSG(scm.get(), "OpenSCManagerW failed");

    wil::unique_schandle service(
        ::OpenServiceW(scm.get(), traits.Name.c_str(), SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG));

    if (service)
    {
        SPDLOG_TRACE(L"Already installed -> ensure image path is correct");

        // read current service config
        std::vector<byte> configBuffer;
        DWORD             configSize;
        (void)::QueryServiceConfigW(service.get(), nullptr, 0, &configSize);
        RETURN_HR_IF(HRESULT_FROM_WIN32(::GetLastError()), ::GetLastError() != ERROR_INSUFFICIENT_BUFFER);
        configBuffer.resize(configSize);

        auto config = (QUERY_SERVICE_CONFIGW*)configBuffer.data();
        RETURN_IF_WIN32_BOOL_FALSE(::QueryServiceConfigW(service.get(), config, configSize, &configSize));

        if (imagePath != config->lpBinaryPathName)
        {
            // store changed image path
            config->lpBinaryPathName = imagePath.data();

            RETURN_IF_WIN32_BOOL_FALSE(::ChangeServiceConfigW(service.get(), config->dwServiceType, config->dwStartType,
                config->dwErrorControl, config->lpBinaryPathName, config->lpLoadOrderGroup, nullptr,
                config->lpDependencies, config->lpServiceStartName, nullptr, config->lpDisplayName));
        }
    }
    else
    {
        // create new service

        std::wstring serviceDependencies(traits.Dependencies);
        serviceDependencies += L"|";
        std::replace(serviceDependencies.begin(), serviceDependencies.end(), L'|', L'\0');

        wil::unique_schandle serv(::CreateServiceW(scm.get(), traits.Name.c_str(), traits.Displayname.c_str(),
            SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START /*SERVICE_AUTO_START*/,
            SERVICE_ERROR_NORMAL, imagePath.c_str(), nullptr, nullptr, serviceDependencies.c_str(), nullptr, L""));

        auto                d    = traits.Description;
        SERVICE_DESCRIPTION desc = {(PTSTR)d.c_str()};

        RETURN_IF_WIN32_BOOL_FALSE(::ChangeServiceConfig2W(serv.get(), SERVICE_CONFIG_DESCRIPTION, &desc));

        if (ppl == L"1")
        {
            SPDLOG_TRACE(L"Register as AM-PPL");

            // TODO: the doc says we should call SetServiceObjectSecurity()
            // https://docs.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-setserviceobjectsecurity

            SERVICE_LAUNCH_PROTECTED_INFO protInfo {SERVICE_LAUNCH_PROTECTED_ANTIMALWARE_LIGHT};
            RETURN_IF_WIN32_BOOL_FALSE(::ChangeServiceConfig2W(serv.get(), SERVICE_CONFIG_LAUNCH_PROTECTED, &protInfo));
        }
    }

    RETURN_IF_FAILED(AddEventSource(traits));

    SPDLOG_TRACE(L"<-");
    return S_OK;
}

HRESULT ServiceBase::Unregister(const ServiceTraits& traits, const std::wstring& unused)
{
    SPDLOG_TRACE(L"->");

    // Stop and Delete; ignore return value, so that further unreg steps may run
    HRESULT res1 = StopImpl(traits, true);

    HRESULT res2 = RemoveEventSource(traits);

    SPDLOG_TRACE(L"<-");

    RETURN_IF_FAILED_MSG(res1, "Unregister failed in StopImpl");
    RETURN_IF_FAILED_MSG(res2, "Unregister failed in RemoveEventSource");

    return S_OK;
}

HRESULT ServiceBase::Start(const ServiceTraits& traits, const std::wstring& unused)
{
    SPDLOG_TRACE(L"->");

    wil::unique_schandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    RETURN_LAST_ERROR_IF_NULL_MSG(scm.get(), "OpenSCManagerW failed");

    wil::unique_schandle service(
        ::OpenServiceW(scm.get(), traits.Name.c_str(), SERVICE_STOP | SERVICE_START | SERVICE_CHANGE_CONFIG));
    RETURN_LAST_ERROR_IF_NULL_MSG(service.get(), "OpenServiceW failed");

    SPDLOG_INFO(L"Start service...");
    if (!::StartServiceW(service.get(), 0, nullptr))
    {
        SPDLOG_INFO(L"StartService failed: {}", LastErrorToString());

        // change to system context and try again
        RETURN_IF_WIN32_BOOL_FALSE(::ChangeServiceConfigW(service.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_NO_CHANGE,
            SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, L"LocalSystem", L"", nullptr));

        RETURN_IF_WIN32_BOOL_FALSE(::StartServiceW(service.get(), 0, nullptr));
    }
    SPDLOG_TRACE(L"<-");
    return S_OK;
}

HRESULT ServiceBase::Stop(const ServiceTraits& traits, const std::wstring& unused)
{
    SPDLOG_TRACE(L"->");

    RETURN_IF_FAILED(StopImpl(traits, false));

    SPDLOG_TRACE(L"<-");
    return S_OK;
}

HRESULT ServiceBase::StopImpl(const ServiceTraits& traits, bool deleteService)
{
    SPDLOG_TRACE(L"->");

    wil::unique_schandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    RETURN_LAST_ERROR_IF_NULL_MSG(scm.get(), "OpenSCManagerW failed");

    wil::unique_schandle service(::OpenServiceW(
        scm.get(), traits.Name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS | (deleteService ? DELETE : 0)));
    if (!service.get())
        return S_FALSE;

    std::vector<byte> statusBuffer;
    DWORD             statusSize;
    (void)::QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, nullptr, 0, &statusSize);
    RETURN_HR_IF(HRESULT_FROM_WIN32(::GetLastError()), ::GetLastError() != ERROR_INSUFFICIENT_BUFFER);
    statusBuffer.resize(statusSize);

    auto statusProcess = (SERVICE_STATUS_PROCESS*)statusBuffer.data();
    RETURN_IF_WIN32_BOOL_FALSE(
        ::QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)statusProcess, statusSize, &statusSize));

    SPDLOG_TRACE("Service state: {}", magic_enum::enum_name((State)statusProcess->dwCurrentState));

    if (statusProcess->dwCurrentState != SERVICE_STOPPED && statusProcess->dwCurrentState != SERVICE_STOP_PENDING)
    {
        SPDLOG_INFO(L"Issue service stop request");
        // Not yet stopped, so issue a stop request.
        SERVICE_STATUS status;
        ::ControlService(service.get(), SERVICE_CONTROL_STOP, &status);
    }

    if (statusProcess->dwProcessId)
    {
        // Ensure the process actually stopped.
        wil::unique_process_handle process(::OpenProcess(SYNCHRONIZE, FALSE, statusProcess->dwProcessId));
        if (process)
        {
            SPDLOG_TRACE(L"Wait until the service process fade away");
            if (::WaitForSingleObject(process.get(), 10000) == WAIT_OBJECT_0)
                SPDLOG_TRACE(L"Service process finished");
        }
        else
        {
            const DWORD waitMilliSec = 3000;
            SPDLOG_TRACE(L"Couldn't open service process => wait {} ms", waitMilliSec);
            ::Sleep(waitMilliSec);
        }
    }

    if (deleteService)
    {
        RETURN_IF_WIN32_BOOL_FALSE(::DeleteService(service.get()));
    }

    SPDLOG_TRACE(L"<-");

    return S_OK;
}

HRESULT ServiceBase::Restart(const ServiceTraits& traits, const std::wstring& unused)
{
    SPDLOG_TRACE(L"->");

    RETURN_IF_FAILED(StopImpl(traits, false));

    wil::unique_schandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    RETURN_LAST_ERROR_IF_NULL_MSG(scm.get(), "OpenSCManagerW failed");

    wil::unique_schandle service(
        ::OpenServiceW(scm.get(), traits.Name.c_str(), SERVICE_STOP | SERVICE_START | SERVICE_CHANGE_CONFIG));
    RETURN_LAST_ERROR_IF_NULL_MSG(service.get(), "OpenServiceW failed");

    SPDLOG_INFO(L"Restart service...");
    if (!::StartServiceW(service.get(), 0, nullptr))
    {
        SPDLOG_INFO(L"StartService failed: {}", LastErrorToString());

        // change to system context and try again
        RETURN_IF_WIN32_BOOL_FALSE(::ChangeServiceConfigW(service.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_NO_CHANGE,
            SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, L"LocalSystem", L"", nullptr));

        RETURN_IF_WIN32_BOOL_FALSE(::StartServiceW(service.get(), 0, nullptr));
    }
    SPDLOG_TRACE(L"<-");
    return EXIT_SUCCESS;
}

HRESULT ServiceBase::AddEventSource(const ServiceTraits& traits)
{
    if (!traits.UsingEventLog())
        return S_OK;

    auto imagePath = wil::GetModuleFileNameW();

    wil::unique_hkey service;
    RETURN_IF_WIN32_ERROR(::RegCreateKeyW(HKEY_LOCAL_MACHINE,
        (LR"(SYSTEM\CurrentControlSet\Services\EventLog\Application\)" + traits.Name).c_str(), &service));

    RETURN_IF_WIN32_ERROR(::RegSetValueExW(service.get(), L"EventMessageFile", 0, REG_EXPAND_SZ, (BYTE*)imagePath.get(),
        2 * ((DWORD)wcslen(imagePath.get()) + 1)));

    //	Write TypesSupported.
    DWORD dwData = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;

    RETURN_IF_WIN32_ERROR(
        ::RegSetValueExW(service.get(), L"TypesSupported", 0, REG_DWORD, (LPBYTE)&dwData, sizeof(DWORD)));

    return S_OK;
}

HRESULT ServiceBase::RemoveEventSource(const ServiceTraits& traits)
{
    if (!traits.UsingEventLog())
        return S_OK;

    std::wstring subKey(LR"(SYSTEM\CurrentControlSet\Services\EventLog\Application\)" + traits.Name);

    RETURN_IF_WIN32_ERROR_EXPECTED(::RegDeleteKeyW(HKEY_LOCAL_MACHINE, subKey.c_str()));

    return S_OK;
}
#pragma endregion

int ServiceBase::Run()
{
    SPDLOG_TRACE(L"->");

    // SERVICE_WIN32_OWN_PROCESS services are ignoring service name given in this table.
    // Those are using the name given to CreateService().
    SERVICE_TABLE_ENTRY st[] = {{(PWSTR)L"", ServiceMain_}, {nullptr, nullptr}};

    // This is a blocking function actually dispatching service requests to the HandlerEx().
    // The function creates a new thread to invoke _ServiceMain().
    FAIL_FAST_IF_WIN32_BOOL_FALSE(::StartServiceCtrlDispatcherW(st));

    if (waitForStop_)
    {
        BOOL _ = ::UnregisterWait(waitForStop_);

        waitForStop_ = nullptr;
    }

    SPDLOG_TRACE(L"<-");

    return (int)Status.dwWin32ExitCode;
}

void WINAPI ServiceBase::ServiceMain_(_In_ DWORD argc, _In_ PWSTR* argv)
{
    // This thread got created within StartServiceCtrlDispatcherW().
    Process::SetThreadName(L"TM-ServiceMain");

    FAIL_FAST_IF_NULL(_this);

    _this->ServiceMain(argc, argv);
}

void ServiceBase::ServiceMain(_In_ DWORD argc, _In_ PWSTR* argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    SPDLOG_TRACE(L"->");

    StatusHandle = ::RegisterServiceCtrlHandlerExW(Traits.Name.c_str(), HandlerEx_, this);
    if (StatusHandle == nullptr)
    {
        LogEvent(L"Handler not installed");
        return;
    }

    // To set upon any stop request, we're waiting on it below in this function.
    stopEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    FAIL_FAST_IF_NULL(stopEvent_);

    SetServiceStatus(State::START_PENDING);

    SetServiceStatus(State::RUNNING);
    LogEvent(L"Service started");

    // Register for device notifications?
    if (AnyBitSet(Traits.AcceptControls, AcceptControls::TM_DEVICEEVENT))
    {
        DEV_BROADCAST_DEVICEINTERFACE_W notificationFilter;
        ZeroMemory(&notificationFilter, sizeof(notificationFilter));
        notificationFilter.dbcc_size       = sizeof(DEV_BROADCAST_DEVICEINTERFACE_W);
        notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

        devNotify_ = ::RegisterDeviceNotificationW(
            StatusHandle, &notificationFilter, DEVICE_NOTIFY_SERVICE_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);

        FAIL_FAST_LAST_ERROR_IF_NULL_MSG(devNotify_, "RegisterDeviceNotificationW failed");
    }

    // Potentially create background threads in derived classes.
    HRESULT hres = OnStart(argc, argv);
    if (FAILED(hres))
    {
        Status.dwWin32ExitCode = hres;
        SetServiceStatus(State::STOP_PENDING);
        ::SetEvent(stopEvent_);
    }

    FAIL_FAST_IF_WIN32_BOOL_FALSE(
        ::RegisterWaitForSingleObject(&waitForStop_, stopEvent_, ServiceStopping, this, INFINITE, WT_EXECUTEDEFAULT));

    SPDLOG_TRACE(L"<-");
}

void CALLBACK ServiceBase::ServiceStopping(_In_ PVOID lpParameter, _In_ BOOLEAN TimerOrWaitFired)
{
    UNREFERENCED_PARAMETER(TimerOrWaitFired);
    auto service = (ServiceBase*)lpParameter;

    ::CloseHandle(service->stopEvent_);
    service->stopEvent_ = nullptr;

    if (service->devNotify_)
    {
        ::UnregisterDeviceNotification(service->devNotify_);
        service->devNotify_ = nullptr;
    }

    service->OnStop();

    service->SetServiceStatus(State::STOPPED);
    service->LogEvent(L"Service stopped");
}

DWORD WINAPI ServiceBase::HandlerEx_(
    _In_ DWORD dwControl, _In_ DWORD dwEventType, _In_ PVOID lpEventData, _In_ PVOID lpContext)
{
    // This got invoked from with StartServiceCtrlDispatcherW()

    auto service = (ServiceBase*)lpContext;
    return service->HandlerEx(dwControl, dwEventType, lpEventData);
}

// https://docs.microsoft.com/en-us/windows/win32/services/service-control-handler-function
DWORD ServiceBase::HandlerEx(_In_ DWORD dwControl, _In_ DWORD dwEventType, _In_ PVOID lpEventData)
{
    SPDLOG_INFO(L"{}", dwControl);

    switch (dwControl)
    {
        case SERVICE_CONTROL_STOP:
            // OnStop will be called on ServiceMain() exit
            SetServiceStatus(State::STOP_PENDING);
            ::SetEvent(stopEvent_);
            break;

        case SERVICE_CONTROL_PAUSE:
            OnPause();
            break;

        case SERVICE_CONTROL_CONTINUE:
            OnContinue();
            break;

        case SERVICE_CONTROL_INTERROGATE:
            OnInterrogate();
            break;

        case SERVICE_CONTROL_SHUTDOWN:
            OnShutdown();
            SetServiceStatus(State::STOP_PENDING);
            ::SetEvent(stopEvent_);
            break;

        case SERVICE_CONTROL_DEVICEEVENT:
            return OnDeviceEvent(dwEventType, lpEventData);

        case SERVICE_CONTROL_HARDWAREPROFILECHANGE:
            return OnHardwareProfileChange(dwEventType);

        case SERVICE_CONTROL_POWEREVENT:
            return OnPowerEvent(
                dwEventType, (dwEventType == PBT_POWERSETTINGCHANGE) ? (POWERBROADCAST_SETTING*)lpEventData : nullptr);

        case SERVICE_CONTROL_SESSIONCHANGE:
            OnSessionChange(dwEventType, ((WTSSESSION_NOTIFICATION*)lpEventData)->dwSessionId);
            break;

        case SERVICE_CONTROL_PRESHUTDOWN:
            OnPreShutdown();
            SetServiceStatus(State::STOP_PENDING);
            ::SetEvent(stopEvent_);
            break;

        case SERVICE_CONTROL_TIMECHANGE:
            OnTimeChange((SERVICE_TIMECHANGE_INFO*)lpEventData);
            break;

        default:
            SPDLOG_TRACE(L"ERROR_CALL_NOT_IMPLEMENTED");
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
    SPDLOG_TRACE(L"<-");
    return NO_ERROR;
}

void ServiceBase::SetServiceStatus(State state)
{
    if (Status.dwCurrentState == SERVICE_STOPPED)
    {
        SPDLOG_INFO(L"<- alread stopped");
        return;
    }
    FAIL_FAST_IF_NULL(StatusHandle);

    if (state == State::START_PENDING)
        Status.dwControlsAccepted = 0;
    else
        Status.dwControlsAccepted = (DWORD)Traits.AcceptControls | SERVICE_ACCEPT_STOP;

    SPDLOG_INFO(L"-> {} accept:{:#x}", static_cast<uint32_t>(state), Status.dwControlsAccepted);

    Status.dwCurrentState = (DWORD)state;
    ::SetServiceStatus(StatusHandle, &Status);

    if (state == State::STOPPED)
        StatusHandle = nullptr; // must not be used again to call ::SetServiceStatus()
}

HRESULT ServiceBase::LogEvent(PCWSTR format, ...)
{
    if (!Traits.UsingEventLog())
        return S_OK;

    wchar_t msg[256];
    va_list pArg;

    va_start(pArg, format);
    vswprintf_s(msg, format, pArg);
    va_end(pArg);

    SPDLOG_INFO(msg);

    HANDLE hEventSource = ::RegisterEventSource(nullptr, Traits.Name.c_str());
    if (hEventSource != nullptr)
    {
        PWSTR lpszStrings[1];
        lpszStrings[0] = msg;
        ::ReportEventW(hEventSource, EVENTLOG_INFORMATION_TYPE, Traits.EventCategory, Traits.EventID, nullptr, 1, 0,
            (PCWSTR*)&lpszStrings[0], nullptr);
        ::DeregisterEventSource(hEventSource);
    }

    return S_OK;
}
