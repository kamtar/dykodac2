@echo off
setlocal EnableExtensions
pushd "%~dp0"

rem Flashes the last Release build (build.bat output) over USB.
rem Extra arguments are passed through, e.g. flash.bat --flashloader C:\path\ivt_flashloader.bin

set "DYKO_IMAGE=build\gmake2\bin\Release\dykodac2.bin"

if not exist "%DYKO_IMAGE%" (
    echo ERROR: %DYKO_IMAGE% not found. Run build.bat first.
    goto :failed
)

where python >nul 2>nul
if errorlevel 1 (
    echo ERROR: python was not found on PATH.
    goto :failed
)

rem Private virtual environment so SPSDK never touches the global Python.
set "DYKO_PY=tools\.venv\Scripts\python.exe"
if not exist "%DYKO_PY%" (
    echo Creating tools\.venv for flashing tools...
    python -m venv tools\.venv
    if errorlevel 1 goto :failed
)
"%DYKO_PY%" -c "import spsdk, hid" >nul 2>nul
if errorlevel 1 (
    echo Installing flashing dependencies ^(spsdk, hidapi^) into tools\.venv...
    "%DYKO_PY%" -m pip install --quiet --disable-pip-version-check spsdk hidapi
    if errorlevel 1 goto :failed
)

for %%F in ("%DYKO_IMAGE%") do echo Flashing %%~fF ^(%%~zF bytes, built %%~tF^)
"%DYKO_PY%" tools\flash_firmware.py "%DYKO_IMAGE%" %*
if errorlevel 1 goto :failed

popd
exit /b 0

:failed
echo.
echo FLASH FAILED.
popd
exit /b 1
