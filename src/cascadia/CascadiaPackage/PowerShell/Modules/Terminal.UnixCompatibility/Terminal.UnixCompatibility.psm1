# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

Import-Module PSReadLine

$enterHandler = Get-PSReadLineKeyHandler -Chord Enter
if ($enterHandler.Function -eq 'AcceptLine') {
    Set-PSReadLineKeyHandler -Chord Enter -BriefDescription 'AcceptLineWithBangHistory' -ScriptBlock {
        param($key, $arg)

        $line = $null
        $cursor = 0
        [Microsoft.PowerShell.PSConsoleReadLine]::GetBufferState([ref] $line, [ref] $cursor)

        $match = [regex]::Match($line, '^(?<indent>\s*)!!(?<suffix>(?:\s+.*)?)$')
        if ($match.Success) {
            $history = Get-History -Count 1
            if ($null -eq $history) {
                [Microsoft.PowerShell.PSConsoleReadLine]::Ding()
                return
            }

            $replacement = $match.Groups['indent'].Value +
                $history.CommandLine +
                $match.Groups['suffix'].Value
            [Microsoft.PowerShell.PSConsoleReadLine]::Replace(
                0,
                $line.Length,
                $replacement,
                $null,
                $null)
        }

        [Microsoft.PowerShell.PSConsoleReadLine]::AcceptLine($key, $arg)
    }
}
