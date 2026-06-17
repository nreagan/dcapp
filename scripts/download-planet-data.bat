@echo off
setlocal enabledelayedexpansion

pushd "%~dp0\.."
set "DCAPP_HOME=%CD%"
popd

if "%DCAPP_PLANET_DATA_DIR%"=="" (
    set "DATA_DIR=%DCAPP_HOME%\data"
) else (
    set "DATA_DIR=%DCAPP_PLANET_DATA_DIR%"
)

set "SOURCE_DIR=%DATA_DIR%"
set "CHUNK_DIR=%DATA_DIR%"
set "IMG_URL=https://imbrium.mit.edu/DATA/LOLA_GDR/POLAR/IMG/LDEM_45S_400M.IMG"
set "LBL_URL=https://imbrium.mit.edu/DATA/LOLA_GDR/POLAR/IMG/LDEM_45S_400M.LBL"
set "IMG_FILE=%SOURCE_DIR%\LDEM_45S_400M.IMG"
set "LBL_FILE=%SOURCE_DIR%\LDEM_45S_400M.LBL"
set "PLANET_JSON=%CHUNK_DIR%\LDEM_45S_400M.planet.json"
if "%DCAPP_PLANET_TILE_SIZE%"=="" (
    set "PLANET_TILE_SIZE=257"
) else (
    set "PLANET_TILE_SIZE=%DCAPP_PLANET_TILE_SIZE%"
)
if "%DCAPP_PLANET_MAX_LOD%"=="" (
    set "PLANET_MAX_LOD=5"
) else (
    set "PLANET_MAX_LOD=%DCAPP_PLANET_MAX_LOD%"
)
set "FORCE=0"
set "EXTRA_ARGS="

:parse
if "%~1"=="" goto parsed
if "%~1"=="-h" goto help
if "%~1"=="--help" goto help
if "%~1"=="--force" (
    set "FORCE=1"
) else (
    set "EXTRA_ARGS=!EXTRA_ARGS! %~1"
)
shift
goto parse

:help
echo Usage: scripts\download-planet-data.bat [--force] [chunkgen options]
echo.
echo Downloads the LOLA LDEM_45S_400M lunar DEM and generates planet chunks.
echo.
echo Environment:
echo   DCAPP_PLANET_DATA_DIR  Override output directory
echo                          default: data
echo   DCAPP_PLANET_TILE_SIZE Override generated chunk tile size
echo                          default: 257
echo   DCAPP_PLANET_MAX_LOD   Override generated max LOD
echo                          default: 5
echo.
echo Options:
echo   --force                Regenerate chunks even if the .planet.json exists
echo   -h, --help             Show this help
echo.
echo Any other options are passed through to dcapp-planet-chunkgen.
exit /b 0

:parsed
echo ========================================
echo Planet Data Download
echo ========================================
echo Data directory: %DATA_DIR%

if not exist "%DATA_DIR%" mkdir "%DATA_DIR%"

if not exist "%IMG_FILE%" (
    echo Downloading LDEM_45S_400M.IMG...
    curl -L -o "%IMG_FILE%" "%IMG_URL%"
    if errorlevel 1 ( echo ERROR: Failed to download IMG & exit /b 1 )
) else (
    echo LDEM_45S_400M.IMG already downloaded, skipping.
)

if not exist "%LBL_FILE%" (
    echo Downloading LDEM_45S_400M.LBL...
    curl -L -o "%LBL_FILE%" "%LBL_URL%"
    if errorlevel 1 ( echo ERROR: Failed to download LBL & exit /b 1 )
) else (
    echo LDEM_45S_400M.LBL already downloaded, skipping.
)

if "%FORCE%"=="1" goto run_chunkgen
if not exist "%PLANET_JSON%" goto run_chunkgen
echo LDEM_45S_400M.planet.json already exists, skipping chunkgen. Use --force to regenerate.
goto done

:run_chunkgen
echo.
echo Running chunkgen...
if "%FORCE%"=="1" del /q "%CHUNK_DIR%\LDEM_45S_400M_f*_l*.p2c" 2>nul
call "%DCAPP_HOME%\bin\dcapp-planet-chunkgen.bat" "%CHUNK_DIR%" "%LBL_FILE%" --radius 1737400 --prefix LDEM_45S_400M --tile-size %PLANET_TILE_SIZE% --max-lod %PLANET_MAX_LOD%%EXTRA_ARGS%
if errorlevel 1 exit /b 1

:done
echo.
echo Planet data ready:
echo   %PLANET_JSON%
