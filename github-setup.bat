@echo off
setlocal EnableExtensions
title TGRe FPS Unlocker - GitHub setup

rem ====== EDIT THESE IF NEEDED ======
set "REPO_URL=https://github.com/Wheresdom-ux/TGRe-FPS-Unlocker.git"
set "BRANCH=main"
set "COMMIT_MSG=Initial commit"
rem ==================================

cd /d "%~dp0"
set "EXITCODE=0"

echo.
echo ============================================================
echo   TGRe FPS Unlocker : GitHub setup and push
echo ============================================================
echo   Folder: %CD%
echo   Remote: %REPO_URL%
echo.

rem ---------------------------------------------------------------
echo [1/7] Checking for Git...
where git >nul 2>&1
if errorlevel 1 goto :no_git
git --version

rem ---------------------------------------------------------------
echo.
echo [2/7] Checking repository...
if exist ".git" goto :trust_check
git init
if errorlevel 1 goto :fail
git symbolic-ref HEAD refs/heads/%BRANCH%
echo     Created a new repository.
goto :repo_ready

:trust_check
git status --short >nul 2>&1
if not errorlevel 1 goto :found_repo
echo     Git does not trust this folder ^(common under Program Files^). Marking it as safe...
set "SAFEDIR=%CD:\=/%"
git config --global --add safe.directory "%SAFEDIR%"
git status --short >nul 2>&1
if errorlevel 1 goto :fail
:found_repo
echo     Found existing repository.

:repo_ready
rem Use whatever branch the repo is already on (falls back to BRANCH above).
for /f "delims=" %%B in ('git branch --show-current 2^>nul') do set "BRANCH=%%B"
echo     Branch: %BRANCH%

rem ---------------------------------------------------------------
echo.
echo [3/7] Checking .gitignore...
if exist ".gitignore" goto :ignore_ready
if not exist ".gitignore.txt" goto :ignore_ready
ren ".gitignore.txt" ".gitignore"
echo     Renamed .gitignore.txt to .gitignore ^(Windows had hidden the real extension^).
:ignore_ready
set "NEEDNL=1"
call :ensure_ignore ".vs/"
call :ensure_ignore "build/"
call :ensure_ignore "backups/"
call :ensure_ignore "install-state.json"
call :ensure_ignore "*.log"
echo     .gitignore is OK.

rem ---------------------------------------------------------------
echo.
echo [4/7] Staging files ^(build/, backups/, .vs/ and other ignored files are left out^)...
rem Un-stage everything first so files that were committed before .gitignore worked get dropped.
git rm -r -q --cached --ignore-unmatch . >nul 2>&1
git -c core.safecrlf=false add -A
if errorlevel 1 goto :fail
set "COUNT=0"
for /f %%N in ('git ls-files 2^>nul ^| find /c /v ""') do set "COUNT=%%N"
echo     %COUNT% files will be in the repository.

rem ---------------------------------------------------------------
echo.
echo [5/7] Setting remote 'origin'...
git remote get-url origin >nul 2>&1
if errorlevel 1 (
    git remote add origin %REPO_URL%
) else (
    git remote set-url origin %REPO_URL%
)
if errorlevel 1 goto :fail
echo     origin = %REPO_URL%

rem ---------------------------------------------------------------
echo.
echo [6/7] Checking GitHub ^(a browser window may ask you to sign in^)...
git ls-remote --exit-code --heads origin %BRANCH% >nul
set "LSR=%errorlevel%"
if "%LSR%"=="0" goto :remote_has_branch
if "%LSR%"=="2" goto :remote_empty
goto :remote_error

:remote_has_branch
echo     The remote already has a '%BRANCH%' branch. Existing history will be kept.
goto :make_commit

:remote_empty
echo     The remote is empty, so nothing has been pushed yet.
set "JUNK="
for /f "delims=" %%L in ('git log --all -1 --oneline -- build backups .vs install-state.json 2^>nul') do set "JUNK=1"
if not defined JUNK goto :make_commit
echo.
echo     Your existing local commit contains build/, backups/ and .vs/ files
echo     ^(compiled DLLs and Visual Studio cache^) from before .gitignore was working.
echo     Because nothing is on GitHub yet, it is cleanest to replace that commit
echo     with one clean commit. Your files on disk are NOT touched.
echo.
choice /c YN /m "Replace the old local commit with a clean one"
if errorlevel 2 goto :make_commit
git checkout --orphan clean-start
if errorlevel 1 goto :fail
git branch -D %BRANCH%
git branch -m %BRANCH%
if errorlevel 1 goto :fail
echo     Done. History reset to a single clean commit.

rem ---------------------------------------------------------------
:make_commit
echo.
echo [7/7] Committing and pushing...
git diff --cached --quiet
if not errorlevel 1 goto :nothing_to_commit
git commit -m "%COMMIT_MSG%"
if errorlevel 1 goto :commit_failed
goto :push
:nothing_to_commit
echo     Nothing new to commit.

:push
git push -u origin %BRANCH%
if errorlevel 1 goto :push_failed

set "REPO_WEB=%REPO_URL:.git=%"
echo.
echo ============================================================
echo   SUCCESS - your project is on GitHub:
echo   %REPO_WEB%
echo ============================================================
echo   The repo is still private. When you are happy with it, go to
echo   Settings ^> General ^> Danger Zone ^> Change visibility.
goto :done

rem ---------------------------------------------------------------
:no_git
echo.
echo [ERROR] Git is not installed or not on PATH.
echo         Install Git for Windows from https://git-scm.com/download/win
echo         then run this script again.
goto :fail

:remote_error
echo.
echo [ERROR] Could not reach %REPO_URL%
echo         - Check your internet connection and that the repo name is right.
echo         - If a sign-in window appeared, finish signing in and run this again.
echo         - For private repos you may need a personal access token instead of a password.
goto :fail

:commit_failed
echo.
echo [ERROR] Commit failed. If Git says it does not know who you are, run:
echo           git config --global user.name "Your Name"
echo           git config --global user.email "you@example.com"
echo         then run this script again.
goto :fail

:push_failed
echo.
echo [ERROR] Push failed. Common fixes:
echo   - The GitHub repo already has files ^(e.g. a README made on github.com^):
echo       git pull origin %BRANCH% --allow-unrelated-histories
echo     resolve any conflicts, then run this script again.
echo   - Sign-in problem: use a personal access token as the password
echo     ^(GitHub ^> Settings ^> Developer settings ^> Personal access tokens^).
echo   - ONLY if the GitHub repo is disposable and you want to overwrite it:
echo       git push -u origin %BRANCH% --force
goto :fail

:fail
set "EXITCODE=1"
echo.
echo Something went wrong - see the messages above.

:done
echo.
pause
exit /b %EXITCODE%

rem ---------------------------------------------------------------
:ensure_ignore
if not exist ".gitignore" goto :ei_add
findstr /x /l /c:%1 ".gitignore" >nul 2>&1
if not errorlevel 1 goto :eof
:ei_add
if defined NEEDNL if exist ".gitignore" echo.>> ".gitignore"
set "NEEDNL="
>> ".gitignore" echo %~1
echo     Added to .gitignore: %~1
goto :eof
