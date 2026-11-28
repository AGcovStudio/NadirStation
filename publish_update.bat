@echo off
setlocal EnableExtensions
chcp 65001 >nul
rem ===========================================================================================================
rem  提交并推送到 GitHub（只推送代码，不动 Release 和安装包）。
rem  提交署名固定为 AGcovStudio；本脚本不保存任何密码或令牌，弹出的 GitHub 登录由你自己完成。
rem ===========================================================================================================
set "BRANCH=main"
cd /d "%~dp0"

where git >nul 2>&1 || (echo 没有找到 git。& goto :fail)

git config user.name "AGcovStudio"
git config user.email "AGcovStudio@users.noreply.github.com"
git config core.safecrlf false

echo [1/3] 提交改动
git rm -r -q --cached --ignore-unmatch docs/media >nul 2>&1
git add -A || goto :fail
git diff --cached --quiet
if errorlevel 1 (
  git commit -q -F installer\COMMIT_MSG_manual_rates.txt || goto :fail
) else (
  echo     没有新的改动需要提交。
)

echo [2/3] 提交作者确认
git log -1 --format="    %%an <%%ae>"

echo [3/3] 推送到 GitHub
git push origin %BRANCH% || (
  echo     推送被拒绝：GitHub 上可能有你本地没有的提交（例如在网页上合并了 PR）。
  echo     先运行 git pull --rebase origin %BRANCH%，再重新运行本脚本。
  goto :fail
)
echo.
echo 完成：https://github.com/AGcovStudio/NadirStation
goto :end

:fail
echo.
echo *** 推送没有完成，请看上面的提示。***
pause
exit /b 1
:end
pause
endlocal
