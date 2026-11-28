@echo off
setlocal EnableExtensions
chcp 65001 >nul
rem ===========================================================================================================
rem  一键发布 NADIR Station 2.1.0：提交并推送代码，再把 Windows 安装包发布到 GitHub Release。
rem  需要：Git for Windows；发布安装包需要 GitHub CLI（https://cli.github.com/）。
rem  登录：弹出的 GitHub 登录由你自己完成；本脚本不保存任何密码或令牌。
rem ===========================================================================================================
set "GH_REPO=AGcovStudio/NadirStation"
set "TAG=v2.1.0"
set "SETUP=dist\NADIR_Station_2.1.0_win10_x64_setup.exe"
cd /d "%~dp0"

where git >nul 2>&1 || (echo 没有找到 git。& goto :fail)
if not exist "%SETUP%" (
  echo [0/3] 还没有安装包，先编译并打包
  set "NO_PAUSE=1"
  call build_windows.bat || goto :fail
  call make_installer.bat || goto :fail
)

echo [1/3] 提交改动
git config user.name "AGcovStudio"
git config user.email "AGcovStudio@users.noreply.github.com"
git config core.safecrlf false
git rm -r -q --cached --ignore-unmatch docs/media >nul 2>&1
git add -A || goto :fail
git diff --cached --quiet
if errorlevel 1 (
  git commit -q -F installer\COMMIT_MSG_2.1.0.txt || goto :fail
) else (
  echo     没有新的改动需要提交。
)

echo [2/3] 推送代码
git push origin main || (
  echo     推送被拒绝：GitHub 上可能有你本地没有的提交（例如在网页上合并了 PR）。
  echo     先运行 git pull --rebase origin main，再重新运行本脚本。
  goto :fail
)

echo [3/3] 发布安装包到 GitHub Release %TAG%
where gh >nul 2>&1
if errorlevel 1 (
  echo     没有 GitHub CLI，改为手动发布：浏览器会打开新建 Release 的页面，
  echo     把打开的文件夹里的安装包拖进去，说明可以复制 installer\RELEASE_NOTES_2.1.0.md。
  start "" "https://github.com/%GH_REPO%/releases/new?tag=%TAG%&title=NADIR%%20Station%%202.1.0"
  start "" explorer /select,"%CD%\%SETUP%"
  goto :end
)
gh auth status >nul 2>&1 || gh auth login || goto :fail
gh release view %TAG% --repo %GH_REPO% >nul 2>&1
if errorlevel 1 (
  gh release create %TAG% "%SETUP%" --repo %GH_REPO% --target main --title "NADIR Station 2.1.0" --notes-file installer\RELEASE_NOTES_2.1.0.md || goto :fail
) else (
  gh release upload %TAG% "%SETUP%" --repo %GH_REPO% --clobber || goto :fail
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
