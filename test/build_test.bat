@echo off
REM Build and run the host-side tests for the NMEASimPanel simulator core.
REM nmea_sim.cpp and ais_sim.cpp have no Arduino dependency, so they compile
REM with plain MSVC. Adjust VSPATH below if Visual Studio is installed elsewhere.

setlocal
set "VSPATH=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VSPATH%" (
    echo Could not find vcvars64.bat at:
    echo   %VSPATH%
    echo Edit VSPATH in this script to point at your Visual Studio install.
    exit /b 1
)

call "%VSPATH%" >nul 2>&1

set "RC=0"

cl /nologo /EHsc /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I "%~dp0.." ^
   "%~dp0test_nmea.cpp" "%~dp0..\nmea_sim.cpp" /Fe:"%~dp0test_nmea.exe"
if errorlevel 1 exit /b 1

cl /nologo /EHsc /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I "%~dp0.." ^
   "%~dp0test_ais.cpp" "%~dp0..\ais_sim.cpp" /Fe:"%~dp0test_ais.exe"
if errorlevel 1 exit /b 1

cl /nologo /EHsc /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I "%~dp0.." ^
   "%~dp0test_play.cpp" "%~dp0..\ais_play.cpp" /Fe:"%~dp0test_play.exe"
if errorlevel 1 exit /b 1

echo.
echo ===== GPS / NMEA =====
"%~dp0test_nmea.exe"
call :note nmea %%ERRORLEVEL%%

echo.
echo ===== AIS =====
"%~dp0test_ais.exe"
call :note ais %%ERRORLEVEL%%

echo.
echo ===== AIS playback =====
"%~dp0test_play.exe"
call :note play %%ERRORLEVEL%%

echo.
if "%RC%"=="0" (echo BUILD_TEST: all suites passed) else (echo BUILD_TEST: FAILURES)
exit /b %RC%

REM Report a suite's exit code and latch any failure. A crashing test (an assert
REM abort exits 3, an access violation exits with a large value) must fail the
REM run just as loudly as a reported [FAIL], so compare against 0 explicitly
REM rather than relying on `if errorlevel`.
:note
echo suite %1 exit code: %2
if not "%2"=="0" set "RC=1"
goto :eof
