param(
 [ValidateSet('verify','synth','cosim')][string]$Mode='synth',
 [string]$BuildRoot='D:\CodexData\visionkit_zcu102\linescan_v2\hls_burst',
 [string]$Hls='D:\Xilinx\Vitis_HLS\2022.1\bin\vitis_hls.bat',
 [string]$Python='C:\Users\YANYZ\anaconda3\python.exe'
)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$resolved=(Resolve-Path -LiteralPath $BuildRoot).Path
$env:LINESCAN_BUILD=(Join-Path $resolved 'preprocess_tile').Replace('\','/')
$env:LINESCAN_MODE=$Mode
$sourceHash=(Get-FileHash "$PSScriptRoot/hls/preprocess_tile.cpp").Hash
$headerHash=(Get-FileHash "$PSScriptRoot/hls/preprocess_tile.h").Hash
$marker=Join-Path $resolved 'preprocess_tile/checked-interface.json'
if(Test-Path -LiteralPath $marker) {Remove-Item -LiteralPath $marker}
Push-Location $resolved
try {
 $output=& $Hls -f "$PSScriptRoot/hls/build.tcl" 2>&1
 $code=$LASTEXITCODE
 $text=($output|ForEach-Object {"$_"}) -join "`n"
 [IO.File]::WriteAllText("$resolved/hls-$Mode.log",$text,[Text.UTF8Encoding]::new($false))
 if($code -ne 0 -or $text -match '(?im)^\s*(ERROR:|Fatal:)' -or -not $text.Contains('CSim done with 0 errors.')) {throw "HLS failed: $resolved/hls-$Mode.log"}
 if((Get-FileHash "$PSScriptRoot/hls/preprocess_tile.cpp").Hash -ne $sourceHash -or (Get-FileHash "$PSScriptRoot/hls/preprocess_tile.h").Hash -ne $headerHash) {throw 'Sources changed during HLS build'}
 if($Mode -ne 'verify') {
  & $Python -X utf8 "$PSScriptRoot/scripts/check_hls.py" --project "$resolved/preprocess_tile" --mode $Mode
  if($LASTEXITCODE -ne 0) {throw 'Generated interface verification failed'}
 }
 Write-Output "PASS HLS $Mode : $resolved/preprocess_tile"
} finally {Pop-Location}
