param(
 [string]$BuildRoot='D:\CodexData\visionkit_zcu102\linescan_v2\verify',
 [string]$Python='C:\Users\YANYZ\anaconda3\python.exe',
 [string]$Cxx='D:\Xilinx\Vitis_HLS\2022.1\tps\mingw\8.3.0\win64.o\nt\bin\g++.exe'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$BuildRoot=(Resolve-Path -LiteralPath $BuildRoot).Path
$sourceHashes=@{}
Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object {$_.Extension -in @('.cpp','.h','.hpp','.sv','.py')} | ForEach-Object {
 $sourceHashes[$_.FullName.Substring($root.Length+1).Replace('\','/')]=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()
}
function Run([string]$Program,[string[]]$Arguments,[string]$Log,[string]$Pass='') {
 $output=& $Program @Arguments 2>&1
 $code=$LASTEXITCODE;$text=($output|ForEach-Object {"$_"}) -join "`n"
 [IO.File]::WriteAllText($Log,$text,[Text.UTF8Encoding]::new($false))
 if($code -ne 0 -or ($Pass -and -not $text.Contains($Pass))){throw "Test failed: $Log (exit $code)"}
}
Push-Location $root
try {
 Run $Cxx @('-std=c++11','-O2','-Wall','-Wextra','-Wno-unknown-pragmas',"$root/hls/preprocess_tile.cpp","$root/tests/test_preprocess_tile.cpp",'-o',"$BuildRoot/test_preprocess.exe") "$BuildRoot/native-build.log"
 Run "$BuildRoot/test_preprocess.exe" @() "$BuildRoot/native-tests.log" 'PASS'
 Run $Cxx @('-std=c++11','-O2','-Wall','-Wextra','-Werror','-pedantic',"$root/tests/test_stream_components_cpp.cpp",'-o',"$BuildRoot/test_components.exe") "$BuildRoot/components-build.log"
 Run "$BuildRoot/test_components.exe" @() "$BuildRoot/components-tests.log" 'PASS'
 Run $Cxx @('-std=c++11','-O2','-shared','-static-libgcc','-static-libstdc++',"$root/hls/preprocess_tile.cpp","$root/hls/native_bridge.cpp",'-o',"$BuildRoot/linescan_preprocess.dll") "$BuildRoot/dll-build.log"
 $env:LINESCAN_NATIVE_LIBRARY="$BuildRoot/linescan_preprocess.dll"
 Run $Python @('-X','utf8','-m','pytest',"$root/tests",'-q','-ra') "$BuildRoot/python-tests.log" 'passed'
 if((Get-Content -Raw "$BuildRoot/python-tests.log") -match '\d+ skipped'){throw 'Tests must not be skipped'}
 foreach($relative in $sourceHashes.Keys){
  if((Get-FileHash -LiteralPath (Join-Path $root $relative)).Hash.ToLowerInvariant() -ne $sourceHashes[$relative]){throw "Source changed during test: $relative"}
 }
 [IO.File]::WriteAllText("$BuildRoot/verified-sources.json",($sourceHashes|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
 Write-Output "PASS native operators, strip/cache/CC and pipeline tests: $BuildRoot"
} finally {Pop-Location}
