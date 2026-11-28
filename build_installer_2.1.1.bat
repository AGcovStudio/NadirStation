@echo off
setlocal EnableExtensions
chcp 65001 >nul
rem 一键编译并打包 NADIR Station 2.1.1 安装包：先 build_windows.bat，再 make_installer.bat。
rem 需要 Qt 6 + MinGW（和 build_windows.bat 一样）以及 Inno Setup 6.3+。
rem 输出：dist\NADIR_Station_2.1.1_win10_x64_setup.exe（旧的 2.1.0 安装包不动）。
cd /d "%~dp0"
set "NO_PAUSE=1"
echo [1/2] 编译程序
call build_windows.bat || goto :fail
echo.
echo [2/2] 生成安装包
call make_installer.bat || goto :fail
if not exist "dist\NADIR_Station_2.1.1_win10_x64_setup.exe" (echo 没有找到 2.1.1 安装包。& goto :fail)
echo.
echo 完成：%CD%\dist\NADIR_Station_2.1.1_win10_x64_setup.exe
start "" explorer /select,"%CD%\dist\NADIR_Station_2.1.1_win10_x64_setup.exe"
goto :end
:fail
echo.
echo *** 没有完成，请看上面的提示。***
pause
exit /b 1
:end
pause
endlocal
