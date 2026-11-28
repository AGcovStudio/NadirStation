@echo off
setlocal EnableExtensions
rem ===========================================================================================
rem  NADIR Station - Windows installer. Run build_windows.bat first.
rem  Needs Inno Setup 6.3+ (https://jrsoftware.org/isdl.php). Output: dist\NADIR_Station_*_setup.exe
rem ===========================================================================================
cd /d "%~dp0"
set "BUILD=%~dp0build"
set "STAGE=%~dp0dist\stage"
if not exist "%BUILD%\NADIR_Station.exe" (
  echo build\NADIR_Station.exe not found. Run build_windows.bat first.
  goto :fail
)
set "ISCC="
for %%I in ("%ProgramFiles%\Inno Setup 7\ISCC.exe" "%ProgramFiles(x86)%\Inno Setup 7\ISCC.exe" "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe" "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe") do if exist "%%~I" set "ISCC=%%~I"
if not defined ISCC (
  echo Inno Setup not found. Install it from https://jrsoftware.org/isdl.php
  goto :fail
)

echo [1/2] Stage the program files
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
rem Only what the program needs: the executable, the Qt/MinGW/libiio DLLs, Qt plugins, resources.
rem The command-line test tools, build files, logs and decoded data stay out.
robocopy "%BUILD%" "%STAGE%" NADIR_Station.exe *.dll /NFL /NDL /NJH /NJS /NP >nul
robocopy "%BUILD%\resources" "%STAGE%\resources" /E /NFL /NDL /NJH /NJS /NP /XD gk2a >nul
for %%D in (platforms styles imageformats iconengines generic networkinformation tls) do (
  if exist "%BUILD%\%%D" robocopy "%BUILD%\%%D" "%STAGE%\%%D" /E /NFL /NDL /NJH /NJS /NP >nul
)
copy /y LICENSE "%STAGE%\LICENSE.txt" >nul
copy /y README.md "%STAGE%\README.md" >nul
copy /y THIRD_PARTY_NOTICES.md "%STAGE%\THIRD_PARTY_NOTICES.md" >nul
if exist "%STAGE%\resources\gk2a" rmdir /s /q "%STAGE%\resources\gk2a"
if exist "%STAGE%\EncryptionKeyMessage.bin" del /q "%STAGE%\EncryptionKeyMessage.bin"

echo [2/2] Compile the installer
"%ISCC%" /Q "installer\nadirstation.iss" || goto :fail
echo.
for %%F in ("%~dp0dist\NADIR_Station_*_setup.exe") do echo Installer: %%~fF
goto :end

:fail
echo.
echo *** INSTALLER FAILED - see the messages above. ***
if not defined NO_PAUSE pause
exit /b 1
:end
if not defined NO_PAUSE pause
exit /b 0
