@echo off
setlocal enabledelayedexpansion

set "DCAPP_HOME=%~dp0.."
pushd "%DCAPP_HOME%"
set "DCAPP_HOME=%CD%"
popd
set "RUN_DIR=%DCAPP_HOME%\pilotlight\out"

if "%~1"=="" (
    cd /d "%RUN_DIR%"
    pilot_light.exe -a dcapp-planet-validate --planet-validate-help
    exit /b %ERRORLEVEL%
)

set "ARGS="
:argloop
if "%~1"=="" goto endargs
set "ARG=%~1"
if exist "%~1" (
    set "ARG=%~f1"
)
set "ARGS=!ARGS! "!ARG!""
shift
goto argloop
:endargs

cd /d "%RUN_DIR%"
echo pilot_light.exe -a dcapp-planet-validate%ARGS%
pilot_light.exe -a dcapp-planet-validate%ARGS%
