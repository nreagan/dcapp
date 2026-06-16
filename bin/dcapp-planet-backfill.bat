@echo off
setlocal enabledelayedexpansion

set "DCAPP_HOME=%~dp0.."
pushd "%DCAPP_HOME%"
set "DCAPP_HOME=%CD%"
popd
set "RUN_DIR=%DCAPP_HOME%\pilotlight\out"

if "%~1"=="" (
    echo Usage: dcapp-planet-backfill.bat ^<planet_json^> [output_dcpm]
    exit /b 1
)

set "INPUT=%~f1"
shift

set "OUTPUT="
if not "%~1"=="" (
    set "OUTPUT=%~f1"
    shift
)

if not "%~1"=="" (
    echo Error: unexpected argument: %~1
    exit /b 1
)

cd /d "%RUN_DIR%"
if "%OUTPUT%"=="" (
    echo pilot_light.exe -a dcapp-planet-backfill "%INPUT%"
    pilot_light.exe -a dcapp-planet-backfill "%INPUT%"
) else (
    echo pilot_light.exe -a dcapp-planet-backfill "%INPUT%" "%OUTPUT%"
    pilot_light.exe -a dcapp-planet-backfill "%INPUT%" "%OUTPUT%"
)
