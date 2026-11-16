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
rem (goto: "exit /b 1" in a block with more lines after it reaches "cmd /c test_offline.bat" as 0)
if not "%1"=="build" (
  python ..\offline\make_graph.py ..\offline\test\tiny.osm %MAP%\route.grf >nul || goto :bad
  python ..\offline\make_search.py ..\offline\test\tiny.osm %MAP%\search.idx >nul || goto :bad
  python ..\offline\make_layers.py ..\offline\test\layers.osm %MAP%\layers.lay >nul || goto :bad
  python ..\offline\make_parklines.py %MAP%\parking.lay ..\offline\test\parklines.json >nul || goto :bad
  rem the trunk roads (4b): tiny's primary and tertiary streets (the residential ones only the region's)
  if not exist obj\test\OSMapsData\trunk mkdir obj\test\OSMapsData\trunk
  python ..\offline\test\make_test_trunk.py ..\offline\test\tiny.osm obj\test\OSMapsData\trunk\trunk.grf >nul || goto :bad
  rem a region's zip as the release has it (offmaps.cpp unpacks it), tiny's region.txt
  python ..\offline\test\make_test_zip.py obj\test\region.zip %MAP% || goto :bad
  if not exist obj\test\OSMapsData\old mkdir obj\test\OSMapsData\old
  if not exist obj\test\OSMapsData\overview mkdir obj\test\OSMapsData\overview
  python ..\offline\test\make_test_tiles.py %MAP%\tiles.vtp obj\test\OSMapsData\old\tiles.vtp obj\test\OSMapsData\overview\overview.vtp >nul || goto :bad
  rem the second region (its search and graph): the town Другоград 5 degrees north
  if not exist obj\test\OSMapsData\twin mkdir obj\test\OSMapsData\twin
  python ..\offline\test\make_twin.py ..\offline\test\tiny.osm obj\test\twin.osm || goto :bad
  python ..\offline\make_search.py obj\test\twin.osm obj\test\OSMapsData\twin\search.idx >nul || goto :bad
  python ..\offline\make_graph.py obj\test\twin.osm obj\test\OSMapsData\twin\route.grf >nul || goto :bad
  rem (its graph as made before the speed limits: an old region on the card)
  python ..\offline\test\strip_limits.py obj\test\OSMapsData\twin\route.grf || goto :bad
  rem the toll region: a toll road and a free way round it, 60 degrees north
  if not exist obj\test\OSMapsData\toll mkdir obj\test\OSMapsData\toll
  python ..\offline\test\make_test_toll.py obj\test\toll.osm || goto :bad
  python ..\offline\make_graph.py obj\test\toll.osm obj\test\OSMapsData\toll\route.grf >nul || goto :bad
  rem the hook region: a fork, a loop with a hairpin beside a straight road, 64 degrees north
  if not exist obj\test\OSMapsData\hook mkdir obj\test\OSMapsData\hook
  python ..\offline\test\make_test_hook.py obj\test\hook.osm || goto :bad
  python ..\offline\make_graph.py obj\test\hook.osm obj\test\OSMapsData\hook\route.grf >nul || goto :bad
  rem the same at 66 N as a graph made before the bearings (cut at its 'OSMB'): the turns by the nodes
  if not exist obj\test\OSMapsData\hookold mkdir obj\test\OSMapsData\hookold
  python ..\offline\test\make_test_hook.py obj\test\hookold.osm 66 || goto :bad
  python ..\offline\make_graph.py obj\test\hookold.osm obj\test\OSMapsData\hookold\route.grf >nul || goto :bad
  python -c "import sys; p = sys.argv[1]; d = open(p, 'rb').read(); open(p, 'wb').write(d[:d.rindex(b'OSMB')])" obj\test\OSMapsData\hookold\route.grf || goto :bad
)
set OPT=/O2
if "%COVER%"=="1" set OPT=/Od /Zi
"%VC%\bin\cl.exe" /nologo %OPT% /W3 /MT /TP test_offline.cpp fling.cpp zoomanim.cpp trafshade.cpp layers.cpp offline.cpp offsearch.cpp offdata.cpp vpack.cpp offmaps.cpp lang.cpp "%CORE%\vjson.cpp" image.cpp /Foobj\test\ /Fdobj\test\ ^
  /Feobj\test\test_offline.exe /DUNICODE /D_UNICODE /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
  /I"%CORE%" /I"%VC%\include" /I"%VC%\PlatformSDK\Include" /link /DEBUG /LIBPATH:"%VC%\lib" ^
  /LIBPATH:"%VC%\PlatformSDK\Lib" kernel32.lib user32.lib gdi32.lib >obj\test\build.log
if errorlevel 1 type obj\test\build.log & exit /b 1
if "%1"=="build" exit /b 0
obj\test\test_offline.exe
exit /b %errorlevel%
:bad
exit /b 1
