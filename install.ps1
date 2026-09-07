# Install gdscript-lsp from a GitHub release.
#
#   irm https://raw.githubusercontent.com/brohd11/gdscript-standalone-lsp/main/install.ps1 | iex
#
# Environment:
#   $env:BIN_DIR = "$HOME\.local\bin"  executable destination
#   $env:VERSION = 'v1.2.3'            release to install (default: latest stable)
#
# `iex` cannot pass arguments. To use flags, invoke the downloaded script block:
#   & ([scriptblock]::Create((irm <url>))) -NoModifyPath

$Repo   = 'brohd11/gdscript-standalone-lsp'
$Binary = 'gdscript-lsp'

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$FromFile = [bool]$MyInvocation.MyCommand.Path

function Write-Err([string]$Message) {
    [Console]::Error.WriteLine("error: $Message")
}

function Show-Help {
    Write-Host @"
install $Binary into `$env:BIN_DIR (default: %USERPROFILE%\.local\bin)

  `$env:BIN_DIR = '<prefix>\bin'  executable destination
  `$env:VERSION = '<tag>'         release to install (default: latest stable)

Flags (use the script-block form because iex cannot receive arguments):
  & ([scriptblock]::Create((irm https://raw.githubusercontent.com/$Repo/main/install.ps1))) -NoModifyPath

  -ModifyPath      update the user PATH without prompting
  -NoModifyPath    never modify the user PATH

The bundled API metadata is installed under <prefix>\share\gdscript-lsp.
"@
}

function Read-Options([string[]]$Arguments) {
    $options = @{ PathMode = 'auto'; Help = $false }
    foreach ($argument in $Arguments) {
        switch -Regex ($argument) {
            '^(--no-modify-path|-+NoModifyPath)$' { $options.PathMode = 'never' }
            '^(--modify-path|-+ModifyPath)$'      { $options.PathMode = 'always' }
            '^(-h|-+help|/\?)$'                   { $options.Help = $true }
            default { throw "unknown option: $argument" }
        }
    }
    return $options
}

function Get-Target {
    $architecture = if ($env:PROCESSOR_ARCHITEW6432) {
        $env:PROCESSOR_ARCHITEW6432
    } else {
        $env:PROCESSOR_ARCHITECTURE
    }

    switch ($architecture) {
        'AMD64' { return 'windows-x64' }
        'ARM64' {
            Write-Host 'no native Windows ARM64 build is published; installing the x64 build under emulation'
            return 'windows-x64'
        }
        default { throw "unsupported Windows architecture: $architecture" }
    }
}

function Get-ReleaseUrl([string]$Version) {
    if ($Version -eq 'latest') {
        return "https://github.com/$Repo/releases/latest/download"
    }
    return "https://github.com/$Repo/releases/download/$Version"
}

function Test-OnPathValue([string]$PathValue, [string]$Directory) {
    if (-not $PathValue) { return $false }
    $wanted = ([Environment]::ExpandEnvironmentVariables($Directory)).TrimEnd('\', '/')
    foreach ($entry in $PathValue -split ';') {
        if (-not $entry) { continue }
        $present = ([Environment]::ExpandEnvironmentVariables($entry)).TrimEnd('\', '/')
        if ($present -ieq $wanted) { return $true }
    }
    return $false
}

function Send-EnvironmentChange {
    if (-not ('GdscriptLspInstaller.Native' -as [type])) {
        Add-Type -Namespace GdscriptLspInstaller -Name Native -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
public static extern IntPtr SendMessageTimeout(
    IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
    uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
'@
    }
    $result = [UIntPtr]::Zero
    [void][GdscriptLspInstaller.Native]::SendMessageTimeout(
        [IntPtr]0xffff, 0x1a, [UIntPtr]::Zero, 'Environment', 0x2, 5000, [ref]$result)
}

function Test-CanPrompt {
    try {
        return [Environment]::UserInteractive -and -not [Console]::IsInputRedirected
    } catch {
        return $false
    }
}

function Add-ToUserPath([string]$DirectoryRaw) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    if (-not $key) { throw 'cannot open HKCU:\Environment' }
    try {
        $current = [string]$key.GetValue(
            'Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if (Test-OnPathValue $current $DirectoryRaw) {
            Write-Host "your user PATH already references $DirectoryRaw -- leaving it alone"
            return $false
        }
        $trimmed = $current.TrimEnd(';')
        $updated = if ($trimmed) { "$trimmed;$DirectoryRaw" } else { $DirectoryRaw }
        $key.SetValue('Path', $updated, [Microsoft.Win32.RegistryValueKind]::ExpandString)
    } finally {
        $key.Close()
    }
    Send-EnvironmentChange
    Write-Host "added $DirectoryRaw to your user PATH"
    return $true
}

function Set-PathEntry([hashtable]$Options, [string]$BinDirRaw, [string]$BinDir) {
    if (Test-OnPathValue $env:Path $BinDir) { return }

    Write-Host ''
    Write-Host "$BinDir is not on your PATH, so '$Binary' will not be runnable by name."
    $sessionLine = "`$env:Path += ';$BinDir'"
    $added = $false

    if ($Options.PathMode -eq 'never') {
        Write-Host 'add it for this session with:'
        Write-Host "  $sessionLine"
        Write-Host "to make it permanent, re-run with -ModifyPath or add $BinDirRaw to your user PATH."
        return
    }

    if ($Options.PathMode -eq 'always') {
        $added = Add-ToUserPath $BinDirRaw
    } elseif (Test-CanPrompt) {
        $reply = Read-Host 'Add it to your user PATH? [y/N]'
        if ($reply -imatch '^y(es)?$') {
            $added = Add-ToUserPath $BinDirRaw
        } else {
            Write-Host 'skipped.'
        }
    } else {
        Write-Host 'run this installer with -ModifyPath to add it, or add it yourself.'
    }

    if ($added) {
        Write-Host 'open a new terminal, or run this in the current one:'
    } else {
        Write-Host 'to use it in this session:'
    }
    Write-Host "  $sessionLine"
}

function Install-StagedFile([string]$Source, [string]$Destination, [bool]$Executable) {
    $directory = Split-Path -Parent $Destination
    $leaf = Split-Path -Leaf $Destination
    $staged = Join-Path $directory (".$leaf." + [Guid]::NewGuid().ToString('N') + '.tmp')
    $old = "$Destination.old"

    Copy-Item -LiteralPath $Source -Destination $staged
    if ($Executable) {
        Unblock-File -LiteralPath $staged -ErrorAction SilentlyContinue
    }

    Remove-Item -LiteralPath $old -Force -ErrorAction SilentlyContinue
    $hadOld = Test-Path -LiteralPath $Destination
    if ($hadOld) {
        Move-Item -LiteralPath $Destination -Destination $old -Force
    }
    try {
        Move-Item -LiteralPath $staged -Destination $Destination -Force
    } catch {
        Remove-Item -LiteralPath $staged -Force -ErrorAction SilentlyContinue
        if ($hadOld -and -not (Test-Path -LiteralPath $Destination)) {
            Move-Item -LiteralPath $old -Destination $Destination -Force
        }
        throw
    }
    Remove-Item -LiteralPath $old -Force -ErrorAction SilentlyContinue
}

function Invoke-Install(
    [hashtable]$Options,
    [string]$BinDirRaw,
    [string]$BinDir,
    [string]$Version
) {
    $target = Get-Target
    $asset = "$Binary-$target.zip"
    $releaseUrl = Get-ReleaseUrl $Version
    $prefix = Split-Path -Parent $BinDir
    $dataDir = Join-Path $prefix 'share\gdscript-lsp'
    $docDir = Join-Path $prefix 'share\doc\gdscript-lsp'

    Write-Host "downloading $Binary ($target, $Version)"
    $temporary = Join-Path ([IO.Path]::GetTempPath()) ("install-$Binary-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporary | Out-Null
    try {
        $archive = Join-Path $temporary $asset
        $checksums = Join-Path $temporary 'SHA256SUMS'
        try {
            [Net.ServicePointManager]::SecurityProtocol =
                [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        } catch { }

        try {
            Invoke-WebRequest -Uri "$releaseUrl/$asset" -OutFile $archive -UseBasicParsing
            Invoke-WebRequest -Uri "$releaseUrl/SHA256SUMS" -OutFile $checksums -UseBasicParsing
        } catch {
            throw "download failed from $releaseUrl
  (check https://github.com/$Repo/releases for available versions)"
        }

        $pattern = '^(?<hash>[0-9A-Fa-f]{64})\s+\*?' + [regex]::Escape($asset) + '$'
        $entries = @(Get-Content -LiteralPath $checksums | ForEach-Object {
            if ($_ -match $pattern) { $Matches.hash }
        })
        if ($entries.Count -ne 1) {
            throw "SHA256SUMS does not contain exactly one entry for $asset"
        }
        $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        if ($actual -ine $entries[0]) {
            throw "checksum verification failed for $asset"
        }

        $unpacked = Join-Path $temporary 'unpacked'
        Expand-Archive -LiteralPath $archive -DestinationPath $unpacked
        $package = Join-Path $unpacked "$Binary-$target"
        $sourceBinary = Join-Path $package "bin\$Binary.exe"
        $sourceApi = Join-Path $package 'share\gdscript-lsp\godot-4.6-extension-api.json'
        $sourceReadme = Join-Path $package 'share\doc\gdscript-lsp\README.md'
        $sourceNotices = Join-Path $package 'share\doc\gdscript-lsp\THIRD_PARTY_NOTICES.md'
        if (-not (Test-Path -LiteralPath $sourceBinary -PathType Leaf)) {
            throw "archive did not contain bin\$Binary.exe"
        }
        if (-not (Test-Path -LiteralPath $sourceApi -PathType Leaf)) {
            throw 'archive did not contain the bundled Godot API metadata'
        }
        if (-not (Test-Path -LiteralPath $sourceReadme -PathType Leaf)) {
            throw 'archive did not contain README.md'
        }
        if (-not (Test-Path -LiteralPath $sourceNotices -PathType Leaf)) {
            throw 'archive did not contain THIRD_PARTY_NOTICES.md'
        }

        try {
            New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
            New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
            New-Item -ItemType Directory -Path $docDir -Force | Out-Null
        } catch {
            throw "cannot create the install directories (set `$env:BIN_DIR to a directory you own)"
        }

        Install-StagedFile $sourceReadme (Join-Path $docDir 'README.md') $false
        Install-StagedFile $sourceNotices (Join-Path $docDir 'THIRD_PARTY_NOTICES.md') $false
        Install-StagedFile $sourceApi (Join-Path $dataDir 'godot-4.6-extension-api.json') $false
        Install-StagedFile $sourceBinary (Join-Path $BinDir "$Binary.exe") $true

        Write-Host "installed -> $(Join-Path $BinDir "$Binary.exe")"
        Write-Host "metadata  -> $(Join-Path $dataDir 'godot-4.6-extension-api.json')"
    } finally {
        Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
    }

    Set-PathEntry $Options $BinDirRaw $BinDir
}

$code = 0
try {
    $options = Read-Options $args
    if ($options.Help) {
        Show-Help
    } else {
        $binDirRaw = if ($env:BIN_DIR) { $env:BIN_DIR } else { '%USERPROFILE%\.local\bin' }
        $expanded = [Environment]::ExpandEnvironmentVariables($binDirRaw)
        if ($expanded -like '*%*') {
            throw 'USERPROFILE is not set; set $env:BIN_DIR explicitly'
        }
        $binDir = [IO.Path]::GetFullPath($expanded)
        if (-not [IO.Path]::IsPathRooted($expanded)) {
            $binDirRaw = $binDir
        }
        $version = if ($env:VERSION) { $env:VERSION } else { 'latest' }
        Invoke-Install $options $binDirRaw $binDir $version
    }
} catch {
    Write-Err $_.Exception.Message
    $code = 1
}

if ($FromFile) {
    exit $code
} elseif ($code -ne 0) {
    throw "$Binary installation failed"
}
