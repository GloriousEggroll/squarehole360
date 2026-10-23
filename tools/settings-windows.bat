@echo off
rem Standalone settings editor (needs Python 3.11+, PySide6 and tomli-w).
where python >nul 2>nul && (python "%~dp0settings\app.py" %* & exit /b) 
py -3 "%~dp0settings\app.py" %*
