# Keeps a copy of an output folder up to date somewhere else, such as a synced folder. Files that are
# gone from the output go from the copy too, so -To has to be a folder that's only for this.
#
#   .\scripts\mirror.ps1 -From C:\Apps\BIMPass -To C:\Users\jwmar\OneDrive\Work\BIMPass
#   .\scripts\mirror.ps1 -From C:\Apps\BIMPass -To C:\Users\jwmar\OneDrive\Work\BIMPass -List
#
# Run it after a run has been checked. -List says what would change, and changes nothing.
param(
    [Parameter(Mandatory)][string]$From,
    [Parameter(Mandatory)][string]$To,
    [switch]$List
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path (Join-Path $From 'register.csv'))) {
    throw "$From has no register.csv, so it isn't a finished output folder"
}
if (Test-Path (Join-Path $From '.work')) {
    throw "$From has a .work folder: a run is going on, or was stopped part way"
}
# Anything already in the copy that the output doesn't have would be deleted, so it has to be a copy to start with
if ((Test-Path $To) -and (Get-ChildItem $To -Force | Select-Object -First 1) -and -not (Test-Path (Join-Path $To 'register.csv'))) {
    throw "$To has files in it but isn't a copy of an output folder, so nothing was changed"
}

# Navisworks leaves its cache files beside what it opens. They aren't output, so they don't get copied.
$options = @('/MIR', '/R:5', '/W:10', '/NP', '/NDL', '/FFT', '/XJ', '/XF', '*.nwc')
if ($List) { $options += '/L' }
robocopy $From $To @options
# Below 8 is robocopy saying that it worked
if ($LASTEXITCODE -ge 8) {
    throw "robocopy failed with exit code $LASTEXITCODE"
}
exit 0
