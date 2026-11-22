@echo off
rem Pipeline test: the ankh from Documents\Blender\ankh\ankh_v2.blend into a
rem Kenshi building. Output in build\assets\ankh; build_mods.py installs it.
setlocal
set BLENDER="C:\Program Files\Blender Foundation\Blender 5.3\blender.exe"
set ASSET_OUT=%~dp0..\..\build\assets\ankh
if not exist "%ASSET_OUT%" mkdir "%ASSET_OUT%"
%BLENDER% -b "%USERPROFILE%\Documents\Blender\ankh\ankh_v2.blend" --python "%~dp0bake.py" || exit /b 1
%BLENDER% -b "%ASSET_OUT%\ankh_low.blend" --python "%~dp0export_mesh.py" || exit /b 1
copy /y "%ASSET_OUT%\ankh_diffuse.png" "%ASSET_OUT%\ankh.png" >nul
python "%~dp0dds.py" "%ASSET_OUT%\ankh.dds" || exit /b 1
python "%~dp0meshdump.py" "%ASSET_OUT%\ankh.mesh"
