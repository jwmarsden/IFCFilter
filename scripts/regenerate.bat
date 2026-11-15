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
set EXE=%SOURCE%\build\Release\ifcfilter.exe
set INPUT=C:\Apps\IFC
set OUT=%~dp0
if "%OUT:~-1%"=="\" set OUT=%OUT:~0,-1%
set ALL_DISCIPLINES=AR BR BS CC CO CV DU FD FS GD HV HY LA LE LT LV MP MS PA RR RS RW SD SU SV SW TF TT TU TV TW UC UE UG US UT UW VA WF

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
    ) else if exist "%INPUT%\%%A\" (
        set FOLDERS=!FOLDERS! "%INPUT%\%%A"
    ) else (
        echo There's no %INPUT%\%%A folder.
        goto :done
    )
)
if "%FOLDERS%"=="" (
    for %%D in (%ALL_DISCIPLINES%) do set FOLDERS=!FOLDERS! "%INPUT%\%%D"
)

set MAPPING=
if exist "%SOURCE%\mapping.csv" set MAPPING=--mapping "%SOURCE%\mapping.csv"
if not exist "%SOURCE%\data\site-id-lookup.xlsx" (echo SiteID lookup workbook is missing. & goto :done)
set SITELOOKUP=--site-lookup "%SOURCE%\data\site-id-lookup.xlsx"
set PROPSCHEMA=--property-schema "%SOURCE%\data\site-id-attributes.xlsx"
set COMBINE=
if exist "%SOURCE%\combine.csv" set COMBINE=--combine "%SOURCE%\combine.csv"

echo Regenerating into %OUT%
echo from:%FOLDERS%
echo.
"%EXE%" split %FOLDERS% --out "%OUT%" --rules "%SOURCE%\rules.txt" %MAPPING% %COMBINE% %SITELOOKUP% %PROPSCHEMA% %OPTIONS%
set CODE=%ERRORLEVEL%
echo.
if %CODE%==0 (echo Done.) else (echo IFCFilter stopped with exit code %CODE%.)

:done
echo.
pause
endlocal




