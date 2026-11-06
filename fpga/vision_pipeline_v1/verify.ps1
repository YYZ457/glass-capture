param(
    [string]$BuildRoot = 'D:\CodexData\visionkit_zcu102\reusable_core\verify',
    [string]$Python = 'C:\Users\YANYZ\anaconda3\python.exe',
    [string]$Vivado = 'D:\Xilinx\Vivado\2022.1\bin',
    [string]$Cxx = 'D:\Xilinx\Vitis_HLS\2022.1\tps\mingw\8.3.0\win64.o\nt\bin\g++.exe'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$BuildRoot=(Resolve-Path -LiteralPath $BuildRoot).Path
$sourceHashes=@{}
Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.Extension -in @('.cpp','.h','.sv','.py') } | ForEach-Object {
    $sourceHashes[$_.FullName.Substring($root.Length+1).Replace('\','/')]=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()
}
function Invoke-Checked([string]$Program,[string[]]$Arguments,[string]$Log,[string]$Pass='') {
    $output = & $Program @Arguments 2>&1
    $code=$LASTEXITCODE
    $text=($output | ForEach-Object { "$_" }) -join "`n"
    [IO.File]::WriteAllText($Log,$text,[Text.UTF8Encoding]::new($false))
    if($code -ne 0 -or $text -match '(?im)^\s*(Fatal:|ERROR:)' -or ($Pass -and -not $text.Contains($Pass))) {
        throw "Verification failed: $Log (exit $code)"
    }
}
Push-Location $BuildRoot
try {
    Invoke-Checked $Cxx @('-std=c++11','-O2','-Wall','-Wextra','-Wno-unknown-pragmas',"$root/hls/quant_engine.cpp","$root/tests/test_quant_engine.cpp",'-o',"$BuildRoot/quant_test.exe") "$BuildRoot/compile.log"
    Invoke-Checked "$BuildRoot/quant_test.exe" @() "$BuildRoot/numeric.log" 'all independent reference, boundary and no-write rejection tests passed'
    Invoke-Checked $Cxx @('-std=c++11','-O2','-shared','-static-libgcc','-static-libstdc++',"$root/hls/quant_engine.cpp","$root/hls/native_bridge.cpp",'-o',"$BuildRoot/vision_quant.dll") "$BuildRoot/native-build.log"
    $env:VISION_NATIVE_LIBRARY="$BuildRoot/vision_quant.dll"
    Invoke-Checked $Python @('-X','utf8','-m','unittest','discover','-s',"$root/tests",'-p','test_*.py','-v') "$BuildRoot/python-tests.log" 'OK'
    foreach($test in @('stream_fifo','job_supervisor','gray_frame_framer')) {
        $testdir=Join-Path $BuildRoot $test
        New-Item -ItemType Directory -Force -Path $testdir | Out-Null
        Push-Location $testdir
        try {
            Invoke-Checked "$Vivado/xvlog.bat" @('-sv',"$root/rtl/$test.sv","$root/tests/tb_$test.sv") "$testdir/compile.log"
            Invoke-Checked "$Vivado/xelab.bat" @("tb_$test",'-s',"sim_$test") "$testdir/elaborate.log"
            Invoke-Checked "$Vivado/xsim.bat" @("sim_$test",'-runall','-log',"$testdir/simulator.log") "$testdir/run.log" "PASS tb_$test"
        } finally { Pop-Location }
    }
    # XSim may return zero after $fatal: calibrate the log-based failure gate.
    Push-Location (Join-Path $BuildRoot 'stream_fifo')
    try {
        $caught=$false
        try {
            Invoke-Checked "$Vivado/xsim.bat" @('sim_stream_fifo','-runall','-testplusarg','FIFO_INJECT_FAILURE','-log',"$BuildRoot/injected-simulator.log") "$BuildRoot/injected-failure.log" 'PASS tb_stream_fifo'
        } catch { $caught=$true }
        if(-not $caught) { throw 'Failure injection was incorrectly accepted' }
        if(-not (Get-Content -Raw "$BuildRoot/injected-failure.log").Contains('Fatal: Intentional FIFO failure-path self-test')) { throw 'Failure injection did not reach intended assertion' }
    } finally { Pop-Location }
    foreach($relative in $sourceHashes.Keys) {
        if((Get-FileHash -LiteralPath (Join-Path $root $relative)).Hash.ToLowerInvariant() -ne $sourceHashes[$relative]) { throw "Sources changed during tests: $relative" }
    }
    [IO.File]::WriteAllText("$BuildRoot/verified-sources.json",($sourceHashes | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    Write-Output "PASS: native operators, composition, CPU preprocessing, host lifecycle, RTL, failure gate. Logs: $BuildRoot"
} finally { Pop-Location }
