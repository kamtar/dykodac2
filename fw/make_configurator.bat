@echo off
setlocal EnableExtensions
pushd "%~dp0"

rem Builds tools\dist\DykoDAC2-Configurator.exe (single file, no console).
rem Uses the private tools\.venv; installs PySide6/PyInstaller/SPSDK/hidapi there on first use.

set "DYKO_PY=tools\.venv\Scripts\python.exe"
if not exist "%DYKO_PY%" (
    where python >nul 2>nul || (echo ERROR: python was not found on PATH. & goto :failed)
    python -m venv tools\.venv || goto :failed
)
"%DYKO_PY%" -c "import PySide6, PyInstaller, spsdk, hid" >nul 2>nul
if errorlevel 1 (
    echo Installing configurator build dependencies into tools\.venv...
    "%DYKO_PY%" -m pip install --quiet --disable-pip-version-check PySide6-Essentials pyinstaller spsdk hidapi || goto :failed
)

rem NXP's RAM flashloader is needed for "Save to flash". It is bundled when
rem present; otherwise the app asks for it (or looks next to the .exe).
set "DYKO_LOADER="
if exist "tools\flashloader\ivt_flashloader.bin" set "DYKO_LOADER=--add-data tools\flashloader\ivt_flashloader.bin;flashloader"

pushd tools
"..\%DYKO_PY%" -m PyInstaller --noconfirm --clean --onefile --windowed --name DykoDAC2-Configurator ^
    --collect-all spsdk --collect-submodules hid --exclude-module tkinter --exclude-module matplotlib ^
    %DYKO_LOADER:tools\=..\tools\% ^
    dykodac_configurator.py
if errorlevel 1 (popd & goto :failed)
popd

echo.
echo Built tools\dist\DykoDAC2-Configurator.exe
popd
exit /b 0

:failed
echo.
echo CONFIGURATOR BUILD FAILED.
popd
exit /b 1
