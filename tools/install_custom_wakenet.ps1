param(
    [string]$ModelDir = "",
    [switch]$EnableSdkconfig
)

$ErrorActionPreference = "Stop"

$ProjectDir = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($ModelDir)) {
    $ModelDir = Join-Path $ProjectDir "custom_wakenet\hi_xiaomeng"
}
$ModelDir = Resolve-Path $ModelDir

$TargetDir = Join-Path $ProjectDir "managed_components\espressif__esp-sr\model\wakenet_model\wn9_customword"
$BackupDir = Join-Path $ProjectDir "managed_components\espressif__esp-sr\model\wakenet_model\wn9_customword.placeholder"
$Required = @("_MODEL_INFO_", "wn9_data", "wn9_index")

foreach ($name in $Required) {
    $path = Join-Path $ModelDir $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing required model file: $path"
    }
}

if (-not (Test-Path -LiteralPath $BackupDir)) {
    Copy-Item -LiteralPath $TargetDir -Destination $BackupDir -Recurse
    Write-Host "Backed up placeholder model to $BackupDir"
}

foreach ($name in $Required) {
    Copy-Item -LiteralPath (Join-Path $ModelDir $name) -Destination (Join-Path $TargetDir $name) -Force
}

Write-Host "Installed custom WakeNet files into $TargetDir"

if ($EnableSdkconfig) {
    $Sdkconfig = Join-Path $ProjectDir "sdkconfig"
    $text = Get-Content -LiteralPath $Sdkconfig -Raw
    $text = $text -replace "(?m)^CONFIG_SR_WN_WN9_HIESP=y$", "# CONFIG_SR_WN_WN9_HIESP is not set"
    $text = $text -replace "(?m)^# CONFIG_SR_WN_WN9_CUSTOMWORD is not set$", "CONFIG_SR_WN_WN9_CUSTOMWORD=y"
    if ($text -notmatch "(?m)^CONFIG_SR_WN_WN9_CUSTOMWORD=y$") {
        $text += "`r`nCONFIG_SR_WN_WN9_CUSTOMWORD=y`r`n"
    }
    Set-Content -LiteralPath $Sdkconfig -Value $text -NoNewline
    Write-Host "Enabled CONFIG_SR_WN_WN9_CUSTOMWORD in sdkconfig"
}

Write-Host "Next: build and flash the full image so the model partition is updated."
