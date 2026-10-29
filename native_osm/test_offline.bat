@echo off
rem The offline router and search tests (desktop x86): builds the test map (..\offline\test\tiny.osm) with the
rem region tools into obj\test\OSMapsData\tiny, builds test_offline.exe with offline.cpp + offsearch.cpp +
rem offdata.cpp, runs it.
rem Exit code: the failed checks.  "test_offline.bat build": only the program (the map kept) - mutator.py's build.
rem COVER=1: the build for coverage (/Od /Zi) - quality.bat.
setlocal
if "%VS8%"=="" set VS8=C:\Program Files (x86)\Microsoft Visual Studio 8
set VC=%VS8%\VC
set PATH=%VS8%\Common7\IDE;%VC%\bin;%PATH%
set CORE=%~dp0..\..\VestaCore
if not exist "%CORE%\vgps.h" set CORE=%~dp0..\core
cd /d "%~dp0"
set MAP=obj\test\OSMapsData\tiny
if not exist %MAP% mkdir %MAP%
if not "%1"=="build" (
  python ..\offline\make_graph.py ..\offline\test\tiny.osm %MAP%\route.grf >nul || exit /b 1
  python ..\offline\make_search.py ..\offline\test\tiny.osm %MAP%\search.idx >nul || exit /b 1
  rem the trunk roads (4b): tiny's primary and tertiary streets (the residential ones only the region's)
  if not exist obj\test\OSMapsData\trunk mkdir obj\test\OSMapsData\trunk
  python ..\offline\test\make_test_trunk.py ..\offline\test\tiny.osm obj\test\OSMapsData\trunk\trunk.grf >nul || exit /b 1
  if not exist obj\test\OSMapsData\old mkdir obj\test\OSMapsData\old
  python ..\offline\test\make_test_tiles.py %MAP%\tiles.vtp obj\test\OSMapsData\old\tiles.vtp >nul || exit /b 1
  rem the second region (only its search): the town Другоград 5 degrees north
  if not exist obj\test\OSMapsData\twin mkdir obj\test\OSMapsData\twin
  python ..\offline\test\make_twin.py ..\offline\test\tiny.osm obj\test\twin.osm || exit /b 1
  python ..\offline\make_search.py obj\test\twin.osm obj\test\OSMapsData\twin\search.idx >nul || exit /b 1
)
set OPT=/O2
if "%COVER%"=="1" set OPT=/Od /Zi
"%VC%\bin\cl.exe" /nologo %OPT% /W3 /MT /TP test_offline.cpp offline.cpp offsearch.cpp offdata.cpp vpack.cpp /Foobj\test\ /Fdobj\test\ ^
  /Feobj\test\test_offline.exe /DUNICODE /D_UNICODE /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
  /I"%CORE%" /I"%VC%\include" /I"%VC%\PlatformSDK\Include" /link /DEBUG /LIBPATH:"%VC%\lib" ^
  /LIBPATH:"%VC%\PlatformSDK\Lib" kernel32.lib >obj\test\build.log
if errorlevel 1 type obj\test\build.log & exit /b 1
if "%1"=="build" exit /b 0
obj\test\test_offline.exe
