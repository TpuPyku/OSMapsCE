@echo off
rem Builds and runs test_voice.exe (the voice prompts; VS2005 x86) in obj\. Exit code: the failed checks.
rem "build": only the program (the mutator runs it).
setlocal
if "%VS8%"=="" set VS8=C:\Program Files (x86)\Microsoft Visual Studio 8
set VC=%VS8%\VC
set PATH=%VS8%\Common7\IDE;%VC%\bin;%PATH%
set CORE=%~dp0..\..\VestaCore
if not exist "%CORE%\vgps.h" set CORE=%~dp0..\core
cd /d "%~dp0"
if not exist obj\voice mkdir obj\voice
set OPT=/O2
if "%COVER%"=="1" set OPT=/Od /Zi
"%VC%\bin\cl.exe" /nologo %OPT% /W3 /MT /TP test_voice.cpp voice.cpp /Foobj\voice\ /Fdobj\voice\ /Feobj\voice\test_voice.exe ^
  /DUNICODE /D_UNICODE /DWIN32 /D_CRT_SECURE_NO_WARNINGS /I"%CORE%" /I"%VC%\include" /I"%VC%\PlatformSDK\Include" ^
  /link /DEBUG /LIBPATH:"%VC%\lib" /LIBPATH:"%VC%\PlatformSDK\Lib" >obj\voice\build.log || (type obj\voice\build.log & exit /b 1)
if "%1"=="build" exit /b 0
obj\voice\test_voice.exe
