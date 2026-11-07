@echo off
rem Drag one or more videos onto this file to make .n3v files next to them.
for %%f in (%*) do python "%~dp0n3vconvert.py" "%%~f"
pause
