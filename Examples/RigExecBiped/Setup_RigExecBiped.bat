@echo off
rem Rebuilds the example project's Control Rig, level and sequence from its
rem RigExecData by running Scripts\setup_example_project.py once as a project
rem startup script (passed on the command line, so no config is changed).
rem The editor opens, builds and saves the content, then quits; the log
rem (Saved\Logs\RigExecBiped.log) says RIGEXEC_SETUP_OK or
rem RIGEXEC_SETUP_FAILED. It builds the project (and the plugin, from
rem ..\..\Plugins) first; close any editor open on the project. UE_ROOT
rem names the engine install.
setlocal
if not defined UE_ROOT set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
for %%I in ("%~dp0..\..\Scripts\setup_example_project.py") do set "SETUP=%%~fI"
set "SETUP=%SETUP:\=/%"
rem The level and sequence are rebuilt from scratch; removing them first
rem keeps the editor from opening (and so locking) the old level.
del /q "%~dp0Content\RigExec\L_RigExec.umap" "%~dp0Content\RigExec\LS_RigExec.uasset" "%~dp0Content\RigExec\L_RigExecGame.umap" "%~dp0Content\RigExec\L_RigExecPlatformer.umap" 2>nul
rem The game animates the biped from the engine's own UE5 Mannequin and its
rem locomotion (the Third Person template's content), copied in here; it
rem is Epic's content, so it is not kept in this repository.
robocopy "%UE_ROOT%\Templates\TemplateResources\High\Characters\Content\Mannequins" "%~dp0Content\Characters\Mannequins" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
    echo could not copy the Mannequin from %UE_ROOT%\Templates
    exit /b 1
)
rem The project is C++ (its game module and the plugin build together).
call "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" RigExecBipedEditor Win64 Development "-Project=%~dp0RigExecBiped.uproject" -WaitMutex -NoHotReload || exit /b 1
"%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" "%~dp0RigExecBiped.uproject" ^
    "-ini:Engine:[/Script/PythonScriptPlugin.PythonScriptPluginSettings]:StartupScripts=%SETUP%" ^
    -log -unattended
findstr /C:"RIGEXEC_SETUP_OK" "%~dp0Saved\Logs\RigExecBiped.log" >nul || (
    echo setup failed; see %~dp0Saved\Logs\RigExecBiped.log
    exit /b 1
)
echo example project rebuilt
exit /b 0
