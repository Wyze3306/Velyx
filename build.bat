@echo off
REM Builds Velyx on Windows with Visual Studio (x64). See docs/BUILDING.md.

setlocal

cd /d "%~dp0"

set BUILD_TYPE=Release
set CONSOLE=OFF

if /I "%1"=="debug" (
    set BUILD_TYPE=Debug
    set CONSOLE=ON
)

cmake -B build -G "Visual Studio 17 2022" -A x64 -DVELYX_CONSOLE=%CONSOLE%
if errorlevel 1 goto :error

cmake --build build --config %BUILD_TYPE% --parallel
if errorlevel 1 goto :error

if not exist "build\bin\%BUILD_TYPE%\assets" mkdir "build\bin\%BUILD_TYPE%\assets"
xcopy /E /I /Y assets "build\bin\%BUILD_TYPE%\assets" >nul

echo.
echo   build\bin\%BUILD_TYPE%\Velyx.dll  client
echo   build\bin\%BUILD_TYPE%\Velyx.exe  launcher
echo.
goto :eof

:error
echo Build failed.
exit /b 1
