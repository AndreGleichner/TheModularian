#include "pch.h"
#include "NativeModule.h"

HRESULT NativeModule::Load()
{
    hmodule_ = wil::unique_hmodule(::LoadLibraryW(path_.c_str()));
    RETURN_HR_IF_NULL_MSG(E_FAIL, hmodule_.get(), "Failed to load native module %ls", path_.c_str());

#define LoadEntry(fn)                                                                    \
    fn##_ = reinterpret_cast<decltype(Entry::fn)*>(GetProcAddress(hmodule_.get(), #fn)); \
    RETURN_LAST_ERROR_IF_NULL_MSG(fn##_, "Failed to load module entry " #fn)

    LoadEntry(InitModule);
    LoadEntry(TermModule);
    LoadEntry(OnMessage);

    RETURN_IF_FAILED(InitModule_(this, OnMsg, OnDiag));

#undef LoadEntry
    return S_OK;
}

HRESULT NativeModule::Unload()
{
    auto freeLib = wil::scope_exit([&] { hmodule_.reset(); });

    RETURN_IF_FAILED(TermModule_());

    return S_OK;
}

HRESULT NativeModule::Send(const std::string_view msg, const ipc::Target& target) noexcept
{
    return OnMessage_(msg.data(), &target);
}

HRESULT CALLBACK NativeModule::OnMsg(void* mod, PCSTR msg, const Guid* service, DWORD session) noexcept
{
    auto m = static_cast<NativeModule*>(mod);
    RETURN_IF_FAILED(ipc::Send(msg, ipc::Target(*service, session)));
    return S_OK;
}

HRESULT CALLBACK NativeModule::OnDiag(void* mod, PCSTR msg) noexcept
{
    auto m = static_cast<NativeModule*>(mod);
    RETURN_IF_FAILED(ipc::SendDiagMsg(msg));
    return S_OK;
}
