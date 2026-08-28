@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0.."
if not exist ".tmp" mkdir ".tmp"
set "TEMP=%CD%\.tmp"
set "TMP=%CD%\.tmp"
set "BOOTSTRAP_PYTHON=python"
where py >nul 2>nul && set "BOOTSTRAP_PYTHON=py -3"
if not exist ".venv\Scripts\pip.exe" %BOOTSTRAP_PYTHON% -m venv --clear .venv
if errorlevel 1 exit /b %ERRORLEVEL%
".venv\Scripts\python.exe" -m pip install --upgrade pip
if errorlevel 1 exit /b %ERRORLEVEL%
".venv\Scripts\python.exe" -m pip install -r tools\requirements.txt
exit /b %ERRORLEVEL%
