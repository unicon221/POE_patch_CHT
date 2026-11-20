@echo off
REM build.bat - Build the PoE language + font patcher
REM Requires MinGW-w64 (gcc + windres) in PATH, and Python for the icon.

setlocal
cd /d "%~dp0"

set CUI=..\cui_src\CUI-master
set GCC=x86_64-w64-mingw32-gcc.exe
set WR=windres.exe

if not exist "%CUI%\include\cui\cui.h" (
  echo [ERROR] CUI source not found: %CUI%
  echo Extract CUI-master into cui_src\
  exit /b 1
)

if not exist "app.ico" (
  echo Generating icon...
  python make_icon.py
)

set RES=
if exist "app.ico" (
  echo Compiling resources...
  %WR% -i app.rc -o app_res.o
  if not errorlevel 1 set RES=app_res.o
)

echo Compiling...
%GCC% -std=c11 -O2 -mwindows -static -static-libgcc -I"%CUI%\include" -I"%CUI%\src" -o poe_patch.exe poe_patch_gui.c poe_core.c poe_ggpk.c %RES% "%CUI%\src\*.c" "%CUI%\src\render\*.c" "%CUI%\src\platform\cui_app.c" "%CUI%\src\platform\cui_dialogs.c" "%CUI%\src\platform\cui_app_win32.c" -lgdi32 -lopengl32 -lgdiplus -lshell32 -lole32 -luser32 -lcomdlg32 -luuid -lcomctl32 -lshlwapi -ldwmapi -lwinmm -ladvapi32

if errorlevel 1 (
  echo [ERROR] Compilation failed
  exit /b 1
)

echo.
echo [OK] poe_patch.exe built
echo Note: oo2core.dll must sit next to the exe.
endlocal
