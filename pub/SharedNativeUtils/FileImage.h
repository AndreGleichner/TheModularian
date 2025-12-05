#pragma once
#include <Windows.h>
#include <magic_enum/magic_enum.hpp>

namespace FileImage
{
enum class Kind
{
    Unknown = 0,

    // One of:
    Dll = 0x01,
    Exe = 0x02,

    // One of:
    Bitness32 = 0x04,
    Bitness64 = 0x08,

    // One of:
    Native  = 0x10,
    Managed = 0x20
};

Kind GetKind(PCWSTR path);
}
