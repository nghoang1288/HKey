# Build portable HKey.exe using w64devkit GCC/Ninja toolchain
param(
    [string]$OutputDir = "$PSScriptRoot/dist/HKey-Portable"
)

$ErrorActionPreference = 'Stop'
$toolchainBin = "E:\Tools\w64devkit\bin"
if (Test-Path $toolchainBin) {
    $env:Path = "$toolchainBin;$env:Path"
}

$root = Resolve-Path "$PSScriptRoot/.."
$buildDir = "$root/Windows/build"

Write-Host "==> Configuring CMake with GCC & Ninja..." -ForegroundColor Cyan
cmake -S "$root/Windows" -B "$buildDir" -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release

Write-Host "==> Building HKey..." -ForegroundColor Cyan
cmake --build "$buildDir" --target hkey

$exeSource = "$buildDir/App/HKey.exe"
if (-not (Test-Path $exeSource)) {
    Write-Error "Build failed: HKey.exe not found at $exeSource"
    exit 1
}

Write-Host "==> Creating Portable distribution at $OutputDir..." -ForegroundColor Cyan
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

Copy-Item $exeSource -Destination "$OutputDir/HKey.exe" -Force

# Create or copy default macro.txt
$macroDest = "$OutputDir/macro.txt"
if (-not (Test-Path $macroDest)) {
    $existingMacro = "$PSScriptRoot/dist/Vikey-Portable/macro.txt"
    $evkMacro = "E:\unikey43RC5-200929-win64\evkmacro.txt"
    if (Test-Path $existingMacro) {
        Write-Host "==> Preserving user macro table from previous build..." -ForegroundColor Green
        Copy-Item $existingMacro -Destination $macroDest -Force
    } elseif (Test-Path $evkMacro) {
        Write-Host "==> Copying existing user macro table from $evkMacro..." -ForegroundColor Green
        Copy-Item $evkMacro -Destination $macroDest -Force
    } else {
        $sample = @'
; ===================================================================
; BẢNG GÕ TẮT HKEY (macro.txt)
; Định dạng hỗ trợ: phím_tắt:từ_thay_thế hoặc phím_tắt||từ_thay_thế
; Tự động giữ hoa/thường: k -> không, K -> Không, K(Caps) -> KHÔNG
; ===================================================================
k:không
dc:được
ng:người
vn:Việt Nam
hk:HKey
'@
        [System.IO.File]::WriteAllText($macroDest, $sample, [System.Text.Encoding]::UTF8)
    }
}

Write-Host "`n[SUCCESS] Portable HKey is ready at: $OutputDir\HKey.exe" -ForegroundColor Green
Write-Host "          Macro file location:      $OutputDir\macro.txt" -ForegroundColor Green
