@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Package-Windows.ps1" %*
set "package_exit=%ERRORLEVEL%"
if not "%package_exit%"=="0" echo Packaging failed with exit code %package_exit%.
if "%~1"=="" pause
exit /b %package_exit%
