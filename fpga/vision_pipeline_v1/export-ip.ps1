param(
    [string]$Project='D:\CodexData\visionkit_zcu102\reusable_core\release_hls\quant_engine',
    [string]$Hls='D:\Xilinx\Vitis_HLS\2022.1\bin\vitis_hls.bat'
)
$ErrorActionPreference='Stop'
$Project=(Resolve-Path -LiteralPath $Project).Path
$checked=Get-Content -Raw -LiteralPath "$Project/checked-interface.json" | ConvertFrom-Json
foreach($name in @('quant_engine.cpp','quant_engine.h')) {
    if((Get-FileHash "$PSScriptRoot/hls/$name").Hash.ToLowerInvariant() -ne $checked.source_sha256.$name) {
        throw 'HLS sources differ from the checked build; rebuild before exporting'
    }
}
$env:VISION_BUILD_DIR=$Project.Replace('\','/')
Push-Location (Split-Path $Project)
try {
    $output=& $Hls -f "$PSScriptRoot/hls/export_ip.tcl" 2>&1
    $code=$LASTEXITCODE
    $text=($output | ForEach-Object {"$_"}) -join "`n"
    [IO.File]::WriteAllText("$Project/export-ip.log",$text,[Text.UTF8Encoding]::new($false))
    if($code -ne 0 -or $text -match '(?im)^\s*(ERROR:|Fatal:)' -or -not (Test-Path -LiteralPath "$Project/quant_engine_ip.zip")) {
        throw "IP export failed; inspect $Project/export-ip.log"
    }
    Write-Output "PASS IP archive: $Project/quant_engine_ip.zip"
} finally { Pop-Location }
