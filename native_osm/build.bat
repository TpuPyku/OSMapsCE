@echo off
rem Builds OsmMapsCE.exe (Windows CE, ARMv4) and OsmMapsPC.exe (desktop x86, for testing)
rem with Visual Studio 2005 (Smart Device support + Pocket PC 2003 SDK).
rem BearSSL libraries must be built first: build_bearssl.bat
setlocal

if "%VS8%"=="" set VS8=C:\Program Files (x86)\Microsoft Visual Studio 8
set VC=%VS8%\VC
set SDK=%VS8%\SmartDevices\SDK\PocketPC2003
set PATH=%VS8%\Common7\IDE;%VC%\bin;%PATH%
set SRC=main.cpp net.cpp http.cpp gps.cpp cache.cpp image.cpp util.cpp
set OUT=..\OsmMapsCE
set BR=%~dp0..\third_party\bearssl-0.6
set COMPAT=%~dp0..\third_party\compat

cd /d "%~dp0"
if not exist obj\bearssl_arm.lib goto nobearssl
if not exist obj\arm mkdir obj\arm
if not exist obj\pc  mkdir obj\pc
if not exist "%OUT%" mkdir "%OUT%"

echo === ARM (Windows CE)
"%VC%\ce\bin\x86_arm\cl.exe" /nologo /O2 /W3 /GS- /TP /c %SRC% /Foobj\arm\ ^
  /DUNICODE /D_UNICODE /DUNDER_CE=0x420 /D_WIN32_WCE=0x420 /DWINCE /DARM /D_ARM_ /DARMV4 /DNDEBUG ^
  /I"%BR%\inc" /I"%COMPAT%" /I"%SDK%\Include" /I"%VC%\ce\include"
if errorlevel 1 goto fail
"%VC%\ce\bin\x86_arm\link.exe" /nologo /SUBSYSTEM:WINDOWSCE,4.20 /MACHINE:ARM /OUT:"%OUT%\OsmMapsCE.exe" ^
  obj\arm\*.obj obj\bearssl_arm.lib /LIBPATH:"%SDK%\Lib\armv4" /LIBPATH:"%VC%\ce\lib\armv4" ^
  coredll.lib corelibc.lib ws2.lib /NODEFAULTLIB:oldnames.lib /NODEFAULTLIB:advapi32.lib
if errorlevel 1 goto fail

echo === x86 (desktop test build)
"%VC%\bin\cl.exe" /nologo /O2 /W3 /MT /TP /c %SRC% /Foobj\pc\ ^
  /DUNICODE /D_UNICODE /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
  /I"%BR%\inc" /I"%COMPAT%" /I"%VC%\include" /I"%VC%\PlatformSDK\Include"
if errorlevel 1 goto fail
"%VC%\bin\link.exe" /nologo /SUBSYSTEM:WINDOWS /OUT:"%OUT%\OsmMapsPC.exe" obj\pc\*.obj obj\bearssl_pc.lib ^
  /LIBPATH:"%VC%\lib" /LIBPATH:"%VC%\PlatformSDK\Lib" user32.lib gdi32.lib kernel32.lib ws2_32.lib advapi32.lib
if errorlevel 1 goto fail

echo === OK
exit /b 0
:nobearssl
echo === run build_bearssl.bat first
exit /b 1
:fail
echo === BUILD FAILED
exit /b 1
