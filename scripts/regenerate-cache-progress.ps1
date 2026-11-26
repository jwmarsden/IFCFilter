# Build viewer caches several at once, as many as there are processors and memory allows, and keep a separate
# live progress page and log.
param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$Rules,
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$InputDir,
    [Parameter(Mandatory=$true)][string]$Output,
    [Parameter(Mandatory=$true)][string]$DataDir,
    [Parameter(Mandatory=$true)][string]$ProgressDir,
    [string]$Selection = $env:IFCFILTER_CACHE_SELECTION,
    [int]$Jobs = [int]$env:IFCFILTER_CACHE_JOBS  # how many at once; 0 or unset for as many as there are processors
)
$ErrorActionPreference = 'Stop'
$excluded = @('SW', 'TW')
$disciplines = @()
if ([string]::IsNullOrWhiteSpace($Selection)) {
    $disciplines = @(Get-ChildItem -LiteralPath $Source -Directory | Where-Object { $_.Name -notin $excluded } | Sort-Object Name | ForEach-Object { $_.Name })
} else {
    $disciplines = @($Selection -split '\s+' | Where-Object { $_ } | Select-Object -Unique)
}
$models = [System.Collections.Generic.List[object]]::new()
foreach ($discipline in $disciplines) {
    if ($discipline -notmatch '^[A-Za-z0-9]+$' -or $discipline -in $excluded) { throw "Invalid or excluded discipline: $discipline" }
    $sourcePath = Join-Path $Source $discipline
    $inputPath = Join-Path $InputDir $discipline
    if (!(Test-Path -LiteralPath $sourcePath -PathType Container)) { throw "Missing source discipline: $discipline" }
    if (!(Test-Path -LiteralPath $inputPath -PathType Container)) { throw "No federation output for discipline: $discipline" }
    foreach ($file in @(Get-ChildItem -LiteralPath $inputPath -Filter '*.ifc' -File | Sort-Object Name)) {
        $models.Add([pscustomobject]@{ File=$file; Discipline=$discipline; State='Waiting'; Started=$null; Estimate=[long]($file.Length * 6); Target=$null; Out=$null; Err=$null; Process=$null; Seconds=0.0; ExitCode=$null; OutputBytes=0; Message='' })
    }
}
if (!$models.Count) { throw 'No federation IFC files found for the selected disciplines.' }
# A cache takes up to about 4 times its federation file in memory, measured on files of 38 and 219 MB; 6 leaves room
if ($Jobs -le 0) { $Jobs=[Environment]::ProcessorCount }
$Jobs=[math]::Max(1, [math]::Min($Jobs, $models.Count))
try {
    $budget=[long]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory * 1KB * 0.8)
    if ($budget -le 0) { throw 'Available memory is unavailable.' }
} catch {
    # Without a reliable memory reading, avoid running multiple caches at once.
    Write-Warning 'Could not determine available memory; building one cache at a time.'
    $Jobs=1
    $budget=[long]::MaxValue
}
$page = Join-Path $ProgressDir 'progress-cache.html'
$logDir = Join-Path $ProgressDir 'cache-logs'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$logName = 'cache-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.log'
$log = Join-Path $logDir $logName
$utf8 = [System.Text.UTF8Encoding]::new($false)
$runStart = Get-Date
$finished = $false
function Html([object]$value) { [System.Net.WebUtility]::HtmlEncode([string]$value) }
function Duration([double]$seconds) {
    $seconds = [math]::Max(0, [math]::Round($seconds))
    if ($seconds -ge 3600) { return ('{0} h {1:00} min' -f [math]::Floor($seconds / 3600), [math]::Floor(($seconds % 3600) / 60)) }
    if ($seconds -ge 60) { return ('{0} min {1:00} s' -f [math]::Floor($seconds / 60), ($seconds % 60)) }
    return ("$seconds s")
}
function Write-ProgressPage {
    $now=Get-Date; $elapsed=($now-$runStart).TotalSeconds
    $done=@($models | Where-Object State -eq 'Done').Count
    $failed=@($models | Where-Object State -eq 'Failed').Count
    $running=@($models | Where-Object State -eq 'Running')
    $completed=@($models | Where-Object { $_.State -in @('Done','Failed') })
    $bytesAll=0L; $bytesDone=0L
    foreach ($m in $models) { $bytesAll += $m.File.Length; if ($m.State -in @('Done','Failed')) { $bytesDone += $m.File.Length } }
    $fraction=if ($bytesAll -gt 0) { $bytesDone / $bytesAll } else { ($done+$failed)/[double]$models.Count }
    $left=if ($finished) { 'none' } elseif ($fraction -gt 0 -and $fraction -lt 1) { 'about ' + (Duration ($elapsed / $fraction - $elapsed)) } else { 'working it out' }
    $pageText=[System.Text.StringBuilder]::new()
    [void]$pageText.Append('<!doctype html><html lang="en"><head><meta charset="utf-8">')
    if (!$finished) { [void]$pageText.Append('<meta http-equiv="refresh" content="5">') }
    [void]$pageText.Append('<meta name="viewport" content="width=device-width, initial-scale=1"><title>IFCViewer cache regeneration</title><style>')
    [void]$pageText.Append(':root{color-scheme:light dark}body{font:14px/1.5 system-ui,Segoe UI,sans-serif;max-width:1100px;margin:20px auto;padding:0 16px}h1{font-size:21px;margin-bottom:0}h2{font-size:16px;margin-top:26px}.muted{opacity:.7}.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:8px;margin:14px 0}.stat{border:1px solid #8886;border-radius:8px;padding:8px 10px}.stat b{display:block;font-size:18px}.bad{color:#c33}.track{height:10px;background:#8884;border-radius:5px;overflow:hidden}.fill{height:100%;background:#3485d0}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}th,td{padding:5px 8px;text-align:left;border:1px solid #8885}th{background:#8882}.num{text-align:right}.wrap{overflow-x:auto}</style></head><body>')
    $title=if ($finished -and $failed) { 'finished with failures' } elseif ($finished) { 'finished' } else { 'running' }
    [void]$pageText.Append("<h1>IFCViewer cache regeneration $title</h1>")
    $elapsedDisplay=if ($finished) { Html (Duration $elapsed) } else { '<span class="timer" data-start="' + ([DateTimeOffset]$runStart).ToUnixTimeMilliseconds() + '">' + (Html (Duration $elapsed)) + '</span>' }
    [void]$pageText.Append('<div class="muted">Started ' + (Html ($runStart.ToString('yyyy-MM-dd HH:mm:ss'))) + ' &middot; up to ' + $Jobs + ' at once &middot; elapsed ' + $elapsedDisplay + $(if (!$finished) {' &middot; refreshes every 5 seconds'} else {''}) + ' &middot; <a href="cache-logs/' + (Html $logName) + '">Open log</a></div>')
    [void]$pageText.Append('<div class="stats"><div class="stat">Completed<b>' + $done + ' / ' + $models.Count + '</b></div><div class="stat">Failed<b class="' + $(if ($failed) {'bad'} else {''}) + '">' + $failed + '</b></div><div class="stat">Running<b>' + $running.Count + '</b></div><div class="stat">Input processed<b>' + ('{0:N1} / {1:N1} MB' -f ($bytesDone/1MB),($bytesAll/1MB)) + '</b></div><div class="stat">Time remaining<b>' + (Html $left) + '</b></div></div>')
    [void]$pageText.Append(('<div class="track"><div class="fill" style="width:{0:0.0}%"></div></div>' -f ($fraction*100)))
    [void]$pageText.Append('<h2>Running</h2><div class="wrap"><table><thead><tr><th>Model</th><th>Discipline</th><th class="num">Input MB</th><th>Started</th><th>Running for</th></tr></thead><tbody>')
    foreach ($m in $running) { [void]$pageText.Append('<tr><td>'+(Html $m.File.Name)+'</td><td>'+(Html $m.Discipline)+'</td><td class="num">'+('{0:N1}' -f ($m.File.Length/1MB))+'</td><td>'+(Html ($m.Started.ToString('HH:mm:ss')))+'</td><td><span class="timer" data-start="'+([DateTimeOffset]$m.Started).ToUnixTimeMilliseconds()+'">'+(Html (Duration (($now-$m.Started).TotalSeconds)))+'</span></td></tr>') }
    [void]$pageText.Append('</tbody></table></div>')
    [void]$pageText.Append('<h2>Finished</h2><div class="wrap"><table><thead><tr><th>Model</th><th>Discipline</th><th class="num">Input MB</th><th class="num">Cache MB</th><th>Time</th><th>Result</th></tr></thead><tbody>')
    foreach ($m in @($completed | Sort-Object Started -Descending)) {
        $result=if ($m.State -eq 'Failed') { 'Failed (exit ' + $m.ExitCode + ')' } elseif ($m.Message) { $m.Message } else { 'Done' }
        $cacheSize=if ($m.State -eq 'Done' -and $m.OutputBytes) { '{0:N1}' -f ($m.OutputBytes/1MB) } else { '' }
        [void]$pageText.Append('<tr><td>'+(Html $m.File.Name)+'</td><td>'+(Html $m.Discipline)+'</td><td class="num">'+('{0:N1}' -f ($m.File.Length/1MB))+'</td><td class="num">'+$cacheSize+'</td><td>'+(Html (Duration $m.Seconds))+'</td><td'+$(if ($m.State -eq 'Failed') {' class="bad"'} else {''})+'>'+(Html $result)+'</td></tr>')
    }
    [void]$pageText.Append('</tbody></table></div><h2>Waiting</h2><div class="muted">'+(@($models | Where-Object State -eq 'Waiting').Count)+' models remain.</div><script>function tick(){document.querySelectorAll(".timer").forEach(e=>{let s=Math.max(0,Math.floor((Date.now()-Number(e.dataset.start))/1000));e.textContent=s>=3600?Math.floor(s/3600)+" h "+Math.floor(s%3600/60)+" min":s>=60?Math.floor(s/60)+" min "+s%60+" s":s+" s"})}tick();setInterval(tick,1000)</script></body></html>')
    $temp = $page + '.writing'
    [System.IO.File]::WriteAllText($temp, $pageText.ToString(), $utf8)
    Move-Item -LiteralPath $temp -Destination $page -Force
}
[System.IO.File]::WriteAllText($log, "Cache regeneration started $($runStart.ToString('s')), up to $Jobs at once in $([math]::Round($budget/1GB)) GB of memory`n", $utf8)
# A cache whose federation file is gone, such as a model's once it's combined with others and written to parts,
# would be opened beside the combined file's cache, with the same elements as they were, so it's removed
foreach ($discipline in $disciplines) {
    $cacheDir = Join-Path $Output $discipline
    if (!(Test-Path -LiteralPath $cacheDir -PathType Container)) { continue }
    foreach ($cache in @(Get-ChildItem -LiteralPath $cacheDir -Filter '*.ifcc' -File)) {
        # NAME.federate.ifcc is made from NAME.federate.ifc
        if (Test-Path -LiteralPath (Join-Path (Join-Path $InputDir $discipline) ($cache.BaseName + '.ifc'))) { continue }
        Remove-Item -LiteralPath $cache.FullName
        [System.IO.File]::AppendAllText($log, "Removed $($cache.FullName), as there's no $($cache.BaseName).ifc to make it from`n", $utf8)
        Write-Host "Removed $discipline\$($cache.Name): no federation file for it"
    }
}
Write-ProgressPage
Write-Host "Progress page: $page"
Write-Host "Log: $log"
Write-Host "$($models.Count) models, up to $Jobs at once, in $([math]::Round($budget/1GB)) GB of memory"
$workDir = Join-Path $logDir ('work-' + [IO.Path]::GetFileNameWithoutExtension($logName))
New-Item -ItemType Directory -Path $workDir -Force | Out-Null
# Biggest first, so a big one isn't left running alone at the end
$queue = [System.Collections.Generic.List[object]]::new()
foreach ($m in @($models | Sort-Object { $_.File.Length } -Descending)) { $queue.Add($m) }
$active = [System.Collections.Generic.List[object]]::new()
$inUse = 0L
$n = 0
function Quote([string]$value) { '"' + $value + '"' }
function Start-Model($model) {
    $script:n++
    $model.State='Running'; $model.Started=Get-Date
    $outputDir=Join-Path $Output $model.Discipline
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
    $model.Target=Join-Path $outputDir ($model.File.BaseName + '.ifcc')
    $model.Out=Join-Path $workDir ("$script:n.out"); $model.Err=Join-Path $workDir ("$script:n.err")
    $arguments='cache ' + (Quote $model.File.FullName) + ' --data-dir ' + (Quote $DataDir) + ' --out ' + (Quote $model.Target) + ' --rules ' + (Quote $Rules)
    $model.Process=Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru -NoNewWindow -RedirectStandardOutput $model.Out -RedirectStandardError $model.Err
    $null=$model.Process.Handle  # kept, so its exit code can be read once it's gone
    Write-Host "Started $($model.File.Name)"
}
function Finish-Model($model, [string]$exitCode) {
    $model.ExitCode=$exitCode
    $model.Seconds=((Get-Date)-$model.Started).TotalSeconds
    if ($exitCode -eq '0') {
        $model.State='Done'
        if (Test-Path -LiteralPath $model.Target -PathType Leaf) { $model.OutputBytes=(Get-Item -LiteralPath $model.Target).Length } else { $model.Message='No drawable geometry' }
    } else { $model.State='Failed' }
    # Each model's output goes in the log in one piece, not interleaved with the others'
    $text=[System.Text.StringBuilder]::new()
    [void]$text.Append("`nStarted $($model.File.FullName) at $($model.Started.ToString('s'))`n")
    foreach ($file in @($model.Out, $model.Err)) {
        if ($file -and (Test-Path -LiteralPath $file)) { $body=[System.IO.File]::ReadAllText($file); if ($body) { [void]$text.Append($body.TrimEnd() + "`n") }; Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue }
    }
    [void]$text.Append("Finished $($model.File.Name): $($model.State), exit $($model.ExitCode), $(Duration $model.Seconds)`n")
    [System.IO.File]::AppendAllText($log, $text.ToString(), $utf8)
    Write-Host "Finished $($model.File.Name): $($model.State) in $(Duration $model.Seconds)"
}
try {
    $lastPage=Get-Date
    while ($queue.Count -or $active.Count) {
        # Start what there's room for: a free slot, and memory for it, though one always runs
        $changed=$false
        $i=0
        while ($active.Count -lt $Jobs -and $i -lt $queue.Count) {
            $m=$queue[$i]
            if ($active.Count -eq 0 -or $inUse + $m.Estimate -le $budget) {
                $queue.RemoveAt($i); Start-Model $m; $active.Add($m); $inUse+=$m.Estimate; $changed=$true
            } else { $i++ }
        }
        if ($changed) { Write-ProgressPage; $lastPage=Get-Date }
        Start-Sleep -Milliseconds 500
        foreach ($m in @($active)) {
            if (!$m.Process.HasExited) { continue }
            $m.Process.WaitForExit()
            Finish-Model $m ([string]$m.Process.ExitCode)
            [void]$active.Remove($m); $inUse-=$m.Estimate; $changed=$true
        }
        if ($changed -or ((Get-Date)-$lastPage).TotalSeconds -ge 5) { Write-ProgressPage; $lastPage=Get-Date }
    }
} catch {
    [System.IO.File]::AppendAllText($log, "Error: $_`n", $utf8)
    Write-Error $_ -ErrorAction Continue
} finally {
    # Nothing is left running on its own
    foreach ($m in @($active)) {
        try { if (!$m.Process.HasExited) { $m.Process.Kill() } } catch {}
        Finish-Model $m 'stopped'
    }
    Remove-Item -LiteralPath $workDir -Recurse -Force -ErrorAction SilentlyContinue
    $finished=$true; Write-ProgressPage
}
$failCount=@($models | Where-Object State -eq 'Failed').Count
$waitCount=@($models | Where-Object State -eq 'Waiting').Count
Write-Host "Processed $($models.Count-$waitCount) of $($models.Count) models; $failCount failed."
if ($failCount -or $waitCount) { exit 1 }
exit 0
