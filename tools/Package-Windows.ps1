[CmdletBinding()]
param(
    [string]$VcpkgRoot,
    [string]$MiniRtcDir,
    [string]$GhosttyVtDir,
    [string]$BuildDirectory,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).ProviderPath

function Get-FullPath([string]$Path) {
    $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Read-CMakeCache([string]$BuildDirectory) {
    $settings = @{}
    $cacheFile = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cacheFile -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $cacheFile) {
            if ($line -match '^([A-Za-z0-9_-]+):[^=]+=(.*)$') {
                $settings[$Matches[1]] = $Matches[2]
            }
        }
    }
    return $settings
}

function Enter-BuildEnvironment($Cache) {
    if ($env:VSCMD_ARG_TGT_ARCH -notin @('x64', 'amd64') -or
        !(Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue)) {
        $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
        $vswhere = Join-Path $installer 'vswhere.exe'
        if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) {
            throw 'Install Visual Studio 2022 or Build Tools with Desktop development with C++.'
        }
        $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0 -or !$vs) { throw 'No Visual Studio installation with MSVC x64 was found.' }
        $env:PATH = "$installer;$env:PATH"
        Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
        Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    }

    # Ninja keeps the compiler path in its cache. Use that compiler's headers
    # and libraries even when Visual Studio's default toolset is different.
    $cachedCompiler = $Cache['CMAKE_CXX_COMPILER']
    if ($cachedCompiler) {
        if (!(Test-Path -LiteralPath $cachedCompiler -PathType Leaf)) {
            throw "The compiler in the CMake cache is missing: $cachedCompiler. Use -BuildDirectory to configure a new build."
        }
        $compilerBin = Split-Path -Parent $cachedCompiler
        $toolset = Get-FullPath (Join-Path $compilerBin '..\..\..')
        $toolsetInclude = Join-Path $toolset 'include'
        $toolsetLib = Join-Path $toolset 'lib\x64'
        if ((Test-Path -LiteralPath $toolsetInclude -PathType Container) -and
            (Test-Path -LiteralPath $toolsetLib -PathType Container)) {
            $env:PATH = "$compilerBin;$env:PATH"
            $env:INCLUDE = "$toolsetInclude;$env:INCLUDE"
            $env:LIB = "$toolsetLib;$env:LIB"
        }
    }
}

function Invoke-CMake([string[]]$Arguments) {
    & $cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake $($Arguments[0]) failed with exit code $LASTEXITCODE."
    }
}

$scratch = $null
try {
    $build = if ($BuildDirectory) { Get-FullPath $BuildDirectory } else { Join-Path $repo 'out\build\windows-x64-qt' }
    $output = if ($OutputDirectory) { Get-FullPath $OutputDirectory } else { Join-Path $repo 'dist\packages' }
    $cache = Read-CMakeCache $build
    if ($cache['CMAKE_HOME_DIRECTORY'] -and (Get-FullPath $cache['CMAKE_HOME_DIRECTORY']) -ne $repo) {
        throw "The CMake cache belongs to a different source directory. Use -BuildDirectory to configure a new build."
    }

    if (!$VcpkgRoot) {
        $cachedVcpkg = if ($cache['CMAKE_TOOLCHAIN_FILE'] -match '^(.*)[/\\]scripts[/\\]buildsystems[/\\]vcpkg\.cmake$') {
            $Matches[1]
        }
        foreach ($candidate in @($cachedVcpkg, $env:VCPKG_ROOT, (Join-Path $repo 'out\deps\vcpkg'))) {
            if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'scripts\buildsystems\vcpkg.cmake') -PathType Leaf)) {
                $VcpkgRoot = $candidate
                break
            }
        }
    }
    if (!$VcpkgRoot) { throw 'Set VCPKG_ROOT or pass -VcpkgRoot. See the Qt 6 build instructions in README.md.' }
    $vcpkgDirectory = Get-FullPath $VcpkgRoot
    $toolchain = Join-Path $vcpkgDirectory 'scripts\buildsystems\vcpkg.cmake'
    if (!(Test-Path -LiteralPath $toolchain -PathType Leaf)) { throw "vcpkg toolchain not found: $toolchain" }
    if ($cache['CMAKE_TOOLCHAIN_FILE'] -and (Get-FullPath $cache['CMAKE_TOOLCHAIN_FILE']) -ne $toolchain) {
        throw 'The existing build uses a different CMake toolchain. Pass a new -BuildDirectory when changing vcpkg.'
    }

    if (!$MiniRtcDir) { $MiniRtcDir = $cache['P2PREMOTE_MINIRTC_DIR'] }
    if (!$MiniRtcDir) { $MiniRtcDir = Join-Path $repo 'out\minirtc' }
    $MiniRtcDir = Get-FullPath $MiniRtcDir
    foreach ($file in @('minirtc.h', 'p2premote_minirtc.dll')) {
        if (!(Test-Path -LiteralPath (Join-Path $MiniRtcDir $file) -PathType Leaf)) {
            throw "Missing $file in $MiniRtcDir. Run tools/minirtc_probe/Build.ps1 once or pass -MiniRtcDir."
        }
    }

    if (!$GhosttyVtDir) { $GhosttyVtDir = $cache['P2PREMOTE_GHOSTTY_VT_DIR'] }
    if (!$GhosttyVtDir) { $GhosttyVtDir = Join-Path $repo 'out\ghostty-vt\ghostty-vt-1.0.1-x86_64-windows-gnu' }
    $GhosttyVtDir = Get-FullPath $GhosttyVtDir
    if (!(Test-Path -LiteralPath (Join-Path $GhosttyVtDir 'lib\cmake\ghostty-vt\ghostty-vt-config.cmake') -PathType Leaf)) {
        throw "libghostty-vt was not found in $GhosttyVtDir. See README.md or pass -GhosttyVtDir."
    }

    Enter-BuildEnvironment $cache
    $env:VCPKG_ROOT = $vcpkgDirectory
    $cmakeCommand = Get-Command cmake.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$cmakeCommand) { throw 'CMake was not found. Install CMake and Ninja or the Visual Studio CMake tools for Windows.' }
    if (!(Get-Command ninja.exe -CommandType Application -ErrorAction SilentlyContinue) -and !$cache['CMAKE_MAKE_PROGRAM']) {
        throw 'Ninja was not found. Install Ninja or the Visual Studio CMake tools for Windows.'
    }
    $cmake = $cmakeCommand.Source

    Push-Location -LiteralPath $repo
    try {
        Write-Host '[1/3] Configuring and building Windows x64 Release'
        Invoke-CMake @('--preset', 'windows-x64-qt', '-B', $build,
            "-DP2PREMOTE_MINIRTC_DIR=$MiniRtcDir", "-DP2PREMOTE_GHOSTTY_VT_DIR=$GhosttyVtDir")
        Invoke-CMake @('--build', $build, '--config', 'Release', '--target', 'p2premote_qt', 'p2premote', 'p2premote_service')

        Write-Host '[2/3] Installing the clients and runtime dependencies'
        [void][IO.Directory]::CreateDirectory($output)
        $output = (Resolve-Path -LiteralPath $output).ProviderPath
        $scratchName = '.p2premote-package-' + [guid]::NewGuid().ToString('N')
        $scratch = Join-Path $output $scratchName
        $stage = Join-Path $scratch 'p2premote-qt-win64'
        [void][IO.Directory]::CreateDirectory($stage)
        Invoke-CMake @('--install', $build, '--config', 'Release', '--prefix', $stage)
        foreach ($file in @(
            'p2premote_qt.exe', 'p2premote.exe', 'p2premote_service.exe', 'p2premote_core.dll', 'p2premote_minirtc.dll',
            'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'platforms\qwindows.dll',
            'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'ghostty-vt.dll',
            'licenses\ghostty-vt.txt', 'lua-notice.html', 'plugins\remote-terminal\main.lua'
        )) {
            if (!(Test-Path -LiteralPath (Join-Path $stage $file) -PathType Leaf)) {
                throw "The portable installation is missing $file."
            }
        }

        Write-Host '[3/3] Creating the portable ZIP'
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $tempZip = Join-Path $scratch 'portable.zip'
        [IO.Compression.ZipFile]::CreateFromDirectory($stage, $tempZip, 'Optimal', $true)
        # Install into the preset's fixed app directory. Build the ZIP from the
        # clean stage above so local settings and extra plugins stay local.
        Invoke-CMake @('--install', $build, '--config', 'Release')
        $packageName = 'p2premote-qt-win64-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.zip'
        $package = Join-Path $output $packageName
        [IO.File]::Move($tempZip, $package)
        Write-Host "Latest application: $(Join-Path $repo 'dist\app\p2premote_qt.exe')"
        Write-Host "Portable ZIP: $package"
    } finally {
        Pop-Location
    }
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    if ($scratch -and (Test-Path -LiteralPath $scratch -PathType Container)) {
        $resolvedScratch = (Resolve-Path -LiteralPath $scratch).ProviderPath
        if ((Split-Path -Parent $resolvedScratch) -eq $output -and (Split-Path -Leaf $resolvedScratch) -eq $scratchName) {
            Remove-Item -LiteralPath $resolvedScratch -Recurse -Force -ErrorAction Continue
        }
    }
}
