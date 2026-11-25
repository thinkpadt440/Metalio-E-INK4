@echo off
setlocal
pushd "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo Python 3 was not found. Install Python 3 for Windows first.
  pause
  popd
  exit /b 1
)
python --version
python mock_backend.py
if errorlevel 1 echo Mock backend exited with an error.
pause
popd
