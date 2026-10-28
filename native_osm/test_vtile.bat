@echo off
rem The vector tiles' tests (desktop x86): test_vtile.exe with vtile.cpp + image.cpp, run. Exit code: the failed
rem checks. "test_vtile.bat build": only the program (mutator.py's build). COVER=1: /Od /Zi (quality.bat).
setlocal
if "%VS8%"=="" set VS8=C:\Program Files (x86)\Microsoft Visual Studio 8
set VC=%VS8%\VC
set PATH=%VS8%\Common7\IDE;%VC%\bin;%PATH%
set CORE=%~dp0..\..\VestaCore
if not exist "%CORE%\vgps.h" set CORE=%~dp0..\core
cd /d "%~dp0"
if not exist obj\vtest mkdir obj\vtest
set OPT=/O2
if "%COVER%"=="1" set OPT=/Od /Zi
"%VC%\bin\cl.exe" /nologo %OPT% /W3 /MT /TP test_vtile.cpp vtile.cpp tilt.cpp junction.cpp ring.cpp image.cpp /Foobj\vtest\ /Fdobj\vtest\ ^
  /Feobj\vtest\test_vtile.exe /DUNICODE /D_UNICODE /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
  /I"%CORE%" /I"%VC%\include" /I"%VC%\PlatformSDK\Include" /link /DEBUG /LIBPATH:"%VC%\lib" ^
  /LIBPATH:"%VC%\PlatformSDK\Lib" kernel32.lib user32.lib gdi32.lib >obj\vtest\build.log
if errorlevel 1 type obj\vtest\build.log & exit /b 1
if "%1"=="build" exit /b 0
obj\vtest\test_vtile.exe
