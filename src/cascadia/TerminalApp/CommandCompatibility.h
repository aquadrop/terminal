// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace TerminalApp::CommandCompatibility
{
    std::span<const std::wstring_view> GetSupportedCommands() noexcept;
    std::vector<std::wstring> GetQuickFixes(std::wstring_view missingCommand);
}
