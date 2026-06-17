@echo off
setlocal enabledelayedexpansion

set "DCAPP_HOME=%~dp0..\.."
pushd "%DCAPP_HOME%"
set "DCAPP_HOME=%CD%"
popd
set "RUN_DIR=%DCAPP_HOME%\pilotlight\out"

if "%~1"=="" (
    echo Usage: dcapp-planet-chunkgen.bat ^<input_dem^> ^<output_dir^> --radius ^<N^> [options]
    exit /b 1
)
if "%~1"=="-h" goto help
if "%~1"=="--help" goto help

goto args

:help
cd /d "%RUN_DIR%"
pilot_light.exe -a dcapp-planet-legacy-chunkgen --planet-legacy-help
exit /b %ERRORLEVEL%

:args

set "INPUT=%~f1"
set "OUTPUT=%~f2"
shift
shift

set "ARGS="
:argloop
if "%~1"=="" goto endargs
set "ARGS=!ARGS! %~1"
shift
goto argloop
:endargs

cd /d "%RUN_DIR%"
echo pilot_light.exe -a dcapp-planet-legacy-chunkgen "%INPUT%" "%OUTPUT%"%ARGS%
pilot_light.exe -a dcapp-planet-legacy-chunkgen "%INPUT%" "%OUTPUT%"%ARGS%
