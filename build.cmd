@echo off
:: =====================================================================
:: VGA4CPC-Enhanced — build single firmware UF2
:: Scanlines effect is a runtime toggle (BOOTSEL button), not a build
:: variant, so there's only one UF2 to ship.
:: Output: dist\vga4cpc_enhanced.uf2
:: =====================================================================

set SDK=C:\Program Files\Raspberry Pi\Pico SDK v1.5.1
set TOOLCHAIN=%SDK%\gcc-arm-none-eabi\bin
set NINJA=%SDK%\ninja
set CMAKE=%SDK%\cmake\bin
set PATH=%TOOLCHAIN%;%NINJA%;%CMAKE%;%PATH%

if not exist dist mkdir dist
if not exist build mkdir build

pushd build

if not exist CMakeCache.txt (
    cmake.exe -G "Ninja" ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%\ninja.exe" ^
        -DPICO_SDK_PATH="%SDK%\pico-sdk" ^
        -DPICO_TOOLCHAIN_PATH="%TOOLCHAIN%" ^
        ..
    if errorlevel 1 (popd & goto :fail)
)

ninja.exe
if errorlevel 1 (popd & goto :fail)

copy /Y vga4cpc_enhanced.uf2 ..\dist\ >nul
popd

echo.
echo ============================================================
echo  BUILD OK
echo ============================================================
dir dist\*.uf2
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
