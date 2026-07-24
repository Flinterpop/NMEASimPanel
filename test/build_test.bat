@echo off
REM Build and run the host-side tests for the NMEASimPanel simulator core.
REM nmea_sim.cpp has no Arduino dependency, so it compiles with plain MSVC.
REM Adjust VSPATH below if Visual Studio is installed elsewhere.

setlocal
set "VSPATH=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VSPATH%" (
    echo Could not find vcvars64.bat at:
    echo   %VSPATH%
    echo Edit VSPATH in this script to point at your Visual Studio install.
    exit /b 1
)

call "%VSPATH%" >nul 2>&1

cl /nologo /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I "%~dp0.." ^
   "%~dp0test_nmea.cpp" "%~dp0..\nmea_sim.cpp" /Fe:"%~dp0test_nmea.exe"
if errorlevel 1 exit /b 1

echo.
"%~dp0test_nmea.exe"
