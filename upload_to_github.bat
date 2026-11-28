@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
rem ===========================================================================================================
rem  一键上传到 GitHub。双击运行即可（放在项目根目录，和 README.md 同一层）。
rem  需要：Git for Windows（https://git-scm.com/download/win）。
rem  可选：GitHub CLI（https://cli.github.com/），装了的话仓库不存在时会自动创建。
rem  登录：第一次推送时会弹出 GitHub 登录窗口，由你自己登录；本脚本不保存任何密码或令牌。
rem ===========================================================================================================
set "GH_OWNER=AGcovStudio"
set "GH_REPO=NadirStation"
set "BRANCH=main"
set "VISIBILITY=public"

cd /d "%~dp0"
if not exist "README.md" (
  echo 请把本脚本放在项目根目录（和 README.md 同一层）再运行。
  goto :fail
)

where git >nul 2>&1 || (
  echo 没有找到 git。请先安装 Git for Windows：https://git-scm.com/download/win
  goto :fail
)

rem ---- 提交者信息：只写进本仓库（不改全局设置），提交显示为 AGcovStudio，不关联个人账号
set "COMMIT_NAME=AGcovStudio"
set "COMMIT_EMAIL=AGcovStudio@users.noreply.github.com"

rem ---- 仓库与提交
if not exist ".git" (
  echo [1/4] 初始化本地仓库
  git init -b %BRANCH% || goto :fail
) else (
  echo [1/4] 本地仓库已存在
)
git checkout -B %BRANCH% >nul 2>&1
git config user.name "%COMMIT_NAME%"
git config user.email "%COMMIT_EMAIL%"
rem ---- 只对本仓库：不再刷屏提示 "LF will be replaced by CRLF"（仓库里统一存 LF，不影响内容）
git config core.safecrlf false

echo [2/4] 添加文件（build、录音、密钥已在 .gitignore 中排除）
git rm -r -q --cached --ignore-unmatch docs/media >nul 2>&1
git add -A || goto :fail
git diff --cached --quiet
if errorlevel 1 (
  set "MSG=NADIR Station 2.0.0, released under AGPL-3.0"
  git rev-parse --verify HEAD >nul 2>&1
  if not errorlevel 1 (
    set "MSG="
    set /p "MSG=提交说明（直接回车用默认 WIP: update）: "
    if "!MSG!"=="" set "MSG=WIP: update"
  )
  git commit -m "!MSG!" || goto :fail
) else (
  echo     没有新的改动需要提交。
)

rem ---- 远程仓库
set "URL=https://github.com/%GH_OWNER%/%GH_REPO%.git"
git remote get-url origin >nul 2>&1 || git remote add origin "%URL%"
git remote set-url origin "%URL%"

echo [3/4] 检查 GitHub 上的仓库 %GH_OWNER%/%GH_REPO%
git ls-remote origin >nul 2>&1
if errorlevel 1 (
  where gh >nul 2>&1
  if errorlevel 1 (
    echo     GitHub 上还没有这个仓库（或还没登录）。
    echo     请先在浏览器打开 https://github.com/new ，Owner 选 %GH_OWNER%，仓库名填 %GH_REPO%，
    echo     不要勾选 README / .gitignore / License，创建后再运行本脚本。
    echo     也可以安装 GitHub CLI 后重新运行，脚本会自动创建仓库。
    goto :fail
  )
  gh auth status >nul 2>&1 || gh auth login || goto :fail
  echo     用 GitHub CLI 创建仓库（%VISIBILITY%）
  gh repo create "%GH_OWNER%/%GH_REPO%" --%VISIBILITY% --description "by AGcov(BI2QYN), A meteorological satellite reception and processing software" || goto :fail
)

echo [4/4] 推送到 %URL%
git push -u origin %BRANCH% || goto :fail

echo.
echo 完成：https://github.com/%GH_OWNER%/%GH_REPO%
goto :end

:fail
echo.
echo *** 上传没有完成，请看上面的提示。***
pause
exit /b 1

:end
pause
endlocal
