@echo off
setlocal EnableExtensions
chcp 65001 >nul
rem ===========================================================================================================
rem  一键发布 NADIR Station 2.1.1：提交并推送代码，再把 Windows 安装包发布到 GitHub Release v2.1.1。
rem  先运行 build_installer_2.1.1.bat 生成安装包。
rem  需要：Git for Windows；发布安装包需要 GitHub CLI（https://cli.github.com/）。
rem  提交署名固定为 AGcovStudio。登录由你自己完成；本脚本不保存任何密码或令牌。
rem ===========================================================================================================
set "GH_REPO=AGcovStudio/NadirStation"
set "TAG=v2.1.1"
set "SETUP=dist\NADIR_Station_2.1.1_win10_x64_setup.exe"
set "NOTES=installer\RELEASE_NOTES_2.1.1.md"
set "BRANCH=main"
cd /d "%~dp0"

where git >nul 2>&1 || (echo 没有找到 git。& goto :fail)
if not exist "%SETUP%" (
  echo 找不到 %SETUP%。请先运行 build_installer_2.1.1.bat。
  goto :fail
)

echo [1/3] 提交改动
git config user.name "AGcovStudio"
git config user.email "AGcovStudio@users.noreply.github.com"
git config core.safecrlf false
git rm -r -q --cached --ignore-unmatch docs/media >nul 2>&1
git add -A || goto :fail
git diff --cached --quiet
if errorlevel 1 (
  git commit -q -F installer\COMMIT_MSG_manual_rates.txt || goto :fail
) else (
  echo     没有新的改动需要提交。
)
git log -1 --format="    提交作者：%%an <%%ae>"

echo [2/3] 推送代码
git push origin %BRANCH% || (
  echo     推送被拒绝：GitHub 上可能有你本地没有的提交（例如在网页上合并了 PR）。
  echo     先运行 git pull --rebase origin %BRANCH%，再重新运行本脚本。
  goto :fail
)

echo [3/3] 发布安装包到 GitHub Release %TAG%
where gh >nul 2>&1
if errorlevel 1 (
  echo     没有 GitHub CLI，改为手动发布：浏览器会打开新建 Release 的页面，
  echo     把打开的文件夹里的安装包拖进去；发布说明已复制到剪贴板，在说明框里 Ctrl+V 粘贴。
  powershell -NoProfile -Command "Get-Content -Raw -Encoding UTF8 '%NOTES%' | Set-Clipboard"
  start "" "https://github.com/%GH_REPO%/releases/new?tag=%TAG%&title=NADIR%%20Station%%202.1.1"
  start "" explorer /select,"%CD%\%SETUP%"
  goto :end
)
gh auth status >nul 2>&1 || gh auth login || goto :fail
gh release view %TAG% --repo %GH_REPO% >nul 2>&1
if errorlevel 1 (
  gh release create %TAG% "%SETUP%" --repo %GH_REPO% --target %BRANCH% --title "NADIR Station 2.1.1" --notes-file "%NOTES%" || goto :fail
) else (
  gh release upload %TAG% "%SETUP%" --repo %GH_REPO% --clobber || goto :fail
  gh release edit %TAG% --repo %GH_REPO% --title "NADIR Station 2.1.1" --notes-file "%NOTES%" || goto :fail
)
echo.
echo 完成：https://github.com/%GH_REPO%/releases/tag/%TAG%
goto :end

:fail
echo.
echo *** 发布没有完成，请看上面的提示。***
pause
exit /b 1
:end
pause
endlocal
