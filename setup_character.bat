@echo off
rem Bakes another character (RIGEXEC_CHARACTER, from RigExecData\<Name>.rigexec
rem and the stage RIGEXEC_CHARACTER_STAGE) into the example project by running
rem Scripts\setup_character.py as an unattended editor startup script. Close
rem any editor open on the project first. The game lists every baked
rem character on its CHOOSE CHARACTER screen.
setlocal
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
if not defined RIGEXEC_CHARACTER (echo set RIGEXEC_CHARACTER & exit /b 1)
if not defined RIGEXEC_CHARACTER_STAGE (echo set RIGEXEC_CHARACTER_STAGE & exit /b 1)
set "PROJECT=%~dp0Examples\RigExecBiped"
for %%I in ("%~dp0Scripts\setup_character.py") do set "SETUP=%%~fI"
set "SETUP=%SETUP:\=/%"
set "RIGEXEC_QUIT=1"
"%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" "%PROJECT%\RigExecBiped.uproject" ^
    "-ini:Engine:[/Script/PythonScriptPlugin.PythonScriptPluginSettings]:StartupScripts=%SETUP%" ^
    -log -unattended -nosplash
findstr /C:"RIGEXEC_SETUP_OK" "%PROJECT%\Saved\Logs\RigExecBiped.log" >nul || (
    echo setup failed; see %PROJECT%\Saved\Logs\RigExecBiped.log
    exit /b 1
)
echo baked %RIGEXEC_CHARACTER%
exit /b 0
