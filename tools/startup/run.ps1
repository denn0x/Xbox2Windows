<#
.SYNOPSIS
Runs a console executable on the desktop stand-ins under the startup debugger (docs/startup-runtime.md).

.DESCRIPTION
Copies the stand-in DLLs from the build into -RunDir (a folder that holds the executable and the
package's other DLLs; prepare it yourself, never commit it), sets the permissive switches and starts
tools/startup/debug_launch.py. The run is offline: the debugger refuses sockets, NetBIOS and DNS.
Logs go to -LogDir; -LogDir.out holds the debugger's summary.

.EXAMPLE
.\tools\startup\run.ps1 -Exe D:\run\title.exe -DataDir G:\ -LogDir out\startup\run1 -Seconds 180 -Render -FakeUser
#>
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$DataDir,   # the package root the title opens its files from (its working directory)
    [Parameter(Mandatory)] [string]$LogDir,
    [int]$Seconds = 120,
    [string]$BuildDir = "out/build/release",
    [int]$MemoryGiB = 14,
    [int]$ContinueAsserts = 0,                 # main-image int3s to log and step over (title soft asserts)
    [string]$InputScript = "",                 # X2W_INPUT_SCRIPT, e.g. "4@70" = A at 70 s
    [string]$StoragePath = "",                 # X2W_STORAGE_PATH: persistent local storage root (a trailing \ is added when missing)
    [string]$ShaderCache = "out/startup/shader_cache",
    [switch]$Render,                           # draw through translated pipelines
    [switch]$FakeUser,                         # answer the user API for one local user
    [switch]$GpuMirror,                        # map CPU-view pages into the paired GPU-view reservation
    [switch]$HostCpu                           # rewrite instructions the host processor lacks (X2W_HOST_CPU)
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$runDir = Split-Path -Parent (Resolve-Path $Exe)
foreach ($dll in "xmem", "d3d12_xs", "AcpHal", "XCRDAPI", "chad", "xapu", "xg_xs", "GameInput") {
    Copy-Item (Join-Path $repo "$BuildDir\$dll.dll") $runDir -Force
}
$env:X2W_AUDIO_PERMISSIVE = "1"; $env:X2W_D3D12_DEVICE_NOOP = "1"; $env:X2W_D3D12_LIST_NOOP = "1"
$env:X2W_D3D12_PERMISSIVE = "1"; $env:X2W_XMEM_PERMISSIVE = "1"; $env:X2W_RUNTIME_PROXY = "1"
$config = Join-Path $DataDir "MicrosoftGame.config"
if (Test-Path $config) {
    $text = Get-Content $config -Raw
    if ($text -match "<TitleId>([^<]*)") { $env:X2W_TITLE_ID = $Matches[1] }
    if ($text -match "<SizeMB>([^<]*)") { $env:X2W_STORAGE_MB = $Matches[1] }
}
if ($StoragePath) { $env:X2W_STORAGE_PATH = $StoragePath }
if ($InputScript) { $env:X2W_INPUT_SCRIPT = $InputScript }
if ($FakeUser) { $env:X2W_FAKE_USER = "1" }
if ($GpuMirror) { $env:X2W_XMEM_GPU_MIRROR = "1" }
if ($HostCpu) { $env:X2W_HOST_CPU = "1" }
if ($Render) {
    New-Item -ItemType Directory -Force $ShaderCache | Out-Null
    $env:X2W_D3D12_RENDER = "1"; $env:X2W_DRAW_NULL_IDENTITY = "1"
    $env:X2W_SHADER_CACHE = (Resolve-Path $ShaderCache).Path
}
New-Item -ItemType Directory -Force $LogDir | Out-Null
python -B (Join-Path $PSScriptRoot "debug_launch.py") $Exe $DataDir $LogDir --timeout $Seconds --memory-gib $MemoryGiB `
    --trace-files 1 --continue-asserts $ContinueAsserts *> "$LogDir.out"
Get-Content "$LogDir.out" -TotalCount 5
