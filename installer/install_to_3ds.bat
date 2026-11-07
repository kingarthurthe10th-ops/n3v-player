@echo off
rem Installs N3VPlayer.cia on your 3DS over Wi-Fi with FBI. Opens a page with the steps.
cd /d "%~dp0"
set "P="
if exist "converter\.venv\Scripts\python.exe" set P="converter\.venv\Scripts\python.exe"
if not defined P py -3 -c "" >nul 2>&1 && set "P=py -3"
if not defined P python -c "" >nul 2>&1 && set "P=python"
if not defined P (
	echo Python is needed. Run converter\setup.bat once ^(it installs Python^), then try again.
	pause
	exit /b 1
)
if exist "%~dp0install_to_3ds.py" (%P% "%~dp0install_to_3ds.py" %*) else (%P% "%~dp0installer\install_to_3ds.py" %*)
echo.
pause
