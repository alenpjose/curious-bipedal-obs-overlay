[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Resolve-Path "$PSScriptRoot/../.."
$BuildSpec = Get-Content "$ProjectRoot/buildspec.json" -Raw | ConvertFrom-Json
$Plugin = $BuildSpec.name
$Version = $BuildSpec.version
$Dll = "$ProjectRoot/release/$Configuration/$Plugin/bin/64bit/$Plugin.dll"
$Zip = "$ProjectRoot/release/$Plugin-$Version-windows-x64.zip"
$Installer = "$ProjectRoot/release/installer/Curious-Bipedal-OBS-Overlay-Setup-$Version-windows-x64.exe"

foreach ($Path in @($Dll, $Zip, $Installer)) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing package output: $Path"
    }
    if ((Get-Item -LiteralPath $Path).Length -eq 0) {
        throw "Empty package output: $Path"
    }
}

$Stream = [System.IO.File]::OpenRead($Dll)
try {
    $Reader = [System.IO.BinaryReader]::new($Stream)
    if ($Reader.ReadUInt16() -ne 0x5A4D) { throw 'Plugin DLL is not a PE file.' }
    $Stream.Position = 0x3C
    $PeOffset = $Reader.ReadUInt32()
    $Stream.Position = $PeOffset
    if ($Reader.ReadUInt32() -ne 0x00004550) { throw 'Plugin DLL has an invalid PE signature.' }
    if ($Reader.ReadUInt16() -ne 0x8664) { throw 'Plugin DLL is not Windows x64.' }
} finally {
    $Stream.Dispose()
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$Archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
try {
    $RequiredEntries = @(
        "$Plugin/bin/64bit/$Plugin.dll",
        "$Plugin/data/locale/en-US.ini",
        "$Plugin/data/assets/curious-bipedal-primary-glyph-safe.png",
        "$Plugin/LICENSE",
        "$Plugin/ASSET-NOTICE.md",
        "$Plugin/README.md"
    )
    $Names = $Archive.Entries.FullName -replace '\\', '/'
    foreach ($Entry in $RequiredEntries) {
        if ($Entry -notin $Names) { throw "Portable ZIP is missing $Entry" }
    }
    if ($Names -match '\.pdb$') { throw 'Portable ZIP must not contain debug symbols.' }
} finally {
    $Archive.Dispose()
}

Write-Host "Verified x64 DLL, portable ZIP structure, and Inno Setup installer output."
