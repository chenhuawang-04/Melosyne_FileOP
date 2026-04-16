param()

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

clang++ -std=c++23 -O2 -DNDEBUG -Iinclude bench\platform_file_benchmark.cpp src\PlatformFileWin32.cpp -o bench\platform_file_benchmark.exe
clang++ -std=c++23 -O2 -DNDEBUG -Iinclude tests\platform_file_safety.cpp src\PlatformFileWin32.cpp -o tests\platform_file_safety.exe

Write-Host "`n=== 运行安全性测试 ==="
& .\tests\platform_file_safety.exe

Write-Host "`n=== 运行性能基准 ==="
& .\bench\platform_file_benchmark.exe
