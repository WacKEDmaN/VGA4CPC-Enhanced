@echo off
:: =====================================================================
:: VGA4CPC-Enhanced — build both variants (NORMAL and NORMAL_SCANLINES)
:: Run from the project directory.
:: Output: dist\vga4cpc_enhanced_NORMAL.uf2
::         dist\vga4cpc_enhanced_NORMAL_SCANLINES.uf2
:: =====================================================================

set SDK=C:\Program Files\Raspberry Pi\Pico SDK v1.5.1
set TOOLCHAIN=%SDK%\gcc-arm-none-eabi\bin
set NINJA=%SDK%\ninja
set CMAKE=%SDK%\cmake\bin
set PATH=%TOOLCHAIN%;%NINJA%;%CMAKE%;%PATH%

if not exist dist mkdir dist

call :build_variant NORMAL            OFF
if errorlevel 1 goto :fail
call :build_variant NORMAL_SCANLINES  ON
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  BUILD OK
echo ============================================================
dir dist\*.uf2
exit /b 0

:build_variant
set VARIANT=%~1
set SCANLINES=%~2
set BUILDDIR=build_%VARIANT%

echo.
echo --- Building %VARIANT% (SCANLINES=%SCANLINES%) ---
if not exist %BUILDDIR% mkdir %BUILDDIR%
pushd %BUILDDIR%

if not exist CMakeCache.txt (
    cmake.exe -G "Ninja" ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%\ninja.exe" ^
        -DPICO_SDK_PATH="%SDK%\pico-sdk" ^
        -DPICO_TOOLCHAIN_PATH="%TOOLCHAIN%" ^
        -DSCANLINES=%SCANLINES% ^
        ..
    if errorlevel 1 (popd & exit /b 1)
)

ninja.exe
if errorlevel 1 (popd & exit /b 1)

copy /Y vga4cpc_enhanced_%VARIANT%.uf2 ..\dist\ >nul
popd
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
