#include "pch.h"
#include <vector>
#include <thread>
#include "ipc.h"

HANDLE g_inWrite = nullptr;
namespace ipc
{
HRESULT Send(const std::string& msg, const Target& target /*= KnownTarget::World*/) noexcept
try
{
    static wil::srwlock lock;

    size_t               size = 4 /*framing length*/ + sizeof(target) + msg.size() + 1 /*zero-term*/;
    std::vector<uint8_t> buf;
    buf.resize(size);

    size_t pos = 0;

    // store count of bytes following the length prefix
    *(DWORD*)&buf[0] = (DWORD)size - 4;
    pos += 4;

    // store target
    *(Target*)&buf[pos] = target;
    pos += sizeof(target);

    // store message
    memcpy(&buf[pos], msg.data(), msg.size() + 1);

    HANDLE h = g_inWrite;
    //::GetStdHandle(STD_OUTPUT_HANDLE);

    // Needs sequential access to the pipe to not interleave messages from multiple threads.
    auto guard = lock.lock_exclusive();

    DWORD written = 0;
    RETURN_IF_WIN32_BOOL_FALSE(::WriteFile(h, buf.data(), (DWORD)size, &written, nullptr));

    RETURN_HR_IF_MSG(E_FAIL, written != (DWORD)size, "ipc::Send failed to send all bytes");

    return S_OK;
}
CATCH_RETURN();

HRESULT StartRead(std::function<void(const std::string_view msg, const Target& target)> onMessage) noexcept
try
{
    static std::thread reader;

    reader = std::thread([=] {
        SPDLOG_INFO(L"StartRead thread");
        HANDLE h    = ::GetStdHandle(STD_INPUT_HANDLE);
        DWORD  size = 0, read = 0;
        while (::ReadFile(h, &size, 4, &read, nullptr) && read == 4 && size > sizeof(Target))
        {
            SPDLOG_INFO(L"StartRead read size {}", size);
            std::vector<uint8_t> buf;
            buf.resize(size);

            if (::ReadFile(h, buf.data(), size, &read, nullptr) && read == size)
            {
                Target target {*(DWORD*)&buf[0], *(Guid*)&buf[4]};

                std::string_view msg((const char*)&buf[sizeof(Target)], size - sizeof(Target) - 1);
                SPDLOG_INFO("StartRead read msg '{}'", msg);

                // while (!::IsDebuggerPresent())
                //{
                //     ::Sleep(1000);
                // }
                //::DebugBreak();

                onMessage(msg, target);
            }
        }
    });
    return S_OK;
}
CATCH_RETURN();

}