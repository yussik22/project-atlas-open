param(
    [string]$BuildDirectory = 'build-public',
    [string]$Version = '0.11.1'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') {
    throw 'Version must contain only letters, digits, dots, underscores or hyphens.'
}

$atlasRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$atlasBuild = (Resolve-Path -LiteralPath (Join-Path $atlasRoot $BuildDirectory)).Path
$atlasDist = Join-Path $atlasRoot 'dist'
$atlasBinaryName = "ProjectAtlas-$Version-win-x64"
$atlasSourceName = "ProjectAtlas-$Version-source"
$atlasBinaryStage = Join-Path $atlasDist $atlasBinaryName
$atlasSourceStage = Join-Path $atlasDist $atlasSourceName
$atlasBinaryZip = Join-Path $atlasDist "$atlasBinaryName.zip"
$atlasSourceZip = Join-Path $atlasDist "$atlasSourceName.zip"

foreach ($atlasTarget in @($atlasBinaryStage, $atlasSourceStage,
                            $atlasBinaryZip, $atlasSourceZip)) {
    if (Test-Path -LiteralPath $atlasTarget) {
        throw "Refusing to overwrite existing public release artifact: $atlasTarget"
    }
}

foreach ($atlasFile in @('project_atlas.exe', 'project_atlas_gui.exe', 'libclang.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $atlasBuild $atlasFile) -PathType Leaf)) {
        throw "Release build is missing $atlasFile in $atlasBuild"
    }
}

New-Item -ItemType Directory -Path $atlasBinaryStage, $atlasSourceStage | Out-Null

function Copy-AtlasFile([string]$atlasRelative, [string]$atlasDestinationRoot) {
    $atlasOrigin = Join-Path $atlasRoot $atlasRelative
    if (-not (Test-Path -LiteralPath $atlasOrigin -PathType Leaf)) {
        throw "Required source file is missing: $atlasRelative"
    }
    $atlasTarget = Join-Path $atlasDestinationRoot $atlasRelative
    $atlasParent = Split-Path -Parent $atlasTarget
    New-Item -ItemType Directory -Path $atlasParent -Force | Out-Null
    Copy-Item -LiteralPath $atlasOrigin -Destination $atlasTarget
}

foreach ($atlasFile in @('project_atlas.exe', 'project_atlas_gui.exe', 'libclang.dll')) {
    Copy-Item -LiteralPath (Join-Path $atlasBuild $atlasFile) -Destination $atlasBinaryStage
}
Copy-AtlasFile 'LICENSE' $atlasBinaryStage
Copy-AtlasFile 'THIRD_PARTY_NOTICES.md' $atlasBinaryStage
Copy-Item -LiteralPath (Join-Path $atlasRoot 'third_party/llvm/LICENSE.txt') `
    -Destination (Join-Path $atlasBinaryStage 'LLVM-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $atlasRoot 'PUBLIC_RELEASE.md') `
    -Destination (Join-Path $atlasBinaryStage 'README.md')
foreach ($atlasFile in Get-ChildItem -LiteralPath (Join-Path $atlasRoot 'examples') -File) {
    if ($atlasFile.Extension -in @('.facts', '.trace')) {
        Copy-AtlasFile ("examples/" + $atlasFile.Name) $atlasBinaryStage
    }
}

# An allowlist excludes Git history, local configuration, cards, build output,
# package caches, reports, symbols, and any future file not reviewed here.
$atlasRootFiles = @(
    '.gitignore', 'AGENTS.md', 'CMakeLists.txt', 'LICENSE', 'README.md',
    'README.uk.md',
    'PUBLIC_RELEASE.md', 'THIRD_PARTY_NOTICES.md',
    'package.json', 'package-lock.json', '.codex/config.example.toml',
    'third_party/llvm/LICENSE.txt'
)
foreach ($atlasFile in $atlasRootFiles) {
    Copy-AtlasFile $atlasFile $atlasSourceStage
}
foreach ($atlasSpec in @(
    @{ Directory = 'include'; Extensions = @('.h', '.hpp', '.hxx', '.inl') },
    @{ Directory = 'src'; Extensions = @('.cpp') },
    @{ Directory = 'tests'; Extensions = @('.cpp') },
    @{ Directory = 'examples'; Extensions = @('.cpp', '.facts', '.trace') },
    @{ Directory = 'tools'; Extensions = @('.mjs', '.ps1') },
    @{ Directory = 'knowledge'; Extensions = @('.md') }
)) {
    $atlasFolder = Join-Path $atlasRoot $atlasSpec.Directory
    foreach ($atlasFile in Get-ChildItem -LiteralPath $atlasFolder -File -Recurse) {
        if ($atlasFile.Extension -notin $atlasSpec.Extensions) { continue }
        if ($atlasFile.Name.EndsWith('.agent.md')) { continue }
        $atlasRootPrefix = $atlasRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
        if (-not $atlasFile.FullName.StartsWith($atlasRootPrefix,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Source file is outside the Atlas checkout: $($atlasFile.FullName)"
        }
        $atlasRelative = $atlasFile.FullName.Substring($atlasRootPrefix.Length)
        Copy-AtlasFile $atlasRelative $atlasSourceStage
    }
}

$atlasUnixHome = '/ho' + 'me/'
$atlasMacHome = '/Us' + 'ers/'
$atlasPersonalPattern = '(?i)(?:[A-Z]:[\\/][^\\/\r\n]+[\\/]|' +
    $atlasUnixHome + '[^/]+/|' + $atlasMacHome + '[^/]+/|' +
    'ghp_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|' +
    '-----BEGIN [A-Z ]*PRIVATE KEY-----)'
$atlasLeaks = Get-ChildItem -LiteralPath $atlasSourceStage -Recurse -File |
    Select-String -Pattern $atlasPersonalPattern -List
if ($atlasLeaks) {
    $atlasNames = ($atlasLeaks | ForEach-Object { $_.Path }) -join ', '
    throw "Potential personal path or credential in source package: $atlasNames"
}

foreach ($atlasFile in @('project_atlas.exe', 'project_atlas_gui.exe')) {
    $atlasBytes = [IO.File]::ReadAllBytes((Join-Path $atlasBinaryStage $atlasFile))
    $atlasAscii = [Text.Encoding]::ASCII.GetString($atlasBytes)
    if ($atlasAscii -match '(?i)(?:MSVCP140D|VCRUNTIME140D|ucrtbased)\.dll') {
        throw "Debug C++ runtime dependency found in $atlasFile"
    }
    if ($atlasAscii.Contains($atlasRoot) -or
        $atlasAscii.Contains($atlasRoot.Replace('\', '/'))) {
        throw "Developer checkout path found in $atlasFile"
    }
}

$atlasChecksums = foreach ($atlasFile in @('project_atlas.exe',
        'project_atlas_gui.exe', 'libclang.dll', 'LICENSE', 'LLVM-LICENSE.txt')) {
    $atlasHash = (Get-FileHash -Algorithm SHA256 -LiteralPath `
        (Join-Path $atlasBinaryStage $atlasFile)).Hash.ToLowerInvariant()
    "$atlasHash  $atlasFile"
}
Set-Content -LiteralPath (Join-Path $atlasBinaryStage 'SHA256SUMS.txt') `
    -Value $atlasChecksums -Encoding utf8

Compress-Archive -Path $atlasBinaryStage -DestinationPath $atlasBinaryZip `
    -CompressionLevel Optimal
Compress-Archive -Path $atlasSourceStage -DestinationPath $atlasSourceZip `
    -CompressionLevel Optimal

Write-Output "Binary archive: $atlasBinaryZip"
Write-Output "Source archive: $atlasSourceZip"
Write-Output "Source files: $((Get-ChildItem $atlasSourceStage -Recurse -File).Count)"
