@echo off
rem Standalone settings editor. Packages carry their own Python (python\);
rem otherwise it needs Python 3.11+, PySide6 and tomli-w.
if exist "%~dp0..\python\python.exe" ("%~dp0..\python\python.exe" "%~dp0settings\app.py" %* & exit /b)
where python >nul 2>nul && (python "%~dp0settings\app.py" %* & exit /b)
py -3 "%~dp0settings\app.py" %*
