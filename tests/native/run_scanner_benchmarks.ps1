param(
    [string]$Platform = "windows",
    [string]$Target = "template_debug",
    [string]$Arch = "x86_64"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "../..")
Push-Location $repoRoot

try {
    scons "platform=$Platform" "target=$Target" "arch=$Arch" "doctest=1"

    $binaryName = "gotool_native_tests"
    if ($Platform -eq "windows") {
        $binaryName = "$binaryName.exe"
    }

    $binaryPath = Join-Path "build/tests/$Platform/$Target/$Arch" $binaryName
    if (-not (Test-Path $binaryPath)) {
        throw "Native test binary was not found at $binaryPath"
    }

    $env:GOTOOL_RUN_SCANNER_BENCHMARK = "1"
    & $binaryPath --test-case="*scanner_benchmark*"

    if ($LASTEXITCODE -ne 0) {
        throw "Benchmark test command failed with exit code $LASTEXITCODE"
    }
}
finally {
    Remove-Item Env:GOTOOL_RUN_SCANNER_BENCHMARK -ErrorAction SilentlyContinue
    Pop-Location
}
