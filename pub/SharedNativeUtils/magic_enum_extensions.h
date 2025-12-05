#pragma once

#include <magic_enum/magic_enum.hpp>

namespace magic_enum::bitwise_operators
{
template <typename T>
using IsEnum = std::enable_if_t<std::is_enum_v<T>>;

template <typename E, typename = IsEnum<E>>
constexpr bool AnyBitSet(E value, E desiredBits) noexcept
{
    return static_cast<underlying_type_t<E>>(0) !=
           (static_cast<underlying_type_t<E>>(value) & static_cast<underlying_type_t<E>>(desiredBits));
}

template <typename E, typename = IsEnum<E>>
constexpr bool AllBitsSet(E value, E requiredBits) noexcept
{
    return static_cast<underlying_type_t<E>>(requiredBits) ==
           (static_cast<underlying_type_t<E>>(value) & static_cast<underlying_type_t<E>>(requiredBits));
}
}
using namespace magic_enum::bitwise_operators;
