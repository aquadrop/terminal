# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

Import-Module PSReadLine

$commandRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\LinuxCommands'))
foreach ($command in 'cat', 'cp', 'du', 'grep', 'head', 'mv', 'rm', 'tail', 'touch', 'wc') {
    $commandPath = Join-Path $commandRoot "$command.exe"
    if (-not (Test-Path -LiteralPath $commandPath -PathType Leaf)) {
        throw "Linux compatibility command is missing: $commandPath"
    }
    Set-Alias -Name $command -Value $commandPath -Scope Global -Option AllScope -Force -ErrorAction Stop
}

function Resolve-TerminalHistoryEvent {
    param(
        [Parameter(Mandatory)]
        [string] $Line,

        [Parameter(Mandatory)]
        [object[]] $History
    )

    $match = [regex]::Match($Line, '^(?<indent>\s*)!(?<event>!|\d+)(?<suffix>(?:\s+.*)?)$')
    if (-not $match.Success) {
        return [pscustomobject]@{
            IsHistoryEvent = $false
            CommandLine = $null
        }
    }

    $event = $match.Groups['event'].Value
    $entry = if ($event -eq '!') {
        $History | Select-Object -Last 1
    } else {
        $History | Where-Object Id -EQ ([long] $event) | Select-Object -First 1
    }

    return [pscustomobject]@{
        IsHistoryEvent = $true
        CommandLine = if ($null -eq $entry) {
            $null
        } else {
            $match.Groups['indent'].Value +
                $entry.CommandLine +
                $match.Groups['suffix'].Value
        }
    }
}

$enterHandler = Get-PSReadLineKeyHandler -Chord Enter
if ($enterHandler.Function -eq 'AcceptLine') {
    Set-PSReadLineKeyHandler -Chord Enter -BriefDescription 'AcceptLineWithBangHistory' -ScriptBlock {
        param($key, $arg)

        $line = $null
        $cursor = 0
        [Microsoft.PowerShell.PSConsoleReadLine]::GetBufferState([ref] $line, [ref] $cursor)

        $expansion = Resolve-TerminalHistoryEvent -Line $line -History @(Get-History)
        if ($expansion.IsHistoryEvent) {
            if ($null -eq $expansion.CommandLine) {
                [Microsoft.PowerShell.PSConsoleReadLine]::Ding()
                return
            }

            [Microsoft.PowerShell.PSConsoleReadLine]::Replace(
                0,
                $line.Length,
                $expansion.CommandLine,
                $null,
                $null)
        }

        [Microsoft.PowerShell.PSConsoleReadLine]::AcceptLine($key, $arg)
    }
}
