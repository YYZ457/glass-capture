param(
 [string]$Project='D:\CodexData\visionkit_zcu102\linescan_v2\hls_burst\preprocess_tile',
 [string]$Hls='D:\Xilinx\Vitis_HLS\2022.1\bin\vitis_hls.bat'
)
$ErrorActionPreference='Stop'
$Project=(Resolve-Path -LiteralPath $Project).Path
$checked=Get-Content -Raw -LiteralPath "$Project/checked-interface.json" | ConvertFrom-Json
if(-not $checked.rtl_cosim_passed){throw 'Export requires a passing RTL cosimulation'}
foreach($name in @('preprocess_tile.cpp','preprocess_tile.h')) {
 if((Get-FileHash "$PSScriptRoot/hls/$name").Hash.ToLowerInvariant() -ne $checked.source_sha256.$name){throw 'Source changed: rebuild and verify first'}
}
$env:LINESCAN_BUILD=$Project.Replace('\','/')
Push-Location (Split-Path $Project)
try {
 $output=& $Hls -f "$PSScriptRoot/hls/export_ip.tcl" 2>&1
 $code=$LASTEXITCODE;$text=($output|ForEach-Object {"$_"}) -join "`n"
 [IO.File]::WriteAllText("$Project/export-ip.log",$text,[Text.UTF8Encoding]::new($false))
 if($code -ne 0 -or $text -match '(?im)^\s*(ERROR:|Fatal:)' -or -not (Test-Path -LiteralPath "$Project/preprocess_tile_ip.zip")){throw 'IP export failed'}
 Write-Output "PASS IP archive: $Project/preprocess_tile_ip.zip"
} finally {Pop-Location}
