#include "pch.h"

#include "FileImage.h"
#include <cstdint>
#include <share.h>
#include <vector>

#include <wil/resource.h>
#include <magic_enum/magic_enum.hpp>
using namespace magic_enum::bitwise_operators;

namespace
{
uint16_t ReadWord(const std::vector<uint8_t>& buffer, uint32_t offset)
{
    if (offset + 2 > buffer.size())
        throw std::exception();

    return *(uint16_t*)&buffer[offset];
}
uint16_t ReadDWord(const std::vector<uint8_t>& buffer, uint32_t offset)
{
    if (offset + 4 > buffer.size())
        throw std::exception();

    return *(uint32_t*)&buffer[offset];
}
}

namespace FileImage
{
Kind GetKind(PCWSTR path)
{
    // https://docs.microsoft.com/en-us/windows/win32/debug/pe-format

    // The offset of the PE header pointer in the DOS header as a DWORD.
    const uint32_t PeHeaderFileOffset = 0x3C;
    // The PE header starts with a signature "PE\0\0".
    const uint32_t PeFileSignature = 0x5A4D;

    // The offset in the PE header to a WORD with bits like IMAGE_FILE_EXECUTABLE_IMAGE and IMAGE_FILE_DLL
    const uint32_t CharacteristicsPeOffset = sizeof(PeFileSignature) + 18;

    // The offset in the PE header where the optional header starts
    const uint32_t OptionalHeaderPeOffset = 24;
    // The optional header starts with a magic WORD:
    const uint16_t MagicPe32 = 0x010B;
    const uint16_t MagicPe64 = 0x020B;

    // The offset in the PE header where the CLR header RVA is located,
    const uint32_t ClrHeaderPeOffset32 = OptionalHeaderPeOffset + 208;
    const uint32_t ClrHeaderPeOffset64 = OptionalHeaderPeOffset + 224;

    try
    {
        wil::unique_file file(_wfsopen(path, L"rb", _SH_DENYNO));
        if (!file)
            return Kind::Unknown;

        std::vector<uint8_t> buffer;
        buffer.resize(1024);

        size_t bytesRead = ::fread(&buffer[0], 1, buffer.size(), file.get());
        buffer.resize(bytesRead);

        uint32_t peStart = ReadDWord(buffer, PeHeaderFileOffset);

        uint16_t characteristic = ReadWord(buffer, peStart + CharacteristicsPeOffset);
        if ((characteristic & IMAGE_FILE_EXECUTABLE_IMAGE) == 0)
            return Kind::Unknown;

        Kind kind = (characteristic & IMAGE_FILE_DLL) != 0 ? Kind::Dll : Kind::Exe;

        uint16_t magic = ReadWord(buffer, peStart + OptionalHeaderPeOffset);
        if (magic != MagicPe32 && magic != MagicPe64)
            return Kind::Unknown;

        bool isPe32 = magic == MagicPe32;
        kind |= isPe32 ? Kind::Bitness32 : Kind::Bitness64;

        uint32_t clrHeaderRva = ReadDWord(buffer, peStart + (isPe32 ? ClrHeaderPeOffset32 : ClrHeaderPeOffset64));

        kind |= clrHeaderRva != 0 ? Kind::Managed : Kind::Native;

        return kind;
    }
    catch (...)
    {
        return Kind::Unknown;
    }
}
}
