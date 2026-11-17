# Engine lock on Windows: builds test tools (MSVC in CI/VS Dev Shell, or w64devkit GCC locally)
# and runs the same checks as Tools/autoswitch/check.sh.
# usage: pwsh Windows/check.ps1 [update]
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'          # Python subprocess text I/O must be UTF-8, not the console code page
$env:PYTHONIOENCODING = 'utf-8'
$root = Resolve-Path "$PSScriptRoot/.."
$as = "$root/Tools/autoswitch"
$buildDir = "$root/Windows/build"

$hasMsvc = $null -ne (Get-Command cl.exe -ErrorAction SilentlyContinue)
$cmakeExtra = @()
$wantCompiler = 'msvc'
if (-not $hasMsvc -and ($env:GITHUB_ACTIONS -ne 'true')) {
    $toolchainBin = "E:\Tools\w64devkit\bin"
    if (Test-Path "$toolchainBin\g++.exe") {
        $env:Path = "$toolchainBin;$env:Path"
        $cmakeExtra = @('-DCMAKE_CXX_COMPILER=g++', '-DCMAKE_C_COMPILER=gcc')
        $wantCompiler = 'gcc'
    }
}

$cacheFile = Join-Path $buildDir "CMakeCache.txt"
if (Test-Path $cacheFile) {
    $cacheText = Get-Content $cacheFile -Raw
    $isGccCache = $cacheText -match 'CMAKE_CXX_COMPILER:.*g\+\+'
    if (($wantCompiler -eq 'msvc' -and $isGccCache) -or ($wantCompiler -eq 'gcc' -and -not $isGccCache)) {
        Remove-Item -Recurse -Force $buildDir
    }
}

cmake -S "$root/Windows" -B $buildDir -G Ninja @cmakeExtra -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE) { exit 1 }
cmake --build $buildDir --config Release
if ($LASTEXITCODE) { exit 1 }
$rc = 0
if (-not $env:TMPDIR) { $env:TMPDIR = $env:TEMP }
python "$as/rules/run_rules.py"; if ($LASTEXITCODE) { $rc = 1 }
& "$buildDir/App/macro_test.exe"; if ($LASTEXITCODE) { $rc = 1 }
& "$buildDir/App/unit_test.exe"; if ($LASTEXITCODE) { $rc = 1 }
$mode = if ($args.Count -gt 0) { $args[0] } else { 'check' }
python "$as/rules/golden.py" $mode; if ($LASTEXITCODE) { $rc = 1 }
$u = Join-Path $env:TMPDIR 'vikey_check_chat.units'
# write the units file from Python itself (a PowerShell pipeline would re-encode the Vietnamese text)
$prep = ($as -replace '\\', '/') + '/sim/prep.py'; $corpus = ($as -replace '\\', '/') + '/sim/corpus/chat_messages.txt'; $uu = $u -replace '\\', '/'
python -c "import runpy, sys, io; sys.stdout = io.TextIOWrapper(open(r'$uu', 'wb'), encoding='utf-8', newline='\n'); sys.argv = ['prep.py', r'$corpus']; runpy.run_path(r'$prep', run_name='__main__'); sys.stdout.flush()"
# driver == simulator (what cmp.sh does, without zsh)
$env:DRV_OUT = Join-Path $env:TMPDIR 'drv.txt'
& "$as/driver/drvlazy.exe" $u --start vi | Out-Null
& "$as/sim/sim.exe" $u --start vi --lazy --dump (Join-Path $env:TMPDIR 'simdump.txt') | Out-Null
$cmp = @"
import os, re
t = os.environ['TMPDIR']
d = open(os.path.join(t, 'drv.txt'), encoding='utf-8').read().split('\n')
s = [l.split('\t')[0] for l in open(os.path.join(t, 'simdump.txt'), encoding='utf-8').read().rstrip('\n').split('\n')]
D = re.sub(r'[\W\d_]+', '', ''.join(d)); S = re.sub(r'[\W\d_]+', '', ''.join(s))
print('driver == simulator:', 'ok' if D == S else 'FAIL', len(D), len(S))
raise SystemExit(0 if D == S else 1)
"@
python -c $cmp; if ($LASTEXITCODE) { $rc = 1 }
& "$as/driver/bench.exe" $u 1000 20000; if ($LASTEXITCODE) { Write-Host 'FAIL performance budget'; $rc = 1 } else { Write-Host 'performance: ok' }
if ($rc) { Write-Host 'CHECK FAILED'; exit 1 } else { Write-Host 'ALL CHECKS PASSED' }
