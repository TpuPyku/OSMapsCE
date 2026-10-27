@echo off
rem The ub-mode checks (SKILL.md) of the offline code: the tests with coverage (OpenCppCoverage for C++, coverage.py
rem for the region tools), then crapper (CRAP < 10 per function) and dryer (no duplicates ranked 0.8+);
rem "quality.bat mutate": mutator too (slow: a build per mutant). The tools: VestaHome\tools (the sibling repo).
rem Needs: pip install lizard coverage; OpenCppCoverage (winget install OpenCppCoverage.OpenCppCoverage).
setlocal
cd /d "%~dp0"
set TOOLS=..\..\VestaHome\tools
set OCC=%ProgramFiles%\OpenCppCoverage\OpenCppCoverage.exe
set CPP=offline.cpp offsearch.cpp
set PY=..\offline\make_graph.py ..\offline\make_search.py ..\offline\make_region.py
set COVER=1
call .\test_offline.bat >nul || (echo === the tests fail & exit /b 1)
set COVER=
"%OCC%" --quiet --sources "%CD%" --excluded_sources "%CD%\test_offline.cpp" ^
  --export_type cobertura:obj\test\cov.xml -- obj\test\test_offline.exe >nul
set MAP=obj\test\cov_map
if not exist %MAP% mkdir %MAP%
python -m coverage run --data-file=obj\test\pydata ..\offline\make_graph.py ..\offline\test\tiny.osm %MAP%\route.grf >nul
python -m coverage run -a --data-file=obj\test\pydata ..\offline\make_search.py ..\offline\test\tiny.osm %MAP%\search.idx >nul
python -m coverage json --data-file=obj\test\pydata -o obj\test\pyreport.json >nul
echo === crapper
python %TOOLS%\crapper.py --cobertura obj\test\cov.xml --pycov obj\test\pyreport.json %CPP% %PY%
set BAD=%errorlevel%
echo === dryer
python %TOOLS%\dryer.py %CPP% %PY%
set /a BAD=%BAD%+%errorlevel%
if "%1"=="mutate" (
  echo === mutator
  python %TOOLS%\mutator.py --build ".\test_offline.bat build" --test "obj\test\test_offline.exe" ^
    --cobertura obj\test\cov.xml --max 120 %CPP%
  set /a BAD=%BAD%+%errorlevel%
)
call .\test_offline.bat build >nul
exit /b %BAD%
