# 构建 DeskHide.exe —— 只需要 MinGW-w64（g++ / windres），无第三方依赖
param(
    [string]$Gxx     = 'D:\mingw64\bin\g++.exe',
    [string]$WindRes = 'D:\mingw64\bin\windres.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $root 'src'
$res  = Join-Path $root 'build_res.o'

& $WindRes --codepage=65001 -I $src -i (Join-Path $src 'app.rc') -o $res
if ($LASTEXITCODE -ne 0) { throw 'windres failed' }

& $Gxx -std=c++17 -municode -mwindows -O2 -s `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    -static -static-libgcc -static-libstdc++ -fno-exceptions -fno-rtti `
    -Wall -Wextra -Wno-unused-parameter `
    -o (Join-Path $root 'DeskHide.exe') (Join-Path $src 'deskhide.cpp') $res -lshell32
if ($LASTEXITCODE -ne 0) { throw 'g++ failed' }

Remove-Item $res -Force
Get-Item (Join-Path $root 'DeskHide.exe') | Select-Object Name, Length, LastWriteTime
