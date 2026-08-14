# Work4 文件快搜 - 一键构建脚本
# 用法: powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

# 1) 编译（使用本机 MinGW 工具链）
$mingwBin = 'C:\Coding\msys64\mingw64\bin'
if (Test-Path (Join-Path $mingwBin 'g++.exe')) {
    $env:PATH = "$mingwBin;$env:PATH"
}

cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build build -- -j8
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exe = Join-Path $root 'build\work4-tools.exe'
if (-not (Test-Path $exe)) { throw "构建失败：未找到 $exe" }

# 2) 创建桌面快捷方式
$desktop = [Environment]::GetFolderPath('Desktop')
$shell = New-Object -ComObject WScript.Shell
$lnkPath = Join-Path $desktop 'Work4 文件快搜.lnk'
$lnk = $shell.CreateShortcut($lnkPath)
$lnk.TargetPath = $exe
$lnk.WorkingDirectory = (Split-Path $exe)
$lnk.Description = 'Work4 文件快搜 - 多线程高速文件搜索工具'
$lnk.IconLocation = "$exe,0"
$lnk.Save()

Write-Host "构建完成: $exe"
Write-Host "快捷方式: $lnkPath"
