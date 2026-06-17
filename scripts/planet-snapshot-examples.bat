@echo off
setlocal

set "DCAPP_HOME=%~dp0.."
set "SNAPSHOT=%DCAPP_HOME%\bin\dcapp-planet-snapshot.bat"
set "PLANET_DATA=%DCAPP_HOME%\data\LDEM_45S_400M.planet.json"
set "OUT_DIR=%DCAPP_HOME%\data"

if "%~1"=="1" set "EXAMPLE=1"
if "%~1"=="2" set "EXAMPLE=2"
if "%~1"=="3" set "EXAMPLE=3"
if defined EXAMPLE goto checkdata

echo Usage: %~nx0 1^|2^|3
echo   1  default manifest view
echo   2  tile debug view
echo   3  oblique Cartesian view
exit /b 1

:checkdata
if not exist "%PLANET_DATA%" (
    echo Missing planet data: "%PLANET_DATA%"
    echo Run scripts\download-planet-data.bat first, then rebuild if needed.
    exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

if "%EXAMPLE%"=="1" goto example1
if "%EXAMPLE%"=="2" goto example2
if "%EXAMPLE%"=="3" goto example3

:example1
call "%SNAPSHOT%" ^
    "%PLANET_DATA%" ^
    --width 1280 --height 720 ^
    --fov 60 ^
    --output "%OUT_DIR%\planet-default.png"
exit /b %errorlevel%

:example2
call "%SNAPSHOT%" ^
    "%PLANET_DATA%" ^
    --width 1024 --height 1024 ^
    --fov 55 ^
    --show-tiles ^
    --output "%OUT_DIR%\planet-tiles.png"
exit /b %errorlevel%

:example3
call "%SNAPSHOT%" ^
    "%PLANET_DATA%" ^
    --eye -494826 -3190740 1882148 ^
    --target 0 0 0 ^
    --width 1280 --height 720 ^
    --fov 60 ^
    --output "%OUT_DIR%\planet-oblique.png"
exit /b %errorlevel%
