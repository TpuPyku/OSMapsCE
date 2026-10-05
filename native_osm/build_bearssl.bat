@echo off
rem Builds BearSSL 0.6 (third_party\bearssl-0.6) as static libraries for Windows CE ARM and x86.
setlocal
if "%VS8%"=="" set VS8=C:\Program Files (x86)\Microsoft Visual Studio 8
set VC=%VS8%\VC
set SDK=%VS8%\SmartDevices\SDK\PocketPC2003
set PATH=%VS8%\Common7\IDE;%VC%\bin;%PATH%
set BR=%~dp0..\third_party\bearssl-0.6
set COMPAT=%~dp0..\third_party\compat
cd /d "%~dp0"
if not exist obj\br_arm mkdir obj\br_arm
if not exist obj\br_pc  mkdir obj\br_pc
dir /s /b "%BR%\src\*.c" > obj\br_files.txt

echo === BearSSL ARM
for /f "usebackq delims=" %%f in (obj\br_files.txt) do (
  "%VC%\ce\bin\x86_arm\cl.exe" /nologo /O2 /W1 /GS- /TC /c "%%f" /Foobj\br_arm\ ^
    /DUNICODE /D_UNICODE /DUNDER_CE=0x420 /D_WIN32_WCE=0x420 /DWINCE /DARM /D_ARM_ /DARMV4 ^
    /DBR_USE_WIN32_TIME=0 /DBR_USE_UNIX_TIME=0 /Dinline=__inline ^
    /I"%BR%\inc" /I"%BR%\src" /I"%COMPAT%" /I"%SDK%\Include" /I"%VC%\ce\include" >> obj\br_arm.log || goto fail
)
"%VC%\ce\bin\x86_arm\lib.exe" /nologo /OUT:obj\bearssl_arm.lib obj\br_arm\*.obj || goto fail

echo === BearSSL x86
for /f "usebackq delims=" %%f in (obj\br_files.txt) do (
  "%VC%\bin\cl.exe" /nologo /O2 /W1 /MT /TC /c "%%f" /Foobj\br_pc\ ^
    /DWIN32 /D_CRT_SECURE_NO_WARNINGS /DBR_USE_WIN32_TIME=0 /DBR_USE_UNIX_TIME=0 /Dinline=__inline /DBR_AES_X86NI=0 /DBR_SSE2=0 /DBR_RDRAND=0 /DBR_INT128=0 /DBR_UMUL128=0 ^
    /I"%BR%\inc" /I"%BR%\src" /I"%COMPAT%" /I"%COMPAT%\pc" /I"%VC%\include" /I"%VC%\PlatformSDK\Include" >> obj\br_pc.log || goto fail
)
"%VC%\bin\lib.exe" /nologo /OUT:obj\bearssl_pc.lib obj\br_pc\*.obj || goto fail
echo === OK
exit /b 0
:fail
echo === BEARSSL BUILD FAILED, see obj\br_*.log
exit /b 1
