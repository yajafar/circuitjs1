@echo off
rem Windows release build with profile-guided optimization (PGO): MSVC + Ninja, Qt as in
rem build-win.bat.  Builds into build-pgo\ next to this script (set BUILD_DIR to change it).
rem
rem   build-win-pgo.bat [training-circuit.txt]
rem
rem 1. builds CircuitJS1, simbench and simtrace instrumented (they count which branches and calls run)
rem 2. runs them on the training circuit (default: the 7-segment decoder example), which writes
rem    .pgc profile files next to the .exe files; pass the circuits you use most for the best speed
rem 3. relinks them optimized with that profile
rem
rem PGO changes only how the code is laid out and inlined, not what it computes: results are the
rem same as a normal build.  The app ends up in build-pgo\CircuitJS1.exe.
setlocal
if not defined BUILD_DIR set "BUILD_DIR=%~dp0build-pgo"
set "TRAIN=%~1"
if "%TRAIN%"=="" set "TRAIN=%~dp0..\ts\public\circuits\7segdecoder.txt"
if not exist "%TRAIN%" (
    echo training circuit not found: %TRAIN%
    exit /b 1
)
rem vcvars64.bat runs vswhere.exe, which isn't on PATH by default; it also puts pgort140.dll
rem (which instrumented programs need) on PATH
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"

echo === 1/3: instrumented build
cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCIRCUITJS_PGO=GEN || exit /b 1
cmake --build "%BUILD_DIR%" --target CircuitJS1 simbench simtrace || exit /b 1
del /q "%BUILD_DIR%\*.pgc" 2>nul

echo === 2/3: training on %TRAIN%
"%BUILD_DIR%\simbench.exe" "%TRAIN%" --seconds 2 --steps 60000 --quiet || exit /b 1
set BENCH=1
"%BUILD_DIR%\simtrace.exe" "%TRAIN%" 30000 >nul || exit /b 1
set BENCH=
rem the app, in Fast Forward for 40 frames; start /wait waits for a GUI program
start /wait "" "%BUILD_DIR%\CircuitJS1.exe" --fast-forward --screenshot "%BUILD_DIR%\pgo-training.png" --frames 40 "%TRAIN%"

echo === 3/3: optimized relink
cmake -S . -B "%BUILD_DIR%" -DCIRCUITJS_PGO=USE || exit /b 1
cmake --build "%BUILD_DIR%" --target CircuitJS1 simbench simtrace || exit /b 1
echo === done: %BUILD_DIR%\CircuitJS1.exe
