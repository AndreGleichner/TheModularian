#include "pch.h"
#include <format>
#include <fstream>

#include <wil/result.h>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "spdlog_headers.h"

#include "string_extensions.h"
using namespace Strings;

#include "ConfStoreModule.h"
#include "ConfStore.h"
#include "env.h"
#include "TMProcess.h"

json ConfStoreModule::DefaultConfigFor(const std::string& moduleName) noexcept
{
    json conf;

    try
    {
        std::wstring          name = ToUtf16(moduleName);
        std::filesystem::path defConfPath;

        if (name == L"Broker")
        {
            defConfPath = Process::ImagePath().replace_filename(L"broker.json");
        }
        else
        {
            defConfPath = ModuleBase::PathFor(name.c_str(), false).replace_filename(name + L".json");
        }

        if (std::filesystem::exists(defConfPath))
        {
            std::ifstream     confStream(defConfPath.c_str());
            std::stringstream c;
            c << confStream.rdbuf();
            // ignoring comments
            conf = json::parse(c.str(), nullptr, true, true);
        }
    }
    catch (...)
    {
    }
    return conf;
}

namespace
{
json GetModuleConf(json& conf, const std::string& moduleName)
{
    // If module conf is not present this will create a null object, which we need to remove.
    // invoking the const operator[] wont create such null object instead would assert.
    json val = conf[json::json_pointer("/" + moduleName)];
    if (val.is_null())
    {
        // revert adding the null object
        (void)conf.erase(moduleName);
    }

    return val;
}
}

HRESULT ConfStoreModule::OnMessage(std::string_view msg, const ipc::Target& target) noexcept
try
{
    if (!target.Equals(ipc::Target(ipc::KnownService::ConfStore)))
        return S_OK;

    const auto c = json::parse(msg).get<ipc::ConfStore>();
    if (c.Cmd != ipc::ConfStore::Cmd::Query && c.Cmd != ipc::ConfStore::Cmd::Update)
    {
        SPDLOG_ERROR("Invalid ConfStore command {}", static_cast<int>(c.Cmd));
        return E_INVALIDARG;
    }

    // Sync file access in any session.

    wil::unique_mutex_failfast lock(L"Global\\ConfStore-{93D385EC-6F61-4594-9386-464E0802BAAB}");
    auto                       releaseOnExit = lock.acquire();

    auto              confDir = env::PrivateDataDir(L"conf");
    auto              file    = confDir / L"store.json";
    wil::unique_hfile confFile;
    size_t            size = 0;
    json              conf;
    bool              store = false;

    // Read current config or init to empty object.
    // Sample where Mod1/2 are module names:
    //     {
    //       "Mod1": {
    //         "ConfVal": 12
    //       },
    //       "Mod2": {
    //         "ConfVal": 22
    //       }
    //     }

    if (std::filesystem::exists(file) && (size = std::filesystem::file_size(file)) > 2)
    {
        confFile.reset(::CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

        RETURN_HR_IF(E_FAIL, !confFile);

        // Read entire content, which should be some JSON.
        std::string j;
        j.resize(size);

        DWORD bytesRead = 0;
        RETURN_IF_WIN32_BOOL_FALSE(::ReadFile(confFile.get(), j.data(), (DWORD)size, &bytesRead, nullptr));

        try
        {
            conf = json::parse(j);
        }
        catch (...)
        {
            // Seems some invalid non-JSON file content
            conf  = json({});
            store = true;
            SPDLOG_ERROR("Invalid ConfStore file content {}", j);
        }
    }
    else
    {
        SPDLOG_INFO(L"Creating new ConfStore file {}", file.c_str());

        conf = json({});

        confFile.reset(::CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        RETURN_HR_IF(E_FAIL, !confFile);
        store = true;
    }

    if (c.Cmd == ipc::ConfStore::Cmd::Query)
    {
        // Send json fragment as selected by given module name.
        // Make a JSON Pointer from module name (https://datatracker.ietf.org/doc/html/rfc6901)
        // const auto val = conf[json::json_pointer("/" + c.Args)];
        const json val = GetModuleConf(conf, c.Args);

        json        res;
        std::string r;
        if (val.is_null())
        {
            // Try reading a default from module specific file.
            res = DefaultConfigFor(c.Args);
            if (!res.is_null())
            {
                r = res.dump();
                SPDLOG_TRACE("Queried ConfStore default value {}", r);
            }
        }
        else
        {
            res[c.Args] = val;

            r = res.dump();
            SPDLOG_TRACE("Queried ConfStore value {}", r);
        }

        if (!r.empty())
            SendMsg(r.c_str(), ipc::Target(ipc::KnownService::ConfConsumer));
    }
    else
    {
        // Apply the given json patch.
        // https://datatracker.ietf.org/doc/html/rfc7386

        auto patch   = json::parse(c.Args);
        auto modName = patch.begin().key();

        // 1st check whether the module conf is already stored.
        // If not, try reading a default to get that as a base.
        const json val = GetModuleConf(conf, modName);
        if (val.is_null())
        {
            // Try reading a default from module specific file.
            json defConf = DefaultConfigFor(modName);
            if (!defConf.is_null())
            {
                // merge that default into the current conf, before applying the patch below
                conf.merge_patch(defConf);
            }
        }

        conf.merge_patch(patch);
        store = true;

        SPDLOG_TRACE("Apply ConfStore patch {}", patch.dump());

        // Read back the just patched conf for the module and broadcast it.
        json        res = {{modName, GetModuleConf(conf, modName)}};
        std::string r   = res.dump();
        if (!r.empty())
            SendMsg(r.c_str(), ipc::Target(ipc::KnownService::ConfConsumer));
    }

    if (store)
    {
        auto   s    = conf.dump(2);
        size_t size = s.length();

        (void)::SetFilePointer(confFile.get(), 0, nullptr, FILE_BEGIN);
        DWORD bytesWritten = 0;
        RETURN_IF_WIN32_BOOL_FALSE(::WriteFile(confFile.get(), s.c_str(), (DWORD)size, &bytesWritten, nullptr));
        RETURN_IF_WIN32_BOOL_FALSE(::SetEndOfFile(confFile.get()));
    }

    return S_OK;
}
CATCH_RETURN()
