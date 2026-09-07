// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Terminal::UnixCommandShim
{
    [[nodiscard]] inline std::filesystem::path ConvertLinuxPath(const std::wstring_view path)
    {
        constexpr std::wstring_view mountPrefix{ L"/mnt/" };
        if (path.size() < mountPrefix.size() + 1 ||
            !path.starts_with(mountPrefix))
        {
            return std::filesystem::path{ path };
        }

        const auto drive = path[mountPrefix.size()];
        const auto isLowercaseDrive = drive >= L'a' && drive <= L'z';
        const auto isUppercaseDrive = drive >= L'A' && drive <= L'Z';
        const auto separator = mountPrefix.size() + 1;
        if ((!isLowercaseDrive && !isUppercaseDrive) ||
            (path.size() > separator && path[separator] != L'/'))
        {
            return std::filesystem::path{ path };
        }

        std::wstring converted;
        converted.reserve(path.size());
        converted.push_back(isLowercaseDrive ? drive - (L'a' - L'A') : drive);
        converted.append(L":\\");
        for (auto index = separator + 1; index < path.size(); ++index)
        {
            converted.push_back(path[index] == L'/' ? L'\\' : path[index]);
        }
        return std::filesystem::path{ converted };
    }
}
