# The gate (VIII) on Windows: builds everything including DlssScreen.exe with warnings as errors and
# tracing, runs the formatter check, the lint, the property tests, mutation testing and the lock check.
# Usage: pwsh gate/gate.ps1 -DlssSdkDir C:\DLSS [-NvofSdkDir C:\NVOF] [-Mutants 40]
param(
    [Parameter(Mandatory = $true)][string]$DlssSdkDir,
    [string]$NvofSdkDir = "",
    [int]$Mutants = 40,
    [double]$Threshold = 0.8,
    [string]$BuildDir = "build-gate-win"
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

$nvof = if ($NvofSdkDir -ne "") { "ON" } else { "OFF" }
Write-Host "== configure and build (MSVC, /WX, /Gh /GH tracing, NVOF=$nvof)"
cmake -S . -B $BuildDir -G "Visual Studio 17 2022" -A x64 "-DDLSS_SDK_DIR=$DlssSdkDir" "-DNVOF_SDK_DIR=$NvofSdkDir" "-DDSCREEN_ENABLE_NVOF=$nvof"
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }
if ($nvof -eq "OFF") {
    Write-Host "== the other value of the feature flag (R31): NVOF=ON is built when NvofSdkDir is given"
}

Write-Host "== formatter"
$sources = git ls-files '*.cpp' '*.h'
clang-format --dry-run --Werror --style=file $sources
if ($LASTEXITCODE -ne 0) { throw "formatter check failed" }

Write-Host "== rules lint, function index and inventories"
& "$BuildDir/Release/rules_lint.exe" . "$BuildDir/gate"
if ($LASTEXITCODE -ne 0) { throw "rules lint failed" }

Write-Host "== property tests and seed fuzzer"
ctest --test-dir $BuildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "tests failed" }
foreach ($seed in 4000, 5000, 6000) {
    & "$BuildDir/Release/dscreen_tests.exe" $seed
    if ($LASTEXITCODE -ne 0) { throw "seed $seed failed" }
}

Write-Host "== mutation testing ($Mutants mutants, threshold $Threshold)"
python gate/mutate.py --build-dir $BuildDir --target dscreen_tests --sample $Mutants --threshold $Threshold
if ($LASTEXITCODE -ne 0) { throw "mutation score below threshold" }

Write-Host "== dependency lock"
python gate/check_lock.py $DlssSdkDir $NvofSdkDir
if ($LASTEXITCODE -ne 0) { throw "dependency lock mismatch" }

Write-Host "gate: passed"
