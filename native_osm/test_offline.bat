@echo off
rem Builds and runs test_offline.exe (desktop x86): the offline router (offline.cpp) on ..\OSMapsData's region.
rem Arguments go to it: "lon,lat;lon,lat" [course] [/v]
setlocal
if "%VS8%"=="" set VS8=C:\Program Files (x86)\Microsoft Visual Studio 8
set VC=%VS8%\VC
set PATH=%VS8%\Common7\IDE;%VC%\bin;%PATH%
set CORE=%~dp0..\..\VestaCore
if not exist "%CORE%\vgps.h" set CORE=%~dp0..\core
cd /d "%~dp0"
if not exist obj\test mkdir obj\test
"%VC%\bin\cl.exe" /nologo /O2 /W3 /MT /TP test_offline.cpp offline.cpp /Foobj\test\ /Feobj\test\test_offline.exe ^
  /DUNICODE /D_UNICODE /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
  /I"%CORE%" /I"%VC%\include" /I"%VC%\PlatformSDK\Include" /link /LIBPATH:"%VC%\lib" /LIBPATH:"%VC%\PlatformSDK\Lib" kernel32.lib
if errorlevel 1 exit /b 1
obj\test\test_offline.exe %*
