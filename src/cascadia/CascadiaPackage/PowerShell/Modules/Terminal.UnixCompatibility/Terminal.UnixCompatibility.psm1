# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

Import-Module PSReadLine

function global:ConvertFrom-TerminalLinuxPath {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyString()]
        [string] $Path
    )

    $match = [regex]::Match(
        $Path,
        '\A/mnt/(?<drive>[A-Za-z])(?:/(?<remainder>.*))?\z',
        [System.Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if (-not $match.Success) {
        return $Path
    }

    $drive = $match.Groups['drive'].Value.ToUpperInvariant()
    $remainder = $match.Groups['remainder'].Value.Replace([char] '/', [char] '\')
    return '{0}:\{1}' -f $drive, $remainder
}

function global:Set-TerminalLocation {
    [CmdletBinding(DefaultParameterSetName = 'Path')]
    param(
        [Parameter(
            ParameterSetName = 'Path',
            Position = 0,
            ValueFromPipeline,
            ValueFromPipelineByPropertyName)]
        [AllowEmptyString()]
        [string] $Path,

        [Parameter(
            Mandatory,
            ParameterSetName = 'LiteralPath',
            ValueFromPipelineByPropertyName)]
        [Alias('PSPath', 'LP')]
        [AllowEmptyString()]
        [string] $LiteralPath,

        [Parameter(ParameterSetName = 'StackName')]
        [string] $StackName,

        [switch] $PassThru
    )

    process {
        $parameters = @{}
        foreach ($entry in $PSBoundParameters.GetEnumerator()) {
            $parameters[$entry.Key] = $entry.Value
        }
        if ($parameters.ContainsKey('Path')) {
            $parameters['Path'] = ConvertFrom-TerminalLinuxPath $parameters['Path']
        }
        if ($parameters.ContainsKey('LiteralPath')) {
            $parameters['LiteralPath'] = ConvertFrom-TerminalLinuxPath $parameters['LiteralPath']
        }
        Microsoft.PowerShell.Management\Set-Location @parameters
    }
}

function global:Push-TerminalLocation {
    [CmdletBinding(DefaultParameterSetName = 'Path')]
    param(
        [Parameter(
            ParameterSetName = 'Path',
            Position = 0,
            ValueFromPipeline,
            ValueFromPipelineByPropertyName)]
        [AllowEmptyString()]
        [string] $Path,

        [Parameter(
            Mandatory,
            ParameterSetName = 'LiteralPath',
            ValueFromPipelineByPropertyName)]
        [Alias('PSPath', 'LP')]
        [AllowEmptyString()]
        [string] $LiteralPath,

        [string] $StackName,
        [switch] $PassThru
    )

    process {
        $parameters = @{}
        foreach ($entry in $PSBoundParameters.GetEnumerator()) {
            $parameters[$entry.Key] = $entry.Value
        }
        if ($parameters.ContainsKey('Path')) {
            $parameters['Path'] = ConvertFrom-TerminalLinuxPath $parameters['Path']
        }
        if ($parameters.ContainsKey('LiteralPath')) {
            $parameters['LiteralPath'] = ConvertFrom-TerminalLinuxPath $parameters['LiteralPath']
        }
        Microsoft.PowerShell.Management\Push-Location @parameters
    }
}

function global:Pop-TerminalLocation {
    [CmdletBinding()]
    param(
        [string] $StackName,
        [switch] $PassThru
    )

    $parameters = @{}
    foreach ($entry in $PSBoundParameters.GetEnumerator()) {
        $parameters[$entry.Key] = $entry.Value
    }
    Microsoft.PowerShell.Management\Pop-Location @parameters
}

$commandRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\LinuxCommands'))
foreach ($command in 'cat', 'cp', 'du', 'grep', 'head', 'mv', 'rm', 'tail', 'touch', 'wc') {
    $commandPath = Join-Path $commandRoot "$command.exe"
    if (-not (Test-Path -LiteralPath $commandPath -PathType Leaf)) {
        throw "Linux compatibility command is missing: $commandPath"
    }
    Set-Alias -Name $command -Value $commandPath -Scope Global -Option AllScope -Force -ErrorAction Stop
}
Set-Alias -Name cd -Value Set-TerminalLocation -Scope Global -Option AllScope -Force -ErrorAction Stop
Set-Alias -Name pushd -Value Push-TerminalLocation -Scope Global -Option AllScope -Force -ErrorAction Stop
Set-Alias -Name popd -Value Pop-TerminalLocation -Scope Global -Option AllScope -Force -ErrorAction Stop

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
