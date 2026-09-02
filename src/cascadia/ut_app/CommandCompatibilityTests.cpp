// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "../TerminalApp/CommandCompatibility.h"
#include "../TerminalApp/QuickFixRequestTracker.h"

using namespace WEX::TestExecution;

namespace TerminalAppUnitTests
{
    class CommandCompatibilityTests
    {
        BEGIN_TEST_CLASS(CommandCompatibilityTests)
        END_TEST_CLASS()

        TEST_METHOD(CommonLinuxCommandsUseWsl);
        TEST_METHOD(CommonFileCommandsPreferWindowsBehavior);
        TEST_METHOD(RemoveUsesRecycleBin);
        TEST_METHOD(QuickFixRequestsAreTrackedPerControl);
        TEST_METHOD(ShellBuiltinsHaveNoBuiltInFix);
        TEST_METHOD(UnsafeOrUnknownCommandsHaveNoBuiltInFix);
    };

    void CommandCompatibilityTests::CommonLinuxCommandsUseWsl()
    {
        const auto commands = ::TerminalApp::CommandCompatibility::GetSupportedCommands();
        VERIFY_ARE_EQUAL(83u, commands.size());
        VERIFY_IS_TRUE(std::find(commands.begin(), commands.end(), std::wstring_view{ L"head" }) != commands.end());
        VERIFY_IS_TRUE(std::find(commands.begin(), commands.end(), std::wstring_view{ L"vim" }) != commands.end());

        for (size_t i = 0; i < commands.size(); ++i)
        {
            const auto command = commands[i];
            for (const auto ch : command)
            {
                VERIFY_IS_TRUE((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9'));
            }

            if (i > 0)
            {
                VERIFY_IS_TRUE(commands[i - 1] < command);
            }

            const auto suggestions = ::TerminalApp::CommandCompatibility::GetQuickFixes(command);
            std::wstring expected{ L"wsl.exe --exec " };
            expected.append(command);

            std::wstring uppercase{ command };
            for (auto& ch : uppercase)
            {
                if (ch >= L'a' && ch <= L'z')
                {
                    ch -= L'a' - L'A';
                }
            }

            const auto uppercaseSuggestions = ::TerminalApp::CommandCompatibility::GetQuickFixes(uppercase);
            VERIFY_ARE_EQUAL(suggestions.size(), uppercaseSuggestions.size());
            for (size_t suggestionIndex = 0; suggestionIndex < suggestions.size(); ++suggestionIndex)
            {
                VERIFY_ARE_EQUAL(suggestions[suggestionIndex].c_str(), uppercaseSuggestions[suggestionIndex].c_str());
            }

            for (const auto& suggestion : suggestions)
            {
                VERIFY_IS_TRUE(suggestion.find_first_of(L"\r\n") == std::wstring::npos);
            }

            if (command == L"rm")
            {
                VERIFY_ARE_EQUAL(1u, suggestions.size());
                VERIFY_IS_TRUE(suggestions.at(0).find(L"SendToRecycleBin") != std::wstring::npos);
            }
            else
            {
                const auto wslSuggestion = std::find(suggestions.begin(), suggestions.end(), expected);
                VERIFY_IS_TRUE(wslSuggestion != suggestions.end());
                VERIFY_ARE_EQUAL(L"wsl.exe --install", suggestions.back().c_str());
            }
        }
    }

    void CommandCompatibilityTests::CommonFileCommandsPreferWindowsBehavior()
    {
        constexpr std::pair<std::wstring_view, std::wstring_view> commands[]{
            { L"cp", L"Copy-Item" },
            { L"grep", L"Select-String" },
            { L"mv", L"Move-Item" },
        };

        for (const auto& [command, expectedSuggestion] : commands)
        {
            const auto suggestions = ::TerminalApp::CommandCompatibility::GetQuickFixes(command);
            VERIFY_ARE_EQUAL(3u, suggestions.size());
            VERIFY_ARE_EQUAL(expectedSuggestion.data(), suggestions.front().c_str());
        }
    }

    void CommandCompatibilityTests::RemoveUsesRecycleBin()
    {
        const auto suggestions = ::TerminalApp::CommandCompatibility::GetQuickFixes(L"rm");
        VERIFY_ARE_EQUAL(1u, suggestions.size());

        const auto& command = suggestions.front();
        VERIFY_IS_TRUE(command.starts_with(L"powershell.exe -NoProfile -Command"));
        VERIFY_IS_TRUE(command.find(L"ValueFromRemainingArguments=$true") != std::wstring::npos);
        VERIFY_IS_TRUE(command.find(L"DeleteFile") != std::wstring::npos);
        VERIFY_IS_TRUE(command.find(L"DeleteDirectory") != std::wstring::npos);
        VERIFY_IS_TRUE(command.find(L"SendToRecycleBin") != std::wstring::npos);
        VERIFY_IS_TRUE(command.find(L"Remove-Item") == std::wstring::npos);
        VERIFY_IS_TRUE(command.find(L"wsl") == std::wstring::npos);
    }

    void CommandCompatibilityTests::QuickFixRequestsAreTrackedPerControl()
    {
        ::TerminalApp::QuickFixRequestTracker firstControl;
        ::TerminalApp::QuickFixRequestTracker secondControl;

        const auto firstControlRequest = firstControl.Start();
        const auto secondControlRequest = secondControl.Start();
        VERIFY_IS_TRUE(firstControl.IsCurrent(firstControlRequest));
        VERIFY_IS_TRUE(secondControl.IsCurrent(secondControlRequest));

        const auto newerFirstControlRequest = firstControl.Start();
        VERIFY_IS_FALSE(firstControl.IsCurrent(firstControlRequest));
        VERIFY_IS_TRUE(firstControl.IsCurrent(newerFirstControlRequest));
        VERIFY_IS_TRUE(secondControl.IsCurrent(secondControlRequest));

        firstControl.Invalidate();
        VERIFY_IS_FALSE(firstControl.IsCurrent(newerFirstControlRequest));
        VERIFY_IS_TRUE(secondControl.IsCurrent(secondControlRequest));

        bool published = false;
        VERIFY_IS_FALSE(firstControl.RunIfCurrent(newerFirstControlRequest, [&]() {
            published = true;
        }));
        VERIFY_IS_FALSE(published);

        const auto currentRequest = firstControl.Start();
        VERIFY_IS_TRUE(firstControl.RunIfCurrent(currentRequest, [&]() {
            published = true;
        }));
        VERIFY_IS_TRUE(published);

        VERIFY_ARE_EQUAL(currentRequest, firstControl.Current());
        VERIFY_IS_TRUE(firstControl.InvalidateIfCurrent(currentRequest));
        VERIFY_IS_FALSE(firstControl.InvalidateIfCurrent(currentRequest));

        bool requestStarted = false;
        const auto resetRequest = firstControl.Start([&]() {
            requestStarted = true;
        });
        VERIFY_IS_TRUE(requestStarted);
        VERIFY_IS_TRUE(firstControl.IsCurrent(resetRequest));

        bool invalidated = false;
        VERIFY_IS_TRUE(firstControl.InvalidateIfCurrent(resetRequest, [&]() {
            invalidated = true;
        }));
        VERIFY_IS_TRUE(invalidated);
    }

    void CommandCompatibilityTests::ShellBuiltinsHaveNoBuiltInFix()
    {
        constexpr std::wstring_view commands[]{
            L"alias",
            L"bg",
            L"cd",
            L"eval",
            L"exec",
            L"export",
            L"fg",
            L"history",
            L"jobs",
            L"popd",
            L"pushd",
            L"read",
            L"set",
            L"source",
            L"trap",
            L"umask",
            L"unset",
        };

        for (const auto command : commands)
        {
            VERIFY_IS_TRUE(::TerminalApp::CommandCompatibility::GetQuickFixes(command).empty());
        }
    }

    void CommandCompatibilityTests::UnsafeOrUnknownCommandsHaveNoBuiltInFix()
    {
        constexpr std::wstring_view commands[]{
            L"",
            L"definitely-not-a-command",
            L" grep",
            L"grep ",
            L"grep.exe",
            L"grep\t",
            L"grep\r\nrm",
            L"gr\u00e9p",
            L"head -n 10",
            L"rm -rf",
            L"sudo;rm",
            L"vim file.txt",
        };

        for (const auto command : commands)
        {
            VERIFY_IS_TRUE(::TerminalApp::CommandCompatibility::GetQuickFixes(command).empty());
        }
    }
}
