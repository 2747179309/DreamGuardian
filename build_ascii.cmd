@echo off
setlocal
chcp 65001 >nul

set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

rem Prefer an already activated ESP-IDF environment.
if defined IDF_PATH if exist "%IDF_PATH%\export.bat" goto :activate_idf

rem Detect the two layouts used by recent and older Espressif installers.
if exist "C:\Espressif\esp-idf-v5.5.4\export.bat" set "IDF_PATH=C:\Espressif\esp-idf-v5.5.4"
if not defined IDF_PATH if exist "C:\Espressif\frameworks\esp-idf-v5.5\export.bat" set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5"
if not defined IDF_PATH for /d %%D in ("C:\Espressif\esp-idf-v5.5*") do if exist "%%~fD\export.bat" set "IDF_PATH=%%~fD"

if not defined IDF_PATH (
    echo [ERROR] ESP-IDF 5.5 was not found.
    echo Install ESP-IDF 5.5.x, or set IDF_PATH before running this script.
    exit /b 2
)

:activate_idf
if not defined IDF_TOOLS_PATH if exist "C:\Espressif\tools" set "IDF_TOOLS_PATH=C:\Espressif\tools"
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

cd /d "%~dp0"
set "IDF_PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe"
if not exist "%IDF_PYTHON%" set "IDF_PYTHON=python"

if "%~1"=="" (
    "%IDF_PYTHON%" "%IDF_PATH%\tools\idf.py" -B build_local build
) else (
    "%IDF_PYTHON%" "%IDF_PATH%\tools\idf.py" -B build_local %*
)
exit /b %ERRORLEVEL%
