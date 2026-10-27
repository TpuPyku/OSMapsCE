@echo off
rem The ub-mode checks (SKILL.md): the tests with coverage (OpenCppCoverage for C++, coverage.py for the region
rem tools), crapper (CRAP < 10 per function), dryer (no duplicates ranked 0.8+); "mutate": mutator too (slow).
rem   quality.bat offline [mutate]   offline.cpp, offsearch.cpp (+ offdata.cpp) and the region tools
rem   quality.bat vtile [mutate]     vtile.cpp
rem   quality.bat                    both
rem The tools: VestaHome\tools (the sibling repo). Needs: pip install lizard coverage;
rem OpenCppCoverage (winget install OpenCppCoverage.OpenCppCoverage). Exit code: the findings.
setlocal
cd /d "%~dp0"
set TOOLS=..\..\VestaHome\tools
set OCC=%ProgramFiles%\OpenCppCoverage\OpenCppCoverage.exe
set BAD=0
if "%1"=="vtile" goto vtile

:offline
set CPP=offline.cpp offsearch.cpp offdata.cpp vpack.cpp
set PY=..\offline\make_graph.py ..\offline\make_search.py ..\offline\make_region.py ..\offline\make_tiles.py
set COVER=1
call .\test_offline.bat >obj\test_offline.log 2>&1 || (type obj\test_offline.log & echo === the offline tests fail & exit /b 1)
set COVER=
"%OCC%" --quiet --sources "%CD%" --excluded_sources "%CD%\test_offline.cpp" ^
  --export_type cobertura:obj\test\cov.xml -- obj\test\test_offline.exe >nul
python -m coverage run --data-file=obj\test\pydata -m unittest discover -s ..\offline\test >obj\test\pytest.log 2>&1 || (type obj\test\pytest.log & echo === the region tools' tests fail & exit /b 1)
python -m coverage json --data-file=obj\test\pydata -o obj\test\pyreport.json >nul
echo === crapper: offline
python %TOOLS%\crapper.py --cobertura obj\test\cov.xml --pycov obj\test\pyreport.json %CPP% %PY%
set /a BAD=%BAD%+%errorlevel%
echo === dryer: offline
python %TOOLS%\dryer.py %CPP% %PY%
set /a BAD=%BAD%+%errorlevel%
if "%2"=="mutate" (
  echo === mutator: offline
  python %TOOLS%\mutator.py --build ".\test_offline.bat build" --test "obj\test\test_offline.exe" ^
    --cobertura obj\test\cov.xml --max 120 %CPP%
  set /a BAD=%BAD%+%errorlevel%
)
call .\test_offline.bat build >nul
if "%1"=="offline" exit /b %BAD%

:vtile
set COVER=1
call .\test_vtile.bat >obj\test_vtile.log 2>&1 || (type obj\test_vtile.log & echo === the vector tile tests fail & exit /b 1)
set COVER=
"%OCC%" --quiet --sources "%CD%\vtile.cpp" --export_type cobertura:obj\vtest\cov.xml -- obj\vtest\test_vtile.exe >nul
echo === crapper: vtile
python %TOOLS%\crapper.py --cobertura obj\vtest\cov.xml vtile.cpp
set /a BAD=%BAD%+%errorlevel%
echo === dryer: vtile
python %TOOLS%\dryer.py vtile.cpp
set /a BAD=%BAD%+%errorlevel%
if "%2"=="mutate" (
  echo === mutator: vtile
  python %TOOLS%\mutator.py --build ".\test_vtile.bat build" --test "obj\vtest\test_vtile.exe" ^
    --cobertura obj\vtest\cov.xml --max 120 vtile.cpp
  set /a BAD=%BAD%+%errorlevel%
)
call .\test_vtile.bat build >nul
exit /b %BAD%
