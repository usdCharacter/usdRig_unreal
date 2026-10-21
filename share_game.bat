@echo off
rem Makes a zip of the game that plays on another Windows PC: packages a
rem clean copy (with Unreal's prerequisites installer, for the Visual C++
rem runtime) into Build\Share and zips it without debug symbols into
rem Build\RigExecBiped_Win64.zip. Unzip it anywhere and run RigExecBiped.exe
rem at its top; the whole folder has to stay together.
rem
rem With the editor open, set RIGEXEC_UAT_ARGS=-nocompileeditor first.
setlocal
set "RIGEXEC_GAME_OUT=%~dp0Build\Share"
if exist "%RIGEXEC_GAME_OUT%" rmdir /s /q "%RIGEXEC_GAME_OUT%"
set "RIGEXEC_UAT_ARGS=%RIGEXEC_UAT_ARGS% -prereqs"
call "%~dp0package_game.bat"
if errorlevel 1 exit /b 1
set "ZIP=%~dp0Build\RigExecBiped_Win64.zip"
if exist "%ZIP%" del "%ZIP%"
rem Only what plays: no debug symbols, logs or manifests, and none of the
rem developer tools the engine stages (Vulkan and D3D12 debug layers, the GPU
rem dump viewer; the game renders with D3D12), nor the ARM64 runtime.
rem Windows' own tar, which writes zips (Git's does not).
pushd "%RIGEXEC_GAME_OUT%\Windows"
"%SystemRoot%\System32\tar.exe" -a -c -f "%ZIP%" --exclude=*.pdb --exclude=Saved --exclude=Manifest_*.txt ^
    --exclude=VkLayer_* --exclude=d3d12SDKLayers.dll --exclude=GPUDumpViewer --exclude=vc_redist.arm64.exe *
set "ZIPPED=%errorlevel%"
popd
if not "%ZIPPED%"=="0" exit /b 1
echo shareable game: %ZIP%
exit /b 0
