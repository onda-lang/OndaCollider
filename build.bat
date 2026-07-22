@echo off
setlocal EnableDelayedExpansion

if "%~1"=="" (
    echo [ERROR] SuperCollider path not provided.
    echo Usage: %~nx0 "C:\path\to\supercollider" [Optional: "C:\path\to\onda-sdk" OR "C:\path\to\install\extensions"] [Optional: "C:\path\to\install\extensions"]
    exit /b 1
)

set "SC_PATH=%~1"
set "ARG2=%~2"
set "ARG3=%~3"
set "ONDA_SDK_PATH="
set "INSTALL_DEST="
set "STAGING_DIR=%CD%\build\Install"
set "INSTALL_ROOT=%STAGING_DIR%"
set "FINAL_INSTALL_DIR=%STAGING_DIR%\Onda"

if not "%ARG3%"=="" (
    rem 3-arg form: arg2 = Onda SDK path, arg3 = install destination
    set "ONDA_SDK_PATH=%ARG2%"
    set "INSTALL_DEST=%ARG3%"
) else (
    if not "%ARG2%"=="" (
        rem 2-arg form: autodetect whether arg2 is Onda SDK path or install destination
        if exist "%ARG2%\include\onda.h" (
            set "ONDA_SDK_PATH=%ARG2%"
        ) else (
            set "INSTALL_DEST=%ARG2%"
        )
    )
)

if "%ONDA_SDK_PATH%"=="" (
    set "ONDA_SDK_PATH=%CD%\build\onda-sdk"
    echo [INFO] Onda SDK path not provided. Downloading the configured Onda release...
    powershell -ExecutionPolicy Bypass -File "%~dp0scripts\fetch-onda.ps1" -Destination "!ONDA_SDK_PATH!"
    if errorlevel 1 (
        echo [ERROR] Failed to download Onda SDK.
        exit /b %errorlevel%
    )
)

if not "%INSTALL_DEST%"=="" (
    for %%I in ("%INSTALL_DEST%") do set "INSTALL_BASENAME=%%~nxI"
    for %%I in ("%INSTALL_DEST%") do set "INSTALL_PARENT=%%~dpI"

    if /I "!INSTALL_BASENAME!"=="Onda" (
        rem Caller passed the extension folder itself (e.g. ...\Extensions\Onda)
        set "INSTALL_ROOT=!INSTALL_PARENT:~0,-1!"
        set "FINAL_INSTALL_DIR=%INSTALL_DEST%"
    ) else (
        rem Caller passed the Extensions root (e.g. ...\Extensions)
        set "INSTALL_ROOT=%INSTALL_DEST%"
        set "FINAL_INSTALL_DIR=%INSTALL_DEST%\Onda"
    )
)

echo [INFO] SC_PATH: "%SC_PATH%"
echo [INFO] ONDA_SDK_PATH: "%ONDA_SDK_PATH%"
echo [INFO] INSTALL_ROOT: "%INSTALL_ROOT%"
echo [INFO] FINAL_INSTALL_DIR: "%FINAL_INSTALL_DIR%"

echo [INFO] Configuring CMake...
cmake -B build ^
    -DSC_PATH="%SC_PATH%" ^
    -DONDA_SDK_PATH="%ONDA_SDK_PATH%" ^
    -DCMAKE_INSTALL_PREFIX="%INSTALL_ROOT%"

if %errorlevel% neq 0 (
    echo [ERROR] CMake configuration failed.
    exit /b %errorlevel%
)

echo [INFO] Building Release configuration...
cmake --build build --config Release

if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    exit /b %errorlevel%
)

echo [INFO] Packaging (installing to target prefix)...
cmake --install build --config Release

if %errorlevel% neq 0 (
    echo [ERROR] Packaging failed.
    exit /b %errorlevel%
)

if not exist "%FINAL_INSTALL_DIR%\Classes\Onda.sc" (
    echo [ERROR] Install verification failed. Missing "%FINAL_INSTALL_DIR%\Classes\Onda.sc"
    exit /b 1
)

if not exist "%FINAL_INSTALL_DIR%\Onda_scsynth.scx" (
    echo [WARN] "%FINAL_INSTALL_DIR%\Onda_scsynth.scx" not found. Check SCSYNTH/SUPERNOVA options.
)

if not exist "%FINAL_INSTALL_DIR%\Onda_supernova.scx" (
    echo [WARN] "%FINAL_INSTALL_DIR%\Onda_supernova.scx" not found. Check SCSYNTH/SUPERNOVA options.
)

if not "%INSTALL_DEST%"=="" (
    echo [SUCCESS] Files installed to "%FINAL_INSTALL_DIR%"
) else (
    echo [SUCCESS] Build complete. Package available at "%FINAL_INSTALL_DIR%"
)
