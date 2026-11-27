@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================================
rem  NADIR Station - Windows build (Qt 6 MinGW 64-bit).
rem  Edit the three paths below if your installation differs, then run this file.
rem  Output: build\NADIR_Station.exe with the Qt runtime and resources\ deployed next to it.
rem ===========================================================================================
if "%QT_DIR%"==""      set "QT_DIR=C:\Qt\6.11.2\mingw_64"
if "%MINGW_DIR%"==""   set "MINGW_DIR=C:\Qt\Tools\mingw1310_64"
rem Optional, for ADALM-PLUTO support. Leave as is if you do not have libiio.
if "%LIBIIO_ROOT%"=="" set "LIBIIO_ROOT=C:\libiio"

cd /d "%~dp0"
set "BUILD=%~dp0build"

if not exist "%QT_DIR%\bin\qmake.exe" (
  echo Qt not found at "%QT_DIR%". Set QT_DIR to your Qt 6 MinGW kit, e.g. C:\Qt\6.8.3\mingw_64
  goto :fail
)
if not exist "%MINGW_DIR%\bin\g++.exe" (
  echo MinGW not found at "%MINGW_DIR%". Set MINGW_DIR to the MinGW that matches your Qt kit.
  goto :fail
)

rem CMake: use one on PATH, otherwise the one installed with Qt.
set "CMAKE=cmake"
where cmake >nul 2>&1
if errorlevel 1 (
  for %%D in ("%QT_DIR%\..\..\Tools\CMake_64" "C:\Qt\Tools\CMake_64") do (
    if exist "%%~D\bin\cmake.exe" set "CMAKE=%%~D\bin\cmake.exe"
  )
)
"%CMAKE%" --version >nul 2>&1 || (
  echo CMake not found. Install it with the Qt Maintenance Tool or from https://cmake.org
  goto :fail
)

set "PATH=%QT_DIR%\bin;%MINGW_DIR%\bin;%PATH%"

set "IIO_ARGS=-DNADIRSTATION_BUILD_PLUTO=ON"
if exist "%LIBIIO_ROOT%\include\iio.h" (
  set "IIO_LIB="
  for %%L in ("%LIBIIO_ROOT%\Windows-MinGW-W64\libiio.dll.a" "%LIBIIO_ROOT%\lib\libiio.dll.a") do if exist "%%~L" set "IIO_LIB=%%~L"
  if defined IIO_LIB (
    echo libiio: %LIBIIO_ROOT%
    set "IIO_ARGS=-DNADIRSTATION_BUILD_PLUTO=ON -DLIBIIO_INCLUDE_DIR=%LIBIIO_ROOT%\include -DLIBIIO_LIBRARY=!IIO_LIB!"
  )
) else (
  echo libiio not found: building without PlutoSDR ^(RTL-SDR and file playback still work^).
)

echo [1/3] Configure
"%CMAKE%" -S . -B "%BUILD%" -G "MinGW Makefiles" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="%MINGW_DIR%\bin\gcc.exe" ^
  -DCMAKE_CXX_COMPILER="%MINGW_DIR%\bin\g++.exe" ^
  -DCMAKE_MAKE_PROGRAM="%MINGW_DIR%\bin\mingw32-make.exe" ^
  -DCMAKE_PREFIX_PATH="%QT_DIR%" !IIO_ARGS! || goto :fail

echo [2/3] Build
"%CMAKE%" --build "%BUILD%" --parallel %NUMBER_OF_PROCESSORS% || goto :fail

echo [3/3] Deploy
set "EXE=%BUILD%\NADIR_Station.exe"
"%QT_DIR%\bin\windeployqt.exe" --release --compiler-runtime "%EXE%" || goto :fail
rem libiio first: its folder carries its own older MinGW runtime, which must not win.
if exist "%LIBIIO_ROOT%\Windows-MinGW-W64\libiio.dll" copy /y "%LIBIIO_ROOT%\Windows-MinGW-W64\*.dll" "%BUILD%\" >nul
for %%F in (libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll) do copy /y "%MINGW_DIR%\bin\%%F" "%BUILD%\" >nul
if exist "%BUILD%\libxml2-2.dll" copy /y "%BUILD%\libxml2-2.dll" "%BUILD%\libxml2.dll" >nul

echo.
echo Build complete: %EXE%
goto :end

:fail
echo.
echo *** BUILD FAILED - see the messages above. ***
if not defined NO_PAUSE pause
exit /b 1

:end
if not defined NO_PAUSE pause
endlocal
