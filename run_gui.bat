@echo off
setlocal

cd /d "%~dp0"

:: Path to Dart SDK
set DART_EXE=C:\Flutter\bin\cache\dart-sdk\bin\dart.exe
if not exist "%DART_EXE%" (
    where dart >nul 2>nul
    if %ERRORLEVEL% equ 0 (
        set DART_EXE=dart
    ) else (
        echo [ERROR] Dart executable not found at %DART_EXE%
        pause
        exit /b 1
    )
)

:: Kill any stale bridge or miner instances
taskkill /F /IM miner.exe >nul 2>nul

:: Start Dart Bridge Daemon in background
start /B "" "%DART_EXE%" bridge.dart

:: Wait for server to bind port 8080
timeout /t 2 /nobreak >nul

:: Launch browser in dedicated standalone application window mode (no address bar, no tabs)
if exist "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" (
    start "" "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" --app=http://localhost:8080 --window-size=1280,840
) else if exist "C:\Program Files\Google\Chrome\Application\chrome.exe" (
    start "" "C:\Program Files\Google\Chrome\Application\chrome.exe" --app=http://localhost:8080 --window-size=1280,840
) else (
    start http://localhost:8080
)

exit /b 0
