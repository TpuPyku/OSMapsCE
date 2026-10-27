@echo off
rem The PC app (maps_app.py) as one exe for users without Python: app\OSMapsCE-Maps.exe.
rem Needs: pip install pyinstaller pmtiles
setlocal
cd /d "%~dp0"
python -m PyInstaller --onefile --windowed --name OSMapsCE-Maps --distpath app --workpath work\pyinstaller ^
  --specpath work\pyinstaller --hidden-import tkinter.filedialog maps_app.py || exit /b 1
