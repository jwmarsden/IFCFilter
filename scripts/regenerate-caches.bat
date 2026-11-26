@echo off
rem Rebuild viewer caches from the federation IFCs in this folder.
rem   regenerate-caches.bat          ask for disciplines; Enter selects all
rem   regenerate-caches.bat CV BR    only those disciplines
setlocal EnableExtensions EnableDelayedExpansion
if not defined IFC_DATA_DIR set "IFC_DATA_DIR=%USERPROFILE%\OneDrive\Work\IFCData"
set "EXE=C:\Users\jwmar\Source\IFCFilter\build\Release\ifcfilter.exe"
set "RULES=C:\Users\jwmar\Source\IFCFilter\rules.txt"
set "SOURCE=C:\Apps\IFC"
set "INPUT=%~dp0federate"
set "OUTPUT=C:\Apps\IFCViewer"
if not exist "%EXE%" (echo IFCFilter executable is missing: %EXE% & goto :fail)
if not exist "%RULES%" (echo Rules file is missing: %RULES% & goto :fail)
if not exist "%SOURCE%\" (echo Source discipline folder is missing: %SOURCE% & goto :fail)
if not exist "%INPUT%\" (echo Federation folder is missing: %INPUT% & goto :fail)
if exist "%~dp0.work\" (echo A regeneration may be running or incomplete. & goto :fail)
set "ARGUMENTS=%*"
if "%~1"=="" (
    set "DISCIPLINES="
    for /d %%D in ("%SOURCE%\*") do if /i not "%%~nxD"=="SW" if /i not "%%~nxD"=="TW" set "DISCIPLINES=!DISCIPLINES! %%~nxD"
    echo Available disciplines:!DISCIPLINES!
    echo Enter discipline codes separated by spaces, or press Enter for all.
    set /p "ARGUMENTS=Disciplines [all]: "
)
set "IFCFILTER_CACHE_SELECTION=!ARGUMENTS!"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\Users\jwmar\Source\IFCFilter\scripts\regenerate-cache-progress.ps1" -Exe "%EXE%" -Rules "%RULES%" -Source "%SOURCE%" -InputDir "%INPUT%" -Output "%OUTPUT%" -DataDir "%IFC_DATA_DIR%" -ProgressDir "%~dp0."
set "CODE=!ERRORLEVEL!"
echo.
pause
exit /b !CODE!

:fail
pause
exit /b 1
