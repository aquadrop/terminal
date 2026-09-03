// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <span>
#include <string_view>

namespace Terminal::UnixCommandShim
{
    int Cat(std::span<const std::wstring_view> arguments);
    int Du(std::span<const std::wstring_view> arguments);
    int Head(std::span<const std::wstring_view> arguments);
    int Tail(std::span<const std::wstring_view> arguments);
    int Touch(std::span<const std::wstring_view> arguments);
    int Wc(std::span<const std::wstring_view> arguments);
}
