# run_tests.ps1 - the Axon compiler test suite.
#   tests\run\*.ax      must compile, and their output must match the .out file next to them
#   tests\errors\*.ax   must FAIL to compile, with the message from their first line (`// error: ...`)
#   tests\cinterop      a project that calls a small C library
# Usage:  powershell -ExecutionPolicy Bypass -File axonc\tests\run_tests.ps1 [-Update]
param([switch]$Update, [string]$Backend = "c")

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$axon = Join-Path $root "..\..\bin\axon.exe"
if (-not (Test-Path $axon)) { Write-Host "build the compiler first: build.bat"; exit 1 }
$axon = (Resolve-Path $axon).Path
$pass = 0; $fail = 0

function Report($ok, $name, $detail) {
    if ($ok) { $script:pass++; Write-Host "  ok    $name" -ForegroundColor Green }
    else { $script:fail++; Write-Host "  FAIL  $name" -ForegroundColor Red; if ($detail) { Write-Host $detail } }
}

function RunExe($exe) {
    $out = & $exe 2>&1 | ForEach-Object { "$_" }
    return ($out -join "`n").TrimEnd()
}

Write-Host "run tests"
Push-Location (Join-Path $root "run")
foreach ($f in Get-ChildItem *.ax) {
    $name = $f.BaseName
    $build = & $axon build $f.Name --backend $Backend 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { Report $false $name ($build -join "`n"); continue }
    $actual = RunExe (Join-Path $f.DirectoryName "build\$name.exe")
    $expFile = Join-Path $f.DirectoryName "$name.out"
    if ($Update -or -not (Test-Path $expFile)) { [IO.File]::WriteAllText($expFile, $actual, (New-Object Text.UTF8Encoding $false)) }
    $expected = ((Get-Content -Raw -Encoding utf8 $expFile) -replace "`r`n", "`n").TrimEnd()
    Report ($actual -eq $expected) $name "--- expected`n$expected`n--- actual`n$actual"
}
Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
Pop-Location

Write-Host "error tests"
Push-Location (Join-Path $root "errors")
foreach ($f in Get-ChildItem *.ax) {
    $want = (Get-Content $f.FullName -TotalCount 1) -replace '^// error: ', ''
    $out = (& $axon check $f.Name 2>&1 | ForEach-Object { "$_" }) -join "`n"
    $ok = ($LASTEXITCODE -ne 0) -and $out.Contains($want)
    Report $ok $f.BaseName "expected an error containing: $want`n$out"
}
Pop-Location

Write-Host "projects"
Push-Location (Join-Path $root "cinterop")
$build = & $axon build 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { Report $false "cinterop" ($build -join "`n") }
else {
    $actual = RunExe (Join-Path (Get-Location) "build\cinterop.exe")
    Report ($actual.Contains("C says hello to Steve") -and $actual.Contains("space is down")) "cinterop" $actual
}
Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
Pop-Location

Write-Host "self-hosting"
$boot = (cmd /c (Join-Path $root "..\..\selfhost\bootstrap.bat") 2>&1 | ForEach-Object { "$_" }) -join "`n"
Report ($boot.Contains("axs compiles itself")) "axs bootstrap (stage 1 == stage 2)" $boot

Write-Host ""
Write-Host "$pass passed, $fail failed"
if ($fail -gt 0) { exit 1 }
