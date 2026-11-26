@echo off
rem Regenerates the federate, non-federate and manifest files in this folder from the input IFCs in C:\Apps\IFC,
rem with IFCFilter as it's built in C:\Users\jwmar\Source\IFCFilter.
rem
rem   regenerate.bat              ask for disciplines; Enter selects all
rem   regenerate.bat BR HV FS     just those disciplines
rem   regenerate.bat BR --all     redo every model, even ones that would come out the same
rem
rem Only models whose input, build (ifcfilter.exe, rules.txt, mapping.csv) or shared GUIDs have changed are
rem redone. GUIDs are only made unique across the models in one run, so a run of a few disciplines
rem can't see GUIDs they share with the rest; run them all before federating everything together.
rem Models given a combined name in combine.csv go to parts\DISCIPLINE instead, and each group is
rem written as one federate\DISCIPLINE\NAME.federate.ifc.
rem Nothing is copied to OneDrive: scripts\mirror.ps1 in the source does that.

setlocal enabledelayedexpansion
set SOURCE=C:\Users\jwmar\Source\IFCFilter
if not defined IFC_DATA_DIR set "IFC_DATA_DIR=%USERPROFILE%\OneDrive\Work\IFCData"
set EXE=%SOURCE%\build\Release\ifcfilter.exe
set INPUT=C:\Apps\IFC
set OUT=%~dp0
if "%OUT:~-1%"=="\" set OUT=%OUT:~0,-1%
if not exist "%INPUT%\" (
    echo Input folder is missing: %INPUT%
    goto :done
)
set "ALL_DISCIPLINES="
for /d %%D in ("%INPUT%\*") do if /i not "%%~nxD"=="SW" if /i not "%%~nxD"=="TW" set "ALL_DISCIPLINES=!ALL_DISCIPLINES! %%~nxD"
if not defined ALL_DISCIPLINES (
    echo No discipline folders found in %INPUT%.
    goto :done
)

set "ARGUMENTS=%*"
if "%~1"=="" (
    echo Available disciplines: %ALL_DISCIPLINES%
    echo Enter discipline codes separated by spaces, or press Enter for all.
    set /p "ARGUMENTS=Disciplines [all]: "
)

echo Building IFCFilter...
cmake --build "%SOURCE%\build" --config Release --target ifcfilter
if errorlevel 1 (
    echo Build failed; no models were processed.
    goto :done
)

if not exist "%EXE%" (
    echo IFCFilter isn't built: %EXE% is missing.
    goto :done
)

set FOLDERS=
set OPTIONS=
for %%A in (!ARGUMENTS!) do (
    set ARG=%%A
    if "!ARG:~0,1!"=="-" (
        set OPTIONS=!OPTIONS! %%A
    ) else if /i "%%~A"=="SW" (
        echo SW is excluded from regeneration.
        goto :done
    ) else if /i "%%~A"=="TW" (
        echo TW is excluded from regeneration.
        goto :done
        ) else if exist "%INPUT%\%%A\" (
        set FOLDERS=!FOLDERS! "%INPUT%\%%A"
    ) else (
        echo There's no %INPUT%\%%A folder.
        goto :done
    )
)
if "%FOLDERS%"=="" (
    for /d %%D in ("%INPUT%\*") do if /i not "%%~nxD"=="SW" if /i not "%%~nxD"=="TW" set FOLDERS=!FOLDERS! "%%~fD"
)

set MAPPING=
if exist "%SOURCE%\mapping.csv" set MAPPING=--mapping "%SOURCE%\mapping.csv"
if not exist "%IFC_DATA_DIR%\site-id-lookup.xlsx" (echo SiteID lookup workbook is missing from %IFC_DATA_DIR%. & goto :done)
set SITELOOKUP=--site-lookup "%IFC_DATA_DIR%\site-id-lookup.xlsx"
set PROPSCHEMA=--property-schema "%IFC_DATA_DIR%\site-id-attributes.xlsx"
set COMBINE=
if exist "%SOURCE%\combine.csv" set COMBINE=--combine "%SOURCE%\combine.csv"

echo Regenerating into %OUT%
echo from:%FOLDERS%
echo.
"%EXE%" split %FOLDERS% --data-dir "%IFC_DATA_DIR%" --out "%OUT%" --rules "%SOURCE%\rules.txt" %MAPPING% %COMBINE% %SITELOOKUP% %PROPSCHEMA% %OPTIONS%
set CODE=%ERRORLEVEL%
echo.
if %CODE%==0 (echo Done.) else (echo IFCFilter stopped with exit code %CODE%.)

:done
echo.
pause
endlocal




