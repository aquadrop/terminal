# Current working directories in PowerShell

Duplicating a tab or pane reuses the current working directory reported by its
shell. Without directory reporting, Terminal only knows the session's starting
directory.

For PowerShell 7 sessions launched by Terminal, directory reporting is enabled
automatically when the command line is `pwsh` or `pwsh.exe` (including a quoted
executable path), optionally followed by `-NoLogo`, `-NoProfile`, or `-NoExit`.
The default PowerShell prompt reports its current filesystem directory using an
OSC 9;9 native Windows path. Spaces, Unicode, and path punctuation are preserved.

This does not edit `$PROFILE` or change the visible default prompt. Custom
prompts are left untouched. Non-filesystem locations, such as `HKCU:\`, leave
the last reported filesystem directory available for duplication.

The initialization hook uses PowerShell's `-NoExit -Command` startup mode,
which suppresses its startup banner. Profile loading is otherwise unchanged.

Explicit commands, scripts, other launch arguments, and sessions started outside
Terminal are not modified. Prompts in constrained-language sessions are also
left untouched. These sessions and custom prompts still need to provide their
own directory reporting using OSC 7 or OSC 9;9. Reporting begins in newly
launched sessions; it cannot be added retroactively to an already running shell.
