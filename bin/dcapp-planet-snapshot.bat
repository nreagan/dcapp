@echo off
setlocal enabledelayedexpansion

set "DCAPP_HOME=%~dp0.."
pushd "%DCAPP_HOME%\pilotlight\out" >nul

if "%~1"=="" (
    pilot_light.exe -a dcapp-planet-snapshot --planet-snapshot-help
    popd >nul
    exit /b %ERRORLEVEL%
)

set "ARGS="
:loop
if "%~1"=="" goto run
if "%~1"=="-h" (
    set "ARGS=!ARGS! --planet-snapshot-help"
) else if "%~1"=="--help" (
    set "ARGS=!ARGS! --planet-snapshot-help"
) else (
    set "ARGS=!ARGS! %~1"
)
shift
goto loop

:run
echo pilot_light.exe -a dcapp-planet-snapshot%ARGS%
pilot_light.exe -a dcapp-planet-snapshot%ARGS%
set "RESULT=%ERRORLEVEL%"
popd >nul
exit /b %RESULT%
