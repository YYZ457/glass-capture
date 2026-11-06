param(
    [ValidateSet('verify','synth','cosim')][string]$Mode='synth',
    [string]$BuildRoot='D:\CodexData\visionkit_zcu102\reusable_core\release_hls',
    [string]$Hls='D:\Xilinx\Vitis_HLS\2022.1\bin\vitis_hls.bat',
    [string]$Python='C:\Users\YANYZ\anaconda3\python.exe'
)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$resolved=(Resolve-Path -LiteralPath $BuildRoot).Path
$env:VISION_BUILD_DIR=(Join-Path $resolved 'quant_engine').Replace('\','/')
$env:VISION_HLS_MODE=$Mode
$sourceHash=(Get-FileHash "$PSScriptRoot/hls/quant_engine.cpp" -Algorithm SHA256).Hash
$headerHash=(Get-FileHash "$PSScriptRoot/hls/quant_engine.h" -Algorithm SHA256).Hash
$marker=Join-Path $resolved 'quant_engine/checked-interface.json'
if(Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker }
Push-Location $resolved
try {
    $output=& $Hls -f "$PSScriptRoot/hls/build.tcl" 2>&1
    $code=$LASTEXITCODE
    $text=($output | ForEach-Object {"$_"}) -join "`n"
    [IO.File]::WriteAllText("$resolved/hls-$Mode.log",$text,[Text.UTF8Encoding]::new($false))
    if($code -ne 0 -or $text -match '(?im)^\s*(ERROR:|Fatal:)' -or -not $text.Contains('CSim done with 0 errors.')) {
        throw "HLS failed; inspect $resolved/hls-$Mode.log"
    }
    if((Get-FileHash "$PSScriptRoot/hls/quant_engine.cpp").Hash -ne $sourceHash -or (Get-FileHash "$PSScriptRoot/hls/quant_engine.h").Hash -ne $headerHash) { throw 'HLS sources changed during build' }
    if($Mode -ne 'verify') {
        & $Python -X utf8 "$PSScriptRoot/scripts/check_hls.py" --project "$resolved/quant_engine" --mode $Mode
        if($LASTEXITCODE -ne 0) { throw 'Generated interface/report validation failed' }
    }
    Write-Output "PASS HLS $Mode; artifacts: $resolved/quant_engine"
} finally { Pop-Location }
