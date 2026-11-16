@echo off
rem The 3D frame's speed (bench_tilt.cpp): obj\bench\bench_tilt.exe (ARM: the device or the Device Emulator - run it
rem there, it writes bench_tilt.txt next to itself) and obj\bench\bench_tiltPC.exe (run here).
setlocal
if "%VS8%"=="" set VS8=C:\Program Files (x86)\Microsoft Visual Studio 8
set VC=%VS8%\VC
set SDK=%VS8%\SmartDevices\SDK\PocketPC2003
set PATH=%VS8%\Common7\IDE;%VC%\bin;%PATH%
set CORE=%~dp0..\..\VestaCore
if not exist "%CORE%\vgps.h" set CORE=%~dp0..\core
cd /d "%~dp0"
if not exist obj\bench\arm mkdir obj\bench\arm
if not exist obj\bench\pc mkdir obj\bench\pc
set SRC=bench_tilt.cpp ring.cpp tilt.cpp image.cpp
"%VC%\ce\bin\x86_arm\cl.exe" /nologo /O2 /W3 /GS- /TP /c %SRC% /Foobj\bench\arm\ ^
  /DUNICODE /D_UNICODE /DUNDER_CE=0x420 /D_WIN32_WCE=0x420 /DWINCE /DARM /D_ARM_ /DARMV4 /DNDEBUG ^
  /I"%SDK%\Include" /I"%VC%\ce\include" /I"%CORE%" >obj\bench\build.log
if errorlevel 1 type obj\bench\build.log & exit /b 1
"%VC%\ce\bin\x86_arm\link.exe" /nologo /SUBSYSTEM:WINDOWSCE,4.20 /MACHINE:ARM /OUT:obj\bench\bench_tilt.exe ^
  obj\bench\arm\*.obj /LIBPATH:"%SDK%\Lib\armv4" /LIBPATH:"%VC%\ce\lib\armv4" coredll.lib corelibc.lib ^
  /NODEFAULTLIB:oldnames.lib >>obj\bench\build.log
if errorlevel 1 type obj\bench\build.log & exit /b 1
"%VC%\bin\cl.exe" /nologo /O2 /W3 /MT /TP %SRC% /Foobj\bench\pc\ /Feobj\bench\bench_tiltPC.exe ^
  /DUNICODE /D_UNICODE /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I"%CORE%" /I"%VC%\include" ^
  /I"%VC%\PlatformSDK\Include" /link /LIBPATH:"%VC%\lib" /LIBPATH:"%VC%\PlatformSDK\Lib" kernel32.lib user32.lib gdi32.lib ^
  >>obj\bench\build.log
if errorlevel 1 type obj\bench\build.log & exit /b 1
echo === OK
