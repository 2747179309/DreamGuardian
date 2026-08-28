@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0.."
if not exist ".venv\Scripts\python.exe" call tools\install_feishu_bridge_deps.bat
if errorlevel 1 exit /b %ERRORLEVEL%
".venv\Scripts\python.exe" tools\feishu_bridge.py --config tools\feishu_bridge_config.json
pause
