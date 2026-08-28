param(
    [string]$BuildDir = "build_local",
    [string]$IdfPath = "",
    [string]$IdfToolsPath = "",
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Port = "",
    [int]$Jobs = 2
)

$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not $IdfPath -and $env:IDF_PATH -and (Test-Path (Join-Path $env:IDF_PATH "export.ps1"))) {
    $IdfPath = $env:IDF_PATH
}
if (-not $IdfPath) {
    $Candidates = @(
        "C:\Espressif\esp-idf-v5.5.4",
        "C:\Espressif\frameworks\esp-idf-v5.5"
    ) + @(Get-ChildItem "C:\Espressif" -Directory -Filter "esp-idf-v5.5*" -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | ForEach-Object FullName)
    $IdfPath = $Candidates | Where-Object { Test-Path (Join-Path $_ "export.ps1") } | Select-Object -First 1
}
if (-not $IdfPath) {
    throw "ESP-IDF 5.5 was not found. Install it or pass -IdfPath."
}
if (-not $IdfToolsPath) {
    if ($env:IDF_TOOLS_PATH) {
        $IdfToolsPath = $env:IDF_TOOLS_PATH
    }
    elseif (Test-Path "C:\Espressif\tools") {
        $IdfToolsPath = "C:\Espressif\tools"
    }
}

$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"
$env:IDF_PATH = $IdfPath
if ($IdfToolsPath) {
    $env:IDF_TOOLS_PATH = $IdfToolsPath
}

& (Join-Path $IdfPath "export.ps1")
if ($LASTEXITCODE -ne 0) {
    throw "ESP-IDF environment activation failed with exit code $LASTEXITCODE"
}

$env:CMAKE_BUILD_PARALLEL_LEVEL = "$Jobs"
$env:NINJAFLAGS = "-j $Jobs"

$IdfPython = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
if (-not (Test-Path $IdfPython)) {
    throw "ESP-IDF Python environment was not found at $IdfPython"
}

Push-Location $ProjectRoot
try {
    $IdfArgs = @("-B", $BuildDir, "build")
    if ($Flash -or $Monitor) {
        $IdfArgs = @("-B", $BuildDir)
        if ($Port.Length -gt 0) {
            $IdfArgs += @("-p", $Port)
        }
        if ($Flash) {
            $IdfArgs += "flash"
        }
        if ($Monitor) {
            $IdfArgs += "monitor"
        }
    }
    & $IdfPython (Join-Path $IdfPath "tools\idf.py") @IdfArgs
    $IdfExitCode = $LASTEXITCODE
    if ($IdfExitCode -ne 0) {
        exit $IdfExitCode
    }
}
finally {
    Pop-Location
}
