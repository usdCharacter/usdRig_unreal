@echo off
rem Installs the Squarebit Eye plugin into the example project from a
rem SquarebitEye checkout, as source: the example is a C++ project, so its
rem build compiles the plugin for this engine. The biped's eyes then draw
rem with the Eye shader (URigExecEyeComponent). Optional: without it the
rem eyes keep the rig's own surface material.
rem
rem SQUAREBIT_EYE names the SquarebitEye checkout (default: ..\SquarebitEye).
setlocal
if not defined SQUAREBIT_EYE (for %%I in ("%~dp0..\SquarebitEye") do set "SQUAREBIT_EYE=%%~fI")
set "SOURCE=%SQUAREBIT_EYE%\unreal\SquarebitEye"
if not exist "%SOURCE%\SquarebitEye.uplugin" (
    echo no SquarebitEye checkout at %SQUAREBIT_EYE%; set SQUAREBIT_EYE
    exit /b 1
)
robocopy "%SOURCE%" "%~dp0Examples\RigExecBiped\Plugins\SquarebitEye" /MIR /XD Binaries Intermediate /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
echo installed into %~dp0Examples\RigExecBiped\Plugins\SquarebitEye; build the project next
exit /b 0
