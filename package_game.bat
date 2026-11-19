@echo off
rem Packages the example as a Win64 game: the biped running around, posed by
rem the RigExec runtime from its baked character asset (no USD at runtime),
rem and the animation studio (L_RigExecStudio, from the pause menu).
rem Run Examples\RigExecBiped\Setup_RigExecBiped.bat once first (it copies
rem the Mannequin, bakes the character and builds the game level).
rem
rem The game lands in Build\Game\Windows\RigExecBiped.exe. UE_ROOT names the
rem engine install; RIGEXEC_GAME_OUT the archive folder; CONFIG the build
rem configuration (Development by default, or Shipping). RIGEXEC_UAT_ARGS
rem adds arguments: -nocompileeditor packages with the editor open (its
rem Live Coding blocks rebuilding the editor modules the cook uses).
setlocal
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
if not defined RIGEXEC_GAME_OUT set "RIGEXEC_GAME_OUT=%~dp0Build\Game"
if not defined CONFIG set "CONFIG=Development"
if not exist "%~dp0Examples\RigExecBiped\Content\RigExec\RXC_Biped.uasset" (
    echo no baked character; run Examples\RigExecBiped\Setup_RigExecBiped.bat first
    exit /b 1
)
call "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun ^
    -project="%~dp0Examples\RigExecBiped\RigExecBiped.uproject" ^
    -noP4 -platform=Win64 -clientconfig=%CONFIG% ^
    -build -cook -stage -pak -iostore -archive -archivedirectory="%RIGEXEC_GAME_OUT%" ^
    -map=/Game/RigExec/L_RigExecGame+/Game/RigExec/L_RigExecPlatformer+/Game/RigExec/L_RigExecStudio -unattended -utf8output %RIGEXEC_UAT_ARGS%
if errorlevel 1 exit /b 1
rem The headset's way in: the same game with -vr (a Quest over Link or Air
rem Link, with Meta Quest Link as the PC's active OpenXR runtime). DirectX
rem 11: under 12 the Link headset stayed black.
(
    echo @echo off
    echo rem Plays the RigExec animation studio in a VR headset ^(OpenXR^).
    echo start "" "%%~dp0RigExecBiped.exe" -vr -dx11 %%*
) > "%RIGEXEC_GAME_OUT%\Windows\RigExecBiped_VR.bat"
echo packaged into %RIGEXEC_GAME_OUT%\Windows
exit /b 0
