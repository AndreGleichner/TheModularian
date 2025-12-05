#include "pch.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "ModuleHost.h"
#include "TMProcess.h"
#include "ipc.h"
#include "HostMsg.h"
#include "FileImage.h"
#include "ModuleBase.h"

int ModuleHost::Run()
{
    std::jthread reader;
    FAIL_FAST_IF_FAILED(ipc::StartRead(reader, [&](const std::string_view msg, const ipc::Target& target) {
        return OnMessageFromBroker(msg, target) == S_FALSE;
    }));

    terminate_.wait();

    // while (!::IsDebuggerPresent())
    //{
    //     ::Sleep(1000);
    // }
    //::DebugBreak();

    reader.request_stop();

    if (reader.joinable())
        reader.join();

    return 0;
}

HRESULT ModuleHost::OnMessageFromBroker(const std::string_view msg, const ipc::Target& target)
try
{
    if (spdlog::should_log(spdlog::level::trace))
    {
        std::string m = msg.data();
        std::erase_if(m, [](char c) { return c == '\r' || c == '\n'; });
        spdlog::trace("RX-H: {} for {}", m, Strings::ToUtf8(target.ToString()));
    }

    /*while (!::IsDebuggerPresent())
    {
        ::Sleep(1000);
    }
    ::DebugBreak();*/

    if (target.Service == ipc::KnownService::HostInit)
    {
        // The Broker sent a unique service guid we need to remember to e.g. handle moduleload/unload.
        auto       j    = json::parse(msg);
        const auto init = j.get<ipc::HostInitMsg>();

        FAIL_FAST_IF_MSG(!target_.Equals(ipc::Target()), "Alread processed an init message before");

        target_    = ipc::Target(init.Service);
        groupName_ = init.GroupName;
    }
    else
    {
        FAIL_FAST_IF_MSG(target_.Equals(ipc::Target()), "Host not initialized yet");

        if (target.Service == target_.Service)
        {
            auto       j       = json::parse(msg);
            const auto hostMsg = j.get<ipc::HostCmdMsg>();

            switch (hostMsg.Cmd)
            {
                case ipc::HostCmdMsg::Cmd::Terminate:
                {
                    if (managedHost_)
                    {
                        managedHost_->Send(msg, ipc::Target(ipc::KnownService::ManagedHost));
                    }

                    terminate_.SetEvent();
                    return S_FALSE; // exit stdin read-loop
                }

                case ipc::HostCmdMsg::Cmd::CtrlModule:
                {
                    auto       ja   = json::parse(hostMsg.Args);
                    const auto args = ja.get<ipc::HostCtrlModuleArgs>();

                    if (args.Cmd == ipc::HostCtrlModuleArgs::Cmd::Load)
                        LoadModule(ToUtf16(args.Module));
                    else
                        UnloadModule(ToUtf16(args.Module));
                    break;
                }

                default:
                {
                    SPDLOG_ERROR("Host received invalid command {}", static_cast<int>(hostMsg.Cmd));
                }
            }
        }
        else
        {
            // Broadcast to all loaded modules.
            for (auto& mod : nativeModules_)
            {
                LOG_IF_FAILED(mod->Send(msg, target));
            }

            if (managedHost_)
            {
                managedHost_->Send(msg, target);
            }
        }
    }

    return S_OK;
}
CATCH_RETURN()

HRESULT ModuleHost::LoadModule(const std::wstring& name) noexcept
try
{
    // while (!::IsDebuggerPresent())
    //{
    //     ::Sleep(1000);
    // }
    //::DebugBreak();

    auto path = ModuleBase::PathFor(name, true);

    // Auto-detect how a given module DLL should be loaded.

    auto kind = FileImage::GetKind(path.c_str());
    if (kind == FileImage::Kind::Unknown)
    {
        // managed assemblies are typically AnyCPU, so no need to have both 32 and 64bit versions.
        path = ModuleBase::PathFor(name, false);
        kind = FileImage::GetKind(path.c_str());
        if (kind == FileImage::Kind::Unknown)
            return E_FAIL;
    }

    // Managed assembly may be marked as a PE32 image although it can be loaded into a 64bit host.
    if (AnyBitSet(kind, FileImage::Kind::Managed))
    {
        if (AnyBitSet(kind, FileImage::Kind::Exe))
            return LoadManagedExeModule(path);

        return LoadManagedDllModule(path);
    }

    // Native modules require a matching bitness of the host.
#if _WIN64
    if (AnyBitSet(kind, FileImage::Kind::Bitness32))
        return E_FAIL;
#else
    if (AnyBitSet(kind, FileImage::Kind::Bitness64))
        return E_FAIL;
#endif

    return LoadNativeModule(path);
}
CATCH_RETURN();

HRESULT ModuleHost::UnloadModule(const std::wstring& name) noexcept
try
{
    // try to find a native module with given name
    auto mod = std::find_if(nativeModules_.begin(), nativeModules_.end(),
        [&](const std::unique_ptr<NativeModule>& m) { return wcscmp(m->path_.stem().c_str(), name.c_str()) == 0; });

    if (mod != nativeModules_.end())
    {
        HRESULT hr = (*mod)->Unload();
        nativeModules_.erase(mod);
        RETURN_HR_MSG(hr, "native module unload failed %ls", name.c_str());
    }

    if (managedHost_ != nullptr)
    {
        RETURN_IF_FAILED(managedHost_->UnloadModule(name));
    }
    return S_OK;
}
CATCH_RETURN();

HRESULT ModuleHost::LoadNativeModule(const std::filesystem::path& path) noexcept
try
{
    auto mod = std::make_unique<NativeModule>(path);
    RETURN_IF_FAILED(mod->Load());

    nativeModules_.push_back(std::move(mod));

    return S_OK;
}
CATCH_RETURN();

HRESULT ModuleHost::LoadManagedDllModule(const std::filesystem::path& path) noexcept
try
{
    if (!managedHost_)
    {
        managedHost_ = std::make_unique<ManagedHost>(this);
        managedHost_->RunAsync();
    }
    RETURN_IF_FAILED(managedHost_->LoadModule(path));
    return S_OK;
}
CATCH_RETURN();

HRESULT ModuleHost::LoadManagedExeModule(const std::filesystem::path& path) noexcept
try
{
    if (managedHost_ != nullptr)
        return S_FALSE;

    managedHost_ = std::make_unique<ManagedHost>(this, path.c_str());
    managedHost_->RunAsync();

    return S_OK;
}
CATCH_RETURN();
