@echo off
rem N3V converter for Windows: installs what it needs the first time (Python 3.12 if missing, then numpy and
rem imageio-ffmpeg with its own ffmpeg, in a private .venv folder here), then opens the converter.
rem Run it on its own to pick videos in a window, or drag videos onto it.
setlocal
cd /d "%~dp0"
set "VPY=%~dp0.venv\Scripts\python.exe"
if exist "%VPY%" goto deps

echo Looking for Python 3.9 or newer...
set "PYCMD="
set "PYTEST=import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)"
py -3 -c "%PYTEST%" >nul 2>&1 && set "PYCMD=py -3"
if not defined PYCMD python -c "%PYTEST%" >nul 2>&1 && set "PYCMD=python"
if not defined PYCMD if exist "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" set PYCMD="%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
if defined PYCMD goto venv

echo Python not found. Downloading Python 3.12.8 from python.org...
curl -L --fail -o "%TEMP%\python-3.12.8-amd64.exe" https://www.python.org/ftp/python/3.12.8/python-3.12.8-amd64.exe || goto fail
echo Installing Python 3.12.8 for this user (takes a minute)...
"%TEMP%\python-3.12.8-amd64.exe" /quiet InstallAllUsers=0 PrependPath=1 Include_launcher=0 Include_test=0 Include_tcltk=1 || goto fail
del "%TEMP%\python-3.12.8-amd64.exe" >nul 2>&1
set PYCMD="%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
if not exist %PYCMD% goto fail

:venv
echo Making a private Python environment in %~dp0.venv ...
%PYCMD% -m venv "%~dp0.venv" || goto fail

:deps
"%VPY%" -c "import numpy, imageio_ffmpeg, tkinter; imageio_ffmpeg.get_ffmpeg_exe()" >nul 2>&1 && goto run
echo Installing numpy and ffmpeg (imageio-ffmpeg)...
"%VPY%" -m pip install --disable-pip-version-check -q --upgrade pip || goto fail
"%VPY%" -m pip install --disable-pip-version-check -q numpy imageio-ffmpeg || goto fail
"%VPY%" -c "import numpy, imageio_ffmpeg; print('ffmpeg ready:', imageio_ffmpeg.get_ffmpeg_exe())" || goto fail

:run
echo.
if "%~1"=="" (
	"%VPY%" n3vconvert.py
) else (
	for %%f in (%*) do "%VPY%" n3vconvert.py "%%~f"
)
echo.
echo Done. Copy the .n3v files to your 3DS's SD card (for example into a "videos" folder).
pause
exit /b 0

:fail
echo.
echo Setup failed. Check your internet connection and try again, or install Python 3.12 from python.org yourself.
pause
exit /b 1
