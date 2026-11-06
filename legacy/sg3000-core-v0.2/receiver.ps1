param([string]$Compiler = 'D:\Xilinx\Vitis\2022.1\gnu\aarch64\nt\aarch64-linux\bin\aarch64-linux-gnu-gcc.exe')
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot
$output = Join-Path $PSScriptRoot 'output'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$sources = @((Join-Path $project 'core/sg3000_capture.c'), (Join-Path $project 'core/sg3000_capture_core.c'))
& $Compiler -std=c11 -O2 -Wall -Wextra -Werror -static @sources -o (Join-Path $output 'sg3000-capture')
if ($LASTEXITCODE -ne 0) { throw 'Receiver compilation failed' }
$readelf = Join-Path (Split-Path $Compiler) 'aarch64-linux-gnu-readelf.exe'
$header = (& $readelf -h (Join-Path $output 'sg3000-capture')) -join "`n"
if ($LASTEXITCODE -ne 0 -or $header -notmatch 'Machine:\s+AArch64') { throw 'Wrong output architecture' }
$segments = (& $readelf -l (Join-Path $output 'sg3000-capture')) -join "`n"
if ($LASTEXITCODE -ne 0 -or $segments -match 'INTERP') { throw 'Expected a static executable' }
Get-FileHash -LiteralPath (Join-Path $output 'sg3000-capture') -Algorithm SHA256
Write-Output 'Built into build/output. The verified board/ files were not changed.'
$streamSources = @('sg_stream_main.c','sg3000_capture_core.c','sg_pipeline.c','sg_processing.c','sg_pl_linux.c') | ForEach-Object { Join-Path $project "core/$_" }
& $Compiler -std=c11 -O2 -Wall -Wextra -Werror -static -pthread @streamSources -o (Join-Path $output 'sg3000-stream')
if ($LASTEXITCODE -ne 0) { throw 'Continuous receiver compilation failed' }
Get-FileHash -LiteralPath (Join-Path $output 'sg3000-stream') -Algorithm SHA256
