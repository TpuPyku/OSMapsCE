@echo off
rem The PC app (maps_app.py) for users without Python: app\OSMapsCE-Maps.zip (the folder OSMapsCE-Maps with the exe).
rem A folder, not one file: a one-file exe unpacks itself to %TEMP% at each start - what Windows Defender takes for
rem malware (the user 2026-10-28: blocked); no UPX for the same reason. The icon: make_icon.py.
rem Needs: pip install pyinstaller pmtiles osmium pillow
setlocal
cd /d "%~dp0"
python make_icon.py >nul || exit /b 1
python -m PyInstaller --noconfirm --onedir --windowed --noupx --icon "%~dp0app.ico" --name OSMapsCE-Maps ^
  --distpath app --workpath work\pyinstaller --specpath work\pyinstaller --hidden-import tkinter.filedialog ^
  maps_app.py || exit /b 1
python -c "import shutil; shutil.make_archive('app/OSMapsCE-Maps', 'zip', 'app', 'OSMapsCE-Maps')" || exit /b 1
