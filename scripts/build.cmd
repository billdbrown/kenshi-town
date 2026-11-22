@echo off
REM Build TownLedger.dll with the VC++ 2010 (v100) x64 compiler.
REM
REM Needs: Windows SDK 7.1 compilers + KB2519277 (no full VS2010), any VS 2019+
REM for MSBuild, and third_party\KenshiLib_deps with boost.zip extracted.
REM We pass INCLUDE/LIB/PATH ourselves and UseEnv=true so MSBuild does not need
REM VS2010 registry detection. Approach borrowed from KenshiCoop's build script.
REM
REM   scripts\build.cmd            build
REM   scripts\build.cmd install    build, then copy the mod into Kenshi\mods
setlocal

set "REPO=%~dp0.."
pushd "%REPO%" >nul
set "REPO=%CD%"
popd >nul

set "VS10=C:\Program Files (x86)\Microsoft Visual Studio 10.0"
set "VC=%VS10%\VC"
set "SDK=C:\Program Files\Microsoft SDKs\Windows\v7.1"
set "KL=%REPO%\third_party\KenshiLib_deps"
if not defined KENSHI_DIR set "KENSHI_DIR=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"

if not exist "%VC%\bin\amd64\cl.exe" (
    echo Missing v100 x64 compiler at "%VC%\bin\amd64\cl.exe". Install Windows SDK 7.1 compilers + KB2519277.
    exit /b 1
)

set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe"`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo Could not find MSBuild.exe via vswhere.
    exit /b 1
)

set "PATH=%VC%\bin\amd64;%VC%\bin;%VS10%\Common7\IDE;%SDK%\Bin\x64;%SDK%\Bin;%PATH%"
set "INCLUDE=%VC%\include;%SDK%\Include;%REPO%\third_party\vc10_compat;%KL%\KenshiLib\Include;%KL%\KenshiLib\Include\ogre;%KL%\boost_1_60_0"
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64;%KL%\KenshiLib\Libraries;%KL%\boost_1_60_0\stage\lib"

echo === Building TownLedger.dll (Release^|x64, v100) ===
"%MSBUILD%" "%REPO%\src\TownLedger.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:UseEnv=true /p:TrackFileAccess=false /nologo /v:minimal
if errorlevel 1 exit /b 1

if /i "%~1"=="install" (
    if not exist "%KENSHI_DIR%\mods" (
        echo Kenshi mods folder not found at "%KENSHI_DIR%\mods"
        exit /b 1
    )
    xcopy /y /i "%REPO%\mod\TownLedger" "%KENSHI_DIR%\mods\TownLedger" >nul
    copy /y "%REPO%\build\TownLedger.dll" "%KENSHI_DIR%\mods\TownLedger\TownLedger.dll" >nul
    echo Installed to "%KENSHI_DIR%\mods\TownLedger"
    REM The data mods, TownLedger.mod among them, are generated from the
    REM vanilla data rather than kept in the repo.
    python -I "%REPO%\scripts\build_mods.py" install
    if errorlevel 1 exit /b 1
)

endlocal
