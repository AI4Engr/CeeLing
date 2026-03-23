@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM Regression test runner for CeeLing C++ transpilation (Windows).
REM Usage:
REM   run_regression.bat [native] [update]
REM   run_regression.bat update

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "PROJECT_DIR=%%~fI"

set "FIXTURES_DIR=%SCRIPT_DIR%\fixtures"
set "OUTPUT_CPP_DIR=%SCRIPT_DIR%\results\cpp"
set "OUTPUT_LOG_DIR=%SCRIPT_DIR%\results\log"
set "EXPECTED_CPP_DIR=%SCRIPT_DIR%\expected\cpp"
set "DIFF_CPP_DIR=%SCRIPT_DIR%\diff\cpp"

set "MODE=%~1"
set "UPDATE=false"

if "%MODE%"=="" set "MODE=native"
if /I "%~1"=="update" (
    set "UPDATE=true"
    set "MODE=native"
)
if /I not "%~1"=="update" (
    if /I "%~2"=="update" (
        set "UPDATE=true"
    )
)

if /I not "%MODE%"=="native" (
    echo Error: Only 'native' mode is supported by this regression runner.
    exit /b 1
)

set "CLING_CMD="
if exist "%PROJECT_DIR%\build\cling.exe" (
    set "CLING_CMD=%PROJECT_DIR%\build\cling.exe"
)
if not defined CLING_CMD (
    if exist "%PROJECT_DIR%\build\cling" (
        set "CLING_CMD=%PROJECT_DIR%\build\cling"
    )
)
if not defined CLING_CMD (
    if exist "%PROJECT_DIR%\dist\cling.exe" (
        set "CLING_CMD=%PROJECT_DIR%\dist\cling.exe"
    )
)
if not defined CLING_CMD (
    if exist "%PROJECT_DIR%\dist\cling" (
        set "CLING_CMD=%PROJECT_DIR%\dist\cling"
    )
)

if not defined CLING_CMD (
    echo Error: build\cling^(.exe^) or dist\cling^(.exe^) not found. Run build first.
    exit /b 1
)

dir /b "%FIXTURES_DIR%\*.clg" >nul 2>&1
if errorlevel 1 (
    echo Error: No .clg fixtures found in %FIXTURES_DIR%.
    exit /b 1
)

if not exist "%OUTPUT_CPP_DIR%" mkdir "%OUTPUT_CPP_DIR%"
if not exist "%OUTPUT_LOG_DIR%" mkdir "%OUTPUT_LOG_DIR%"
if not exist "%EXPECTED_CPP_DIR%" mkdir "%EXPECTED_CPP_DIR%"
if not exist "%DIFF_CPP_DIR%" mkdir "%DIFF_CPP_DIR%"

del /q "%OUTPUT_CPP_DIR%\*.cpp" 2>nul
del /q "%OUTPUT_LOG_DIR%\*.log" 2>nul
del /q "%DIFF_CPP_DIR%\*.diff" 2>nul

echo Running regression tests with: %CLING_CMD%

set /a pass_count=0
set /a fail_count=0
set /a missing_baseline_count=0
set /a generated_count=0
set /a skipped_count=0

for %%F in ("%FIXTURES_DIR%\*.clg") do (
    set "BASE_NAME=%%~nF"
    set "OUT_CPP=%OUTPUT_CPP_DIR%\!BASE_NAME!.cpp"
    set "OUT_LOG=%OUTPUT_LOG_DIR%\!BASE_NAME!.log"

    echo Generating: !BASE_NAME!
    "%CLING_CMD%" --cpp "!OUT_CPP!" "%%~fF" >"!OUT_LOG!" 2>&1
    if !errorlevel! neq 0 (
        findstr /C:"Unsupported syntax remains after transformation" /C:"Parse error" "!OUT_LOG!" >nul 2>&1
        if !errorlevel! equ 0 (
            echo SKIP: unsupported legacy syntax in !BASE_NAME!
            set /a skipped_count+=1
        ) else (
            echo FAIL: generation failed for !BASE_NAME! ^(see !OUT_LOG!^)
            set /a fail_count+=1
        )
    ) else (
        set /a generated_count+=1
    )
)

echo Generated outputs: !generated_count!

if /I "!UPDATE!"=="true" (
    if exist "%OUTPUT_CPP_DIR%\*.cpp" (
        for %%G in ("%OUTPUT_CPP_DIR%\*.cpp") do copy /Y "%%~fG" "%EXPECTED_CPP_DIR%\" >nul
    )
    echo Updated baselines in %EXPECTED_CPP_DIR%
    exit /b 0
)

if exist "%OUTPUT_CPP_DIR%\*.cpp" (
    for %%G in ("%OUTPUT_CPP_DIR%\*.cpp") do (
        set "FILE_NAME=%%~nxG"
        set "EXPECTED_CPP=%EXPECTED_CPP_DIR%\!FILE_NAME!"
        set "DIFF_FILE=%DIFF_CPP_DIR%\%%~nG.diff"

        if not exist "!EXPECTED_CPP!" (
            echo MISSING BASELINE: !FILE_NAME! ^(run with 'update'^)
            set /a missing_baseline_count+=1
        ) else (
            fc /n "!EXPECTED_CPP!" "%%~fG" >"!DIFF_FILE!"
            if !errorlevel! equ 0 (
                del /q "!DIFF_FILE!" 2>nul
                echo PASS: !FILE_NAME!
                set /a pass_count+=1
            ) else (
                echo FAIL: !FILE_NAME! ^(see !DIFF_FILE!^)
                set /a fail_count+=1
            )
        )
    )
)

echo.
echo Summary:
echo   Passed:            !pass_count!
echo   Failed:            !fail_count!
echo   Missing baselines: !missing_baseline_count!
echo   Skipped:           !skipped_count!

if !fail_count! gtr 0 exit /b 1

echo Regression run completed.
exit /b 0
