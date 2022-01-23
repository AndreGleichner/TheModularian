#pragma once

#include <nethost/nethost.h>
#include <nethost/coreclr_delegates.h>
#include <nethost/hostfxr.h>

#ifdef _WIN32
#    define _X(s) L##s
#else
#    define _X(s) s
#endif

#define INIT_HOSTFXR_FROM_CMDLINE 1
#define INIT_HOSTFXR_FROM_RUNTIMECONFIG 2

#define INIT_HOSTFXR_FROM INIT_HOSTFXR_FROM_CMDLINE

// documented here: https://github.com/dotnet/runtime/issues/46128
typedef int32_t(HOSTFXR_CALLTYPE* hostfxr_get_dotnet_environment_info_fn)(const char_t* dotnet_root, void* reserved,
    hostfxr_get_dotnet_environment_info_result_fn result, void* result_context);

class ManagedHost final
{
public:
    ManagedHost();
    ~ManagedHost();
    bool RunAsync();
    int  OnProgress(int progress) const;

private:
    bool LoadFxr();

#if INIT_HOSTFXR_FROM == INIT_HOSTFXR_FROM_CMDLINE
    bool InitHostContext(int argc, const char_t** argv);
    bool InitFunctionPointerFactory();
#elif INIT_HOSTFXR_FROM == INIT_HOSTFXR_FROM_RUNTIMECONFIG
    bool                                      InitHostContext(std::filesystem::path runtimeconfigPath);
    bool                                      InitFunctionPointerFactory();
#endif
    void*                        CreateFunction(const char_t* name) const;
    static void HOSTFXR_CALLTYPE ErrorWriter(const char_t* message);

    static void HOSTFXR_CALLTYPE DotnetEnvInfo(
        const struct hostfxr_dotnet_environment_info* info, void* result_context);

    void* LoadLib(const char_t* path) const;
    void* GetExport(void* h, const char* name) const;

    // hostfxr exports
#if INIT_HOSTFXR_FROM == INIT_HOSTFXR_FROM_CMDLINE
    hostfxr_initialize_for_dotnet_command_line_fn initFromCmdline_ = nullptr;
    hostfxr_run_app_fn                            runApp_          = nullptr;
    get_function_pointer_fn                       functionFactory_ = nullptr;
#elif INIT_HOSTFXR_FROM == INIT_HOSTFXR_FROM_RUNTIMECONFIG
    hostfxr_initialize_for_runtime_config_fn  initFromRuntimeconfig_ = nullptr;
    load_assembly_and_get_function_pointer_fn functionFactory_       = nullptr;
#else
#    error invalid INIT_HOSTFXR_FROM
#endif

    hostfxr_get_runtime_delegate_fn        getFunctionPointerFactory_ = nullptr;
    hostfxr_close_fn                       closeHostContext_          = nullptr;
    hostfxr_get_runtime_property_value_fn  getRuntimeProperty_        = nullptr;
    hostfxr_set_runtime_property_value_fn  setRuntimeProperty_        = nullptr;
    hostfxr_get_runtime_properties_fn      getAllRuntimeProperties_   = nullptr;
    hostfxr_set_error_writer_fn            setErrorWriter_            = nullptr;
    hostfxr_get_dotnet_environment_info_fn getDotnetEnvInfo_          = nullptr;

    hostfxr_handle hostContext_ = nullptr;

    std::filesystem::path assemblyPath_;

    // https://docs.microsoft.com/en-us/dotnet/api/system.type.assemblyqualifiedname?view=net-5.0
    // https://docs.microsoft.com/en-us/dotnet/framework/reflection-and-codedom/specifying-fully-qualified-type-names
    const char_t* dotnetType_ = _X("ManagedHost.Program, ManagedHost");

    std::thread mainThread_;
};

extern ManagedHost TheManagedHost;
