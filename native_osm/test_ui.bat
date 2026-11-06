@echo off
rem Builds and runs test_ui.exe (the route's start, the languages; VS2005 x86) in obj\, then lang_check.py: every Russian
rem string of the sources in OSMapsCE\Lang\en.txt, formats the same. Exit code: the failed checks.
rem "build": only the program (the mutator runs it).
setlocal
if "%VS8%"=="" set VS8=C:\Program Files (x86)\Microsoft Visual Studio 8
set VC=%VS8%\VC
set PATH=%VS8%\Common7\IDE;%VC%\bin;%PATH%
cd /d "%~dp0"
if not exist obj mkdir obj
"%VC%\bin\cl.exe" /nologo /O2 /W3 /MT /TP test_ui.cpp lang.cpp /Foobj\ /Fdobj\ /Feobj\test_ui.exe ^
  /DUNICODE /D_UNICODE /DWIN32 /D_CRT_SECURE_NO_WARNINGS /I"%VC%\include" /I"%VC%\PlatformSDK\Include" ^
  /link /LIBPATH:"%VC%\lib" /LIBPATH:"%VC%\PlatformSDK\Lib" kernel32.lib >obj\build_ui.log || (type obj\build_ui.log & exit /b 1)
if "%1"=="build" exit /b 0
obj\test_ui.exe || exit /b 1
python lang_check.py
