@echo off
setlocal
cd /d "%~dp0"
py -3 -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1
if not errorlevel 1 (
    py -3 server.py --open %*
    goto finished
)
python -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1
if not errorlevel 1 (
    python server.py --open %*
    goto finished
)
echo Python 3.10 or newer is required.
echo Install Python for Windows with the Python launcher or Add Python to PATH enabled.
echo Then run start.bat again. No other packages are required.
exit /b 1
:finished
set "result=%errorlevel%"
if not "%result%"=="0" echo RIOT Introduction stopped with an error. See the message above.
exit /b %result%
