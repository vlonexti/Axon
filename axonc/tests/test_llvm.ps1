# test_llvm.ps1 - checks the parts of Axon that use LLVM: the LLVM backend and C header import (libclang).
# Needs LLVM unpacked into tools\llvm (or clang/libclang on PATH). Everything is written to llvm-test.log.
# Usage: powershell -ExecutionPolicy Bypass -File axonc\tests\test_llvm.ps1
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$axon = Join-Path $root "bin\axon.exe"
$log = Join-Path $PSScriptRoot "llvm-test.log"
$work = Join-Path $env:TEMP "axon-llvm-test"
"Axon LLVM test - $(Get-Date)" | Out-File -Encoding utf8 $log
function Log($text) { $text | Out-File -Encoding utf8 -Append $log; Write-Host $text }
function Run($title, [scriptblock]$cmd) {
    Log "`n===== $title"
    $out = & $cmd 2>&1 | ForEach-Object { "$_" }
    Log ($out -join "`n")
    Log "(exit code $LASTEXITCODE)"
}

$clang = Get-ChildItem (Join-Path $root "tools\llvm") -Recurse -Filter clang.exe -ErrorAction SilentlyContinue | Select-Object -First 1
$libclang = Get-ChildItem (Join-Path $root "tools\llvm") -Recurse -Filter libclang.dll -ErrorAction SilentlyContinue | Select-Object -First 1
Log "clang:    $($clang.FullName)"
Log "libclang: $($libclang.FullName)"
if (-not $clang) { Log "LLVM is not unpacked into tools\llvm yet"; exit 1 }
Run "clang version" { & $clang.FullName --version }

# 1. the whole test suite on the LLVM backend
Run "test suite, --backend llvm" { powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run_tests.ps1") -Backend llvm }

# 2. bindings generated from raylib.h
$raylibInc = Join-Path $root "examples\raylib-square\libs\raylib\include"
Run "axon bindgen raylib.h" { & $axon bindgen raylib.h -I $raylibInc -o (Join-Path $PSScriptRoot "raylib_generated.ax") }
Log (Get-Content (Join-Path $PSScriptRoot "raylib_generated.ax") -TotalCount 40 -ErrorAction SilentlyContinue | Out-String)

# 3. projects without hand-written bindings: `using c "x.h"` reads the header itself
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
Copy-Item -Recurse (Join-Path $root "examples\raylib-square") (Join-Path $work "square")
Remove-Item -Recurse -Force (Join-Path $work "square\bindings"), (Join-Path $work "square\build") -ErrorAction SilentlyContinue
Copy-Item -Recurse (Join-Path $root "axonc\tests\cinterop") (Join-Path $work "cinterop")
Remove-Item -Recurse -Force (Join-Path $work "cinterop\bindings"), (Join-Path $work "cinterop\build") -ErrorAction SilentlyContinue
Push-Location (Join-Path $work "square")
Run "raylib-square with imported bindings (C backend)" { & $axon build }
Run "raylib-square with imported bindings (LLVM backend)" { & $axon build --backend llvm }
Pop-Location
Push-Location (Join-Path $work "cinterop")
Run "cinterop with imported bindings" { & $axon run }
Run "cinterop with imported bindings, LLVM backend" { & $axon run --backend llvm }
Log (Get-Content (Join-Path $work "cinterop\build\bindings\vec.ax") -ErrorAction SilentlyContinue | Out-String)
Pop-Location

# 4. examples on the LLVM backend
Push-Location (Join-Path $root "examples\zombies")
Run "zombies tests, LLVM backend, leak check" { & $axon test --backend llvm --leak-check }
Pop-Location
Push-Location (Join-Path $root "examples")
Run "testing.ax (expects 2 failures), LLVM backend" { & $axon test testing.ax --backend llvm }
Run "fib --release on LLVM" { & $axon run fib.ax --backend llvm --release }
Pop-Location

Log "`nDone. The log is $log"
