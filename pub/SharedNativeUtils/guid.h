#pragma once

#include <Windows.h>
#include <string>
#include <string_view>
#include <guiddef.h>
#include <objbase.h>
#include <wil/resource.h>
#include <absl/hash/hash.h>
#include "string_extensions.h"

struct Guid final : GUID
{
    static Guid CreateNew()
    {
        Guid    newGuid;
        HRESULT _ = ::CoCreateGuid(&newGuid);
        return newGuid;
    }

    Guid(bool createNew = false)
    {
        if (createNew)
            (void)::CoCreateGuid(this);
        else
            *(GUID*)this = GUID_NULL;
    }

    Guid(const GUID& guid)
    {
        *(GUID*)this = guid;
    }

    Guid(std::wstring_view guid)
    {
        FAIL_FAST_IF_FAILED_MSG(Parse(guid), "Failed to parse guid '%ls'", guid.data());
    }

    Guid(std::string_view guid)
    {
        FAIL_FAST_IF_FAILED_MSG(Parse(guid), "Failed to parse guid '%hs'", guid.data());
    }

    Guid(PCWSTR guid)
    {
        FAIL_FAST_IF_FAILED_MSG(Parse(guid), "Failed to parse guid '%ls'", guid);
    }

    Guid(PCSTR guid)
    {
        FAIL_FAST_IF_FAILED_MSG(Parse(guid), "Failed to parse guid '%hs'", guid);
    }

    // formatted as "{831532DC-7EFB-4A8C-841B-7BBE21558F8F}"
    std::wstring ToUtf16() const
    {
        std::wstring str(38, L'\0');
        THROW_HR_IF(E_FAIL, 0 == ::StringFromGUID2(*this, str.data(), (int)str.size() + 1));
        return str;
    }
    std::string ToUtf8() const
    {
        return Strings::ToUtf8(ToUtf16());
    }

    HRESULT Parse(const std::wstring_view guid)
    {
        CLSID clsid;
        if (guid.size() == 38)
        {
            RETURN_IF_FAILED(::CLSIDFromString(guid.data(), &clsid));
        }
        else if (guid.size() == 36)
        {
            std::wstring g(L"{");
            g += guid;
            g += L"}";

            RETURN_IF_FAILED(::CLSIDFromString(g.data(), &clsid));
        }
        *this = *(Guid*)&clsid;
        return S_OK;
    }
    HRESULT Parse(const std::string_view guid)
    {
        return Parse(Strings::ToUtf16(guid));
    }

    bool Equals(const Guid& rhs) const
    {
        return !!::IsEqualGUID(*this, rhs);
    }
    bool operator==(const Guid& rhs) const
    {
        return Equals(rhs);
    }

    // To e.g. make this work: std::unordered_set<Guid, absl::Hash<Guid>> services;
    template <typename H>
    friend H AbslHashValue(H h, const Guid& guid)
    {
        return H::combine(std::move(h), guid.Data1, guid.Data2, guid.Data3, *(uint64_t*)&guid.Data4[0]);
    }
};
