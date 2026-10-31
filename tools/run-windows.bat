@echo off
rem Windows counterpart of tools/run-linux.sh: asset preflight, default paths
rem inside the checkout, then the game. Extra arguments go to gow3.exe.
rem Plain cmd so it also runs under Wine/Proton, whose powershell is a stub.
setlocal EnableExtensions
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
cd /d "%ROOT%"
set "EXE=%ROOT%\build-windows\bin\gow3.exe"
if exist "%EXE%" goto have_exe
echo Missing %EXE%; build it with tools/build-windows.sh. 1>&2
exit /b 1
:have_exe

rem Packages carry their own Python with PySide6 and tomli-w. Paths stay
rem quoted inside blocks because the install path may contain parentheses.
set "PY=" & set "PY_ARGS="
if exist "%ROOT%\python\python.exe" set "PY=%ROOT%\python\python.exe"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY where py >nul 2>nul && set "PY=py" && set "PY_ARGS=-3"

rem CLI11 rejects repeated options, so a default is omitted when the caller
rem supplies it. A custom asset directory is honored by the preflight too.
set "GAME_DATA=%ROOT%\game"
set "HAS_GAME_DATA=" & set "HAS_USER_DATA=" & set "HAS_LOG=" & set "HAS_CONFIG="
set "CHECK_ASSETS=1"
:scan
if "%~1"=="" goto scanned
set "ARG=%~1"
if "%ARG%"=="--game_data_root" (set "HAS_GAME_DATA=1" & set "GAME_DATA=%~2" & shift)
if "%ARG:~0,17%"=="--game_data_root=" (set "HAS_GAME_DATA=1" & set "GAME_DATA=%ARG:~17%")
if "%ARG:~0,16%"=="--user_data_root" set "HAS_USER_DATA=1"
if "%ARG:~0,10%"=="--log_file" set "HAS_LOG=1"
if "%ARG:~0,13%"=="--gow3_config" set "HAS_CONFIG=1"
if "%ARG%"=="--help" set "CHECK_ASSETS="
if "%ARG%"=="-h" set "CHECK_ASSETS="
if "%ARG%"=="--version" set "CHECK_ASSETS="
shift
goto scan
:scanned

if defined CHECK_ASSETS (
  if defined PY (
    "%PY%" %PY_ARGS% tools\import-assets.py check --output "%GAME_DATA%" >nul 2>nul
    if errorlevel 1 (
      rem First run or damaged files: the setup wizard imports your own
      rem copy of the game, then optionally its DLC.
      "%PY%" %PY_ARGS% tools\setup-wizard.py --output "%GAME_DATA%"
      if not errorlevel 1 "%PY%" %PY_ARGS% tools\import-assets.py check --output "%GAME_DATA%"
      if errorlevel 1 (
        echo The game files are not set up. Run tools\setup-wizard.py, or import your own ISO: 1>&2
        echo   python tools\import-assets.py import "C:\path\to\gow3.iso" 1>&2
        exit /b 1
      )
    )
  ) else (
    echo Python not found; skipping the asset check. 1>&2
  )
)
if not exist "%ROOT%\user" mkdir "%ROOT%\user"
if not exist "%ROOT%\logs" mkdir "%ROOT%\logs"

set "DEFAULTS="
if not defined HAS_GAME_DATA set DEFAULTS=%DEFAULTS% "--game_data_root=%GAME_DATA%"
if not defined HAS_USER_DATA set DEFAULTS=%DEFAULTS% "--user_data_root=%ROOT%\user"
if not defined HAS_LOG set DEFAULTS=%DEFAULTS% "--log_file=%ROOT%\logs\gow3.log"
if not defined HAS_CONFIG set DEFAULTS=%DEFAULTS% "--gow3_config=%ROOT%\config\gow3.toml"
"%EXE%" %DEFAULTS% %*
exit /b %errorlevel%
