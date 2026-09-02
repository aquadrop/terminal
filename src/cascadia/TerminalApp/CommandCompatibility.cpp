// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CommandCompatibility.h"

namespace
{
    constexpr std::wstring_view RecycleCommand{
        LR"(powershell.exe -NoProfile -Command '& { param([Parameter(Mandatory=$true, ValueFromRemainingArguments=$true)][string[]]$Path) Add-Type -AssemblyName Microsoft.VisualBasic; foreach ($item in $Path) { $resolved = (Resolve-Path -LiteralPath $item -ErrorAction Stop).Path; if ([System.IO.Directory]::Exists($resolved)) { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($resolved, [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs, [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin) } else { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($resolved, [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs, [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin) } } }')"
    };

    // Shell built-ins are intentionally excluded because wsl.exe --exec cannot
    // preserve changes such as the working directory or environment.
    constexpr std::wstring_view LinuxCommands[]{
        L"awk",
        L"basename",
        L"bash",
        L"cat",
        L"chgrp",
        L"chmod",
        L"chown",
        L"clear",
        L"comm",
        L"cp",
        L"curl",
        L"cut",
        L"date",
        L"df",
        L"diff",
        L"dirname",
        L"du",
        L"env",
        L"file",
        L"find",
        L"grep",
        L"groups",
        L"gunzip",
        L"gzip",
        L"head",
        L"hostname",
        L"id",
        L"ip",
        L"join",
        L"kill",
        L"less",
        L"ln",
        L"ls",
        L"mkdir",
        L"mktemp",
        L"more",
        L"mv",
        L"nano",
        L"nl",
        L"nohup",
        L"paste",
        L"patch",
        L"pgrep",
        L"ping",
        L"pkill",
        L"printenv",
        L"ps",
        L"pwd",
        L"readlink",
        L"realpath",
        L"rm",
        L"rmdir",
        L"scp",
        L"sed",
        L"sftp",
        L"sh",
        L"sha256sum",
        L"sort",
        L"ss",
        L"ssh",
        L"stat",
        L"strings",
        L"sudo",
        L"tail",
        L"tar",
        L"tee",
        L"time",
        L"top",
        L"touch",
        L"tr",
        L"uname",
        L"uniq",
        L"uptime",
        L"vi",
        L"vim",
        L"watch",
        L"wc",
        L"wget",
        L"whereis",
        L"which",
        L"whoami",
        L"xargs",
        L"xz",
    };
}

std::span<const std::wstring_view> TerminalApp::CommandCompatibility::GetSupportedCommands() noexcept
{
    return LinuxCommands;
}

std::vector<std::wstring> TerminalApp::CommandCompatibility::GetQuickFixes(const std::wstring_view missingCommand)
{
    for (const auto command : GetSupportedCommands())
    {
        if (til::equals_insensitive_ascii(missingCommand, command))
        {
            // Prefer Windows-native behavior for common Unix command names.
            // rm is deliberately never delegated to WSL, where deletion would
            // bypass the Windows Recycle Bin.
            if (command == L"rm")
            {
                return { std::wstring{ RecycleCommand } };
            }

            std::wstring wslCommand{ L"wsl.exe --exec " };
            wslCommand.append(command);

            if (command == L"cp")
            {
                return { L"Copy-Item", std::move(wslCommand), L"wsl.exe --install" };
            }
            if (command == L"grep")
            {
                return { L"Select-String", std::move(wslCommand), L"wsl.exe --install" };
            }
            if (command == L"mv")
            {
                return { L"Move-Item", std::move(wslCommand), L"wsl.exe --install" };
            }

            return { std::move(wslCommand), L"wsl.exe --install" };
        }
    }

    return {};
}
