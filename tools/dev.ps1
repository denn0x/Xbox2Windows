$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installations = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
if (-not $installations) {
    # CMake and Ninja can come from a package manager instead of the VS CMake
    # component; the instance then only needs the x64 C++ toolset. An instance
    # with only that toolset is "incomplete" to vswhere, so the fallback also
    # lists incomplete installations (-all, newest first).
    $installations = & $vswhere -all -sort -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
# Prefer the newest instance that actually ships the developer shell module,
# so an incomplete installation without a usable environment is not selected.
$installation = $null
foreach ($candidate in $installations) {
    if (Test-Path -LiteralPath (Join-Path $candidate 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')) {
        $installation = $candidate
        break
    }
}
if (-not $installation) {
    throw 'Install Visual Studio Build Tools with x64 C++ and CMake tools.'
}
# Enter-VsDevShell with VsDevCmd arguments selects the x64 host and target on
# VS 2019 as well as VS 2022. The Launch-VsDevShell.ps1 that VS 2019 16.11
# ships has no -Arch or -HostArch parameter and would start an x86 shell.
Import-Module (Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
# Keep CMake's detected /showIncludes prefix in the same encoding as MSVC.
chcp.com 65001 | Out-Null

$bin = Join-Path $PSScriptRoot '..\out\tools\xbdepends\build\bin'
if (Test-Path -LiteralPath (Join-Path $bin 'xbdepends.exe')) {
    $bin = (Resolve-Path -LiteralPath $bin).Path
    if ($bin -notin ($env:Path -split ';')) { $env:Path = "$bin;$env:Path" }
}

# Opt-in compiler cache. With X2W_USE_SCCACHE=1 and sccache on PATH, the global
# $X2wSccacheCmakeArgs holds the extra configure arguments; pass them as
# "cmake --preset dev @X2wSccacheCmakeArgs". Embedded debug information (/Z7)
# lets sccache cache Debug objects; CMP0141 must be NEW for CMake to honor it.
# If sccache is missing, the arguments clear both launchers so that a build
# directory configured earlier with sccache stops calling it. Without the
# opt-in the variable is removed and every command line stays as it was.
# x2w-sccache-begin
Remove-Variable -Name X2wSccacheCmakeArgs -Scope Global -ErrorAction SilentlyContinue
if ($env:X2W_USE_SCCACHE -eq '1') {
    if (Get-Command sccache -CommandType Application -ErrorAction SilentlyContinue) {
        $global:X2wSccacheCmakeArgs = @(
            '-DCMAKE_C_COMPILER_LAUNCHER=sccache',
            '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache',
            '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded',
            '-DCMAKE_POLICY_DEFAULT_CMP0141=NEW'
        )
    } else {
        Write-Warning 'X2W_USE_SCCACHE=1 but sccache is not on PATH; configuring without a compiler launcher.'
        $global:X2wSccacheCmakeArgs = @(
            '-DCMAKE_C_COMPILER_LAUNCHER=',
            '-DCMAKE_CXX_COMPILER_LAUNCHER='
        )
    }
}
# x2w-sccache-end
