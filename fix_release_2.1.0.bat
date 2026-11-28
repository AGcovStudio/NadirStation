@echo off
setlocal EnableExtensions
chcp 65001 >nul
rem 重新发布 v2.1.0 的安装包：换上 2.1.0 安装包，删掉误传的 2.0.0 安装包，补上发布说明。
rem 不保存任何密码或令牌；需要登录时由你自己完成。
set "GH_REPO=AGcovStudio/NadirStation"
set "TAG=v2.1.0"
set "SETUP=dist\NADIR_Station_2.1.0_win10_x64_setup.exe"
set "OLD=NADIR-Station-2.0.0-Windows-x64-Setup.exe"
set "NOTES=installer\RELEASE_NOTES_2.1.0.md"
cd /d "%~dp0"
if not exist "%SETUP%" (echo 找不到 %SETUP% & goto :fail)

where gh >nul 2>&1
if errorlevel 1 goto :manual

gh auth status >nul 2>&1 || gh auth login || goto :fail
echo [1/3] 上传 2.1.0 安装包
gh release upload %TAG% "%SETUP%" --repo %GH_REPO% --clobber || goto :fail
echo [2/3] 删除误传的 2.0.0 安装包
gh release delete-asset %TAG% "%OLD%" --repo %GH_REPO% -y >nul 2>&1
echo [3/3] 写入发布说明
gh release edit %TAG% --repo %GH_REPO% --title "NADIR Station 2.1.0" --notes-file "%NOTES%" || goto :fail
echo.
echo 完成：https://github.com/%GH_REPO%/releases/tag/%TAG%
goto :end

:manual
echo 没有 GitHub CLI，改为网页操作：
echo   1. 浏览器会打开 v2.1.0 的编辑页面；
echo   2. 把旧附件 %OLD% 右边的删除按钮点掉；
echo   3. 把资源管理器里选中的 2.1.0 安装包拖进附件框；
echo   4. 发布说明已复制到剪贴板，在说明框里 Ctrl+V 粘贴；
echo   5. 点 Update release。
powershell -NoProfile -Command "Get-Content -Raw -Encoding UTF8 '%NOTES%' | Set-Clipboard"
start "" "https://github.com/%GH_REPO%/releases/edit/%TAG%"
start "" explorer /select,"%CD%\%SETUP%"
goto :end

:fail
echo.
echo *** 没有完成，请看上面的提示。***
pause
exit /b 1
:end
pause
endlocal
