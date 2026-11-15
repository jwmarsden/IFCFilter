@echo off
rem Rebuild viewer caches from the federation IFCs in this folder.
rem   regenerate-caches.bat          ask for disciplines; Enter selects all
rem   regenerate-caches.bat CV BR    only those disciplines
setlocal EnableExtensions EnableDelayedExpansion
set "EXE=C:\Users\jwmar\Source\IFCFilter\build\Release\ifcfilter.exe"
set "SOURCE=C:\Apps\IFC"
set "INPUT=%~dp0federate"
set "OUTPUT=C:\Apps\IFCViewer"
if not exist "%EXE%" (echo IFCFilter executable is missing: %EXE% & goto :fail)
if not exist "%SOURCE%\" (echo Source discipline folder is missing: %SOURCE% & goto :fail)
if not exist "%INPUT%\" (echo Federation folder is missing: %INPUT% & goto :fail)
if exist "%~dp0.work\" (echo A regeneration may be running or incomplete. & goto :fail)
set "FAILED=0"
set "COUNT=0"
set "ARGUMENTS=%*"
if "%~1"=="" (
    set "DISCIPLINES="
    for /d %%D in ("%SOURCE%\*") do set "DISCIPLINES=!DISCIPLINES! %%~nxD"
    echo Available disciplines:!DISCIPLINES!
    echo Enter discipline codes separated by spaces, or press Enter for all.
    set /p "ARGUMENTS=Disciplines [all]: "
)
if not defined ARGUMENTS (
    for /d %%D in ("%SOURCE%\*") do call :discipline "%%~nxD"
) else (
    for %%D in (!ARGUMENTS!) do call :discipline "%%~D"
)
echo.
echo Processed %COUNT% models; %FAILED% failed.
if not "%FAILED%"=="0" goto :fail
pause
exit /b 0

:discipline
set "DISC=%~1"
if not exist "%SOURCE%\%DISC%\" (echo Missing source discipline: %DISC% & set /a FAILED+=1 & exit /b)
if not exist "%INPUT%\%DISC%\" (echo No federation output for discipline: %DISC% & set /a FAILED+=1 & exit /b)
if not exist "%OUTPUT%\%DISC%\" mkdir "%OUTPUT%\%DISC%"
for %%F in ("%INPUT%\%DISC%\*.ifc") do if exist "%%~fF" (
    set /a COUNT+=1
    echo Building %%~nxF
    "%EXE%" cache "%%~fF" --out "%OUTPUT%\%DISC%\%%~nF.ifcc"
    if not "!ERRORLEVEL!"=="0" set /a FAILED+=1
)
exit /b

:fail
pause
exit /b 1
