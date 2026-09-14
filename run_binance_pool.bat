@echo off
title Bitcoin CPU Miner - Binance Pool (Shared Mining)

cd /d "%~dp0"

echo ===================================================
echo          Bitcoin CPU Miner (Binance Pool)
echo ===================================================
echo Pool: sha256.poolbinance.com:8888
echo Worker: 330133013301.001
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
echo Pool: sha256.poolbinance.com:8888
echo Account: 330133013301.001
echo ===================================================
echo.

miner.exe --pool sha256.poolbinance.com --port 8888 --user 330133013301.001 --password 123456 --threads %THREADS%

echo.
echo ===================================================
echo Miner stopped.
echo ===================================================
pause
