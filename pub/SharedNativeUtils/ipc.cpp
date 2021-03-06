#include "pch.h"
#include <vector>
#include <thread>
#include "ipc.h"
#include "TMProcess.h"

namespace ipc
{
HRESULT Send(const std::string_view msg, const Target& target) noexcept
{
    return Send(::GetStdHandle(STD_OUTPUT_HANDLE), msg, target);
};

HRESULT Send(HANDLE out, const std::string_view msg, const Target& target) noexcept
try
{
    RETURN_HR_IF_MSG(E_FAIL, target.Service == KnownService::All, "Can't send IPC msg to 'All'");

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

    // Needs sequential access to the pipe to not interleave messages from multiple threads.
    auto guard = lock.lock_exclusive();

    DWORD written = 0;
    RETURN_IF_WIN32_BOOL_FALSE(::WriteFile(out, buf.data(), (DWORD)size, &written, nullptr));

    RETURN_HR_IF_MSG(E_FAIL, written != (DWORD)size, "ipc::Send failed to send all bytes");

    return S_OK;
}
CATCH_RETURN();

HRESULT SendDiagMsg(const std::string_view msg) noexcept
try
{
    static wil::srwlock lock;
    // Needs sequential access to the pipe to not interleave messages from multiple threads.
    auto guard = lock.lock_exclusive();

    DWORD size    = 1 + (DWORD)msg.size(); // zero term
    DWORD written = 0;
    RETURN_IF_WIN32_BOOL_FALSE(::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), msg.data(), size, &written, nullptr));

    RETURN_HR_IF_MSG(E_FAIL, written != size, "ipc::SendDiagMsg failed to send all bytes");

    return S_OK;
}
CATCH_RETURN();

HRESULT StartRead(
    std::jthread& reader, std::function<bool(const std::string_view msg, const Target& target)> onMessage) noexcept
{
    return StartRead(::GetStdHandle(STD_INPUT_HANDLE), reader, onMessage, ::GetCurrentProcessId());
}

HRESULT StartRead(HANDLE in, std::jthread& reader,
    std::function<bool(const std::string_view msg, const Target& target)> onMessage, DWORD pid) noexcept
try
{
    reader = std::jthread([=](std::stop_token stoken) {
        Process::SetThreadName(std::format(L"TM-IpcReader-{}", pid).c_str());
        DWORD size = 0, read = 0;
        while (
            !stoken.stop_requested() && ::ReadFile(in, &size, 4, &read, nullptr) && read == 4 && size > sizeof(Target))
        {
            std::vector<uint8_t> buf;
            buf.resize(size);

            if (!stoken.stop_requested() && ::ReadFile(in, buf.data(), size, &read, nullptr) && read == size)
            {
                Target target {*(Guid*)&buf[0], *(DWORD*)&buf[sizeof(Guid)]};

                std::string_view msg((const char*)&buf[sizeof(Target)], size - sizeof(Target) - 1);

                if (onMessage(msg, target))
                    return;
            }
        }
    });
    return S_OK;
}
CATCH_RETURN();

}
