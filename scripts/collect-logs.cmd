@echo off
REM MediaAnalyse - one-click diagnostics collector.
REM Double-click this file (or run it from a console) to create a zip with
REM logs, recent minidumps and environment info next to this script.
setlocal
echo Collecting MediaAnalyse diagnostics...

REM Resolve PowerShell by absolute path: the launcher may inherit a trimmed
REM PATH (portable shells, scheduled tasks, some IDEs), in which case a bare
REM "powershell.exe" is not found and the collector silently does nothing.
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" set "PS=powershell.exe"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect_logs.ps1"
if errorlevel 1 echo WARNING: collector reported an error. See messages above.
echo.
pause
