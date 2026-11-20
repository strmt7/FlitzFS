# Input: resource-qualified Windows tool process. Output: normalized SDK path
# and provenance; optional CI environment transport. Read MSI tables/cabinets
# only; an explicit driver evidence request stages bytes without installing them.
[CmdletBinding()]
param([string]$OutputRoot,
    [ValidateSet('Stable','Candidate','Compatibility21','RetirementReference')][string]$Profile = 'Stable',
    [switch]$IncludeDriverEvidence)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'This dependency extractor uses native Windows MSI table APIs.'
}
$taskStageParent = if ($OutputRoot) { $OutputRoot } elseif ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$taskSourceCommit = $env:GITHUB_SHA
if (-not $taskSourceCommit) {
    $taskSourceCommit = git -C (Join-Path $PSScriptRoot '../..') rev-parse HEAD
    if ($LASTEXITCODE) { throw 'Cannot determine the source commit.' }
}
if ($taskSourceCommit -notmatch '^[0-9a-f]{40}$') { throw 'Exact source identity is required.' }

$lockName = switch ($Profile) {
    'Compatibility21' { 'winfsp-stable-sdk-lock.json' }
    'RetirementReference' { 'winfsp-retirement-sdk-lock.json' }
    default { 'winfsp-sdk-lock.json' }
}
$lockPath = Join-Path (Split-Path -Parent $PSScriptRoot) $lockName
if ((Get-Item -LiteralPath $lockPath).Length -gt 16384) { throw 'WinFsp lock budget.' }
$recipe = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
if ($recipe.schema -ne 1 -or $recipe.repository -ne 'winfsp/winfsp' -or
    $recipe.tag -notmatch '^v[0-9]+(?:\.[0-9]+){1,3}(?:B[0-9]+)?$' -or
    $recipe.msi_name -notmatch '^winfsp-[0-9]+\.[0-9]+\.[0-9]+\.msi$' -or
    $recipe.sha256 -notmatch '^[0-9a-f]{64}$' -or $recipe.source_commit -notmatch '^[0-9a-f]{40}$' -or
    $recipe.bytes -le 0 -or
    $recipe.url -cne ("https://github.com/winfsp/winfsp/releases/download/" + $recipe.tag + "/" + $recipe.msi_name)) {
    throw 'Invalid source-bound WinFsp SDK lock.'
}

# Windows PowerShell may not have loaded the system HTTP assembly.
Add-Type -AssemblyName System.Net.Http
function Get-SdkSha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}
$base = [IO.Path]::GetFullPath($taskStageParent)
$drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($base))
if ($drive.AvailableFreeSpace - 1GB -lt 2 * $recipe.bytes) { throw 'Insufficient temporary storage headroom for MSI and cabinet.' }
$root = Join-Path $base ('flitzfs-sdk-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $root) { throw 'Fresh SDK staging directory already exists.' }
[IO.Directory]::CreateDirectory($root) | Out-Null
$msi = Join-Path $root 'winfsp.msi'
$url = $recipe.url
$expected = $recipe.sha256
# Limit the entire response to the pinned asset length and 60 seconds.
$cancel = [Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(60))
$client = [Net.Http.HttpClient]::new()
$response = $null; $inputStream = $null; $outputStream = $null
try {
    $response = $client.GetAsync($url, [Net.Http.HttpCompletionOption]::ResponseHeadersRead, $cancel.Token).GetAwaiter().GetResult()
    $response.EnsureSuccessStatusCode() | Out-Null
    if ($response.Content.Headers.ContentLength -ne $recipe.bytes) { throw 'SDK download length differs from official pinned asset.' }
    $inputStream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
    $outputStream = [IO.File]::Open($msi, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $buffer = [byte[]]::new(65536)
    $total = 0L
    while (($n = $inputStream.ReadAsync($buffer, 0, $buffer.Length, $cancel.Token).GetAwaiter().GetResult()) -gt 0) {
        $total += $n
        if ($total -gt $recipe.bytes) { throw 'SDK download exceeds pinned size.' }
        $outputStream.Write($buffer, 0, $n)
    }
} finally {
    if ($outputStream) { $outputStream.Dispose() }
    if ($inputStream) { $inputStream.Dispose() }
    if ($response) { $response.Dispose() }
    $client.Dispose(); $cancel.Dispose()
}
if ($total -ne $recipe.bytes -or (Get-SdkSha256 $msi) -ne $expected) {
    throw 'WinFsp SDK hash does not match the pinned official release.'
}
# Windows Installer database mode 0 is read-only. No install/custom actions run.
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.OpenDatabase($msi, 0)
$view = $null
$wanted = @{
    'winfsp.h' = 'include/winfsp/winfsp.h'
    'fsctl.h' = 'include/winfsp/fsctl.h'
    'winfsp-x64.lib' = 'lib/winfsp-x64.lib'
    'winfsp-x64.dll' = 'bin/winfsp-x64.dll'
}
# Component identities are selected from the pinned package. The
# optional fsext library has the same basename but is NOT the public SDK library.
$components = @{
    'winfsp.h' = 'C.winfsp.h'
    'fsctl.h' = 'C.fsctl.h'
    'winfsp-x64.lib' = 'C.winfsp_x64.lib'
    'winfsp-x64.dll' = 'C.winfsp_x64.dll.x64'
}
if ($IncludeDriverEvidence) {
    $wanted['winfsp-x64.sys'] = 'driver/winfsp-x64.sys'
    $components['winfsp-x64.sys'] = 'C.winfsp_x64.sys'
}
$sdk = Join-Path $root 'sdk'
$copied = @{}
try {
    # Embedded cabinet streams are database bytes, not an installation. Native
    # ReadStream format 1 returns one byte per BSTR character; strict Latin-1
    # conversion preserves NUL/high bytes and rejects unexpected characters.
    $encoding = [Text.Encoding]::GetEncoding(28591, [Text.EncoderExceptionFallback]::new(), [Text.DecoderExceptionFallback]::new())
    $cabinets = [Collections.Generic.List[object]]::new()
    $media = $database.OpenView('SELECT `Cabinet`, `LastSequence` FROM `Media` ORDER BY `DiskId`')
    try {
        $media.Execute()
        while ($row = $media.Fetch()) {
            try {
                $name = $row.StringData(1)
                $lastSequence = $row.IntegerData(2)
                if ($cabinets.Count -ge 16 -or $name -cnotmatch '^#[A-Za-z0-9_-][A-Za-z0-9_.-]{0,79}\.cab$' -or
                    $name.Contains('..') -or $lastSequence -le 0 -or
                    ($cabinets.Count -and $lastSequence -le $cabinets[$cabinets.Count-1].LastSequence)) {
                    throw 'Unexpected embedded cabinet descriptor.'
                }
                $streamName = $name.Substring(1)
                $cabinetPath = Join-Path $root $streamName
                $streamView = $database.OpenView("SELECT ``Data`` FROM ``_Streams`` WHERE ``Name`` = '$streamName'")
                $record = $null; $outputCabinet = $null
                try {
                    $streamView.Execute(); $record = $streamView.Fetch()
                    if (-not $record) { throw 'Embedded cabinet stream missing.' }
                    $outputCabinet = [IO.File]::Open($cabinetPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                    $cabinetBytes = 0L
                    while ($chunk = $record.ReadStream(1, 65536, 1)) {
                        $bytes = $encoding.GetBytes($chunk); $cabinetBytes += $bytes.Length
                        if ($bytes.Length -ne $chunk.Length -or $cabinetBytes -gt $recipe.bytes) { throw 'Cabinet byte budget.' }
                        $outputCabinet.Write($bytes, 0, $bytes.Length)
                    }
                    if (-not $cabinetBytes) { throw 'Empty embedded cabinet.' }
                    $cabinets.Add([pscustomobject]@{ Path=$cabinetPath; LastSequence=$lastSequence })
                } finally {
                    if ($outputCabinet) { $outputCabinet.Dispose() }
                    if ($record) { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) | Out-Null }
                    $streamView.Close(); [Runtime.InteropServices.Marshal]::FinalReleaseComObject($streamView) | Out-Null
                }
            } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($row) | Out-Null }
        }
    } finally { $media.Close(); [Runtime.InteropServices.Marshal]::FinalReleaseComObject($media) | Out-Null }
    if (-not $cabinets.Count) { throw 'No embedded SDK cabinet.' }
    $destination = Join-Path $root 'selected-payloads'
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    $expand = Join-Path ([Environment]::SystemDirectory) 'expand.exe'
    if (-not (Test-Path -LiteralPath $expand -PathType Leaf)) { throw 'Native cabinet extraction unavailable.' }
    $view = $database.OpenView('SELECT `File`, `FileName`, `Component_`, `Sequence`, `FileSize` FROM `File`')
    $view.Execute()
    while ($record = $view.Fetch()) {
      try {
        $key = $record.StringData(1)
        $filename = ($record.StringData(2) -split '\|')[-1]
        if (-not $wanted.ContainsKey($filename)) { continue }
        if ($record.StringData(3) -ne $components[$filename]) { continue }
        if ($copied.ContainsKey($filename)) { throw "Ambiguous SDK table entry: $filename" }
        $sequence = $record.IntegerData(4); $fileBytes = $record.IntegerData(5)
        if ($key -cnotmatch '^[A-Za-z0-9_][A-Za-z0-9_.-]{0,159}$' -or $key.Contains('..') -or
            $sequence -le 0 -or $fileBytes -le 0) { throw 'Unexpected SDK payload descriptor.' }
        if ($drive.AvailableFreeSpace - 1GB -lt 2 * [long]$fileBytes) { throw 'Insufficient headroom for SDK payload and normalized copy.' }
        $cabinet = @($cabinets | Where-Object { $_.LastSequence -ge $sequence } | Select-Object -First 1)
        if ($cabinet.Count -ne 1) { throw 'SDK payload has no cabinet.' }
        $source = Join-Path $destination $key
        if (Test-Path -LiteralPath $source) { throw 'SDK payload destination already exists.' }
        & $expand $cabinet[0].Path "-F:$key" $destination | Out-Null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $source -PathType Leaf) -or
            (Get-Item -LiteralPath $source).Length -ne $fileBytes) { throw 'Selective SDK extraction failed.' }
        $target = Join-Path $sdk $wanted[$filename]
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        [IO.File]::Copy($source, $target, $false)
        $copied[$filename] = $true
      } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) | Out-Null }
    }
} finally {
    if ($view) { $view.Close(); [Runtime.InteropServices.Marshal]::FinalReleaseComObject($view) | Out-Null }
    [Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) | Out-Null
    [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) | Out-Null
}
if ($copied.Count -ne $wanted.Count) { throw 'Required SDK headers or user-mode library are missing.' }
# All selected files are read-only build/evidence inputs.
if ($env:GITHUB_ENV) {
    Add-Content -LiteralPath $env:GITHUB_ENV -Value "FLITZFS_WINFSP_SDK=$sdk" -Encoding utf8
}
$payloads = @()
foreach ($relative in $wanted.Values) {
    $item = Get-Item -LiteralPath (Join-Path $sdk $relative)
    $payloads += [ordered]@{ path=$relative; bytes=$item.Length; sha256=(Get-SdkSha256 $item.FullName) }
}
$provenance = [ordered]@{ commit=$taskSourceCommit; profile=$Profile; dependency=$recipe; msi_bytes=$total; payloads=$payloads; driver_evidence=[bool]$IncludeDriverEvidence; driver_installed=$false; driver_loaded=$false }
$receipt = Join-Path $root 'sdk-provenance.json'
$receiptStream = [IO.File]::Open($receipt, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $receiptBytes = [Text.UTF8Encoding]::new($false).GetBytes(($provenance | ConvertTo-Json -Depth 8))
    $receiptStream.Write($receiptBytes, 0, $receiptBytes.Length)
} finally {
    $receiptStream.Dispose()
}
if ($env:GITHUB_ENV) {
    Add-Content -LiteralPath $env:GITHUB_ENV -Value "FLITZFS_WINFSP_PROVENANCE=$receipt" -Encoding utf8
}
Write-Output ([ordered]@{ sdk=$sdk; provenance=$receipt; driver_installed=$false; driver_loaded=$false } | ConvertTo-Json -Compress)
Write-Output ("Verified source-bound WinFsp " + $recipe.tag + " SDK staged; no driver/service installation.")
