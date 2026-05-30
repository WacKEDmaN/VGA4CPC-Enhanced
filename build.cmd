@echo off
:: =====================================================================
:: VGA4CPC-Enhanced — build the OVERCLOCKED firmware UF2
:: The source tree is the overclocked build (50 Hz 256 MHz / 60 Hz
:: 240 MHz). Output: dist\VGA4CPC-Enhanced-Overclocked.uf2
::
:: The stock (non-overclocked) dist\vga4cpc_enhanced.uf2 is a committed
:: prebuilt binary for Picos that don't like the overclock. To rebuild
:: THAT from source, check out the pre-overclock commit:
::     git checkout 32e841a -- src
:: (then this script outputs vga4cpc_enhanced.uf2 again — rename it back).
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

copy /Y vga4cpc_enhanced.uf2 "..\dist\VGA4CPC-Enhanced-Overclocked.uf2" >nul
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
