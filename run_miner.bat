@echo off
title Bitcoin CPU Miner - solo.ckpool.org

cd /d "%~dp0"

echo ===================================================
echo          Bitcoin CPU Miner
echo ===================================================
echo Pool: solo.ckpool.org:3333
echo Address: 1EUeWGhrKsrSmicJoSgYVgbbNAT4hFBzqS
echo.
echo Your CPU supports up to 8 logical threads.
echo.

:ASK_THREADS
set /p THREADS=Enter number of threads to use (1-8): 

if not defined THREADS goto ASK_THREADS

if %THREADS% LSS 1 goto INVALID
if %THREADS% GTR 8 goto INVALID

goto START

:INVALID
echo.
echo Invalid number. Please enter a value between 1 and 8.
echo.
goto ASK_THREADS

:START
echo.
echo ===================================================
echo Starting miner with %THREADS% threads
echo Address: 1EUeWGhrKsrSmicJoSgYVgbbNAT4hFBzqS
echo ===================================================
echo.

miner.exe --pool solo.ckpool.org --port 3333 --user 1EUeWGhrKsrSmicJoSgYVgbbNAT4hFBzqS --threads %THREADS%

echo.
echo ===================================================
echo Miner stopped.
echo ===================================================
pause
