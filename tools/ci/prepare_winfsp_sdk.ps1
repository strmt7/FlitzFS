# Input: a disposable GitHub-hosted Windows runner. Output: normalized SDK path
# in GITHUB_ENV. Read MSI tables/cabinets only; never execute an installation.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or $env:RUNNER_OS -ne 'Windows') {
    throw 'This dependency extractor is restricted to GitHub-hosted Windows runners.'
}
if (-not $env:RUNNER_TEMP -or -not $env:GITHUB_ENV) { throw 'Runner paths are missing.' }

$lockPath = Join-Path $PSScriptRoot '../winfsp-sdk-lock.json'
if ((Get-Item -LiteralPath $lockPath).Length -gt 16384) { throw 'WinFsp lock budget.' }
$recipe = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
if ($recipe.schema -ne 1 -or $recipe.repository -ne 'winfsp/winfsp' -or
    $recipe.tag -notmatch '^v[0-9]+(?:\.[0-9]+){1,3}(?:B[0-9]+)?$' -or
    $recipe.msi_name -notmatch '^winfsp-[0-9]+\.[0-9]+\.[0-9]+\.msi$' -or
    $recipe.sha256 -notmatch '^[0-9a-f]{64}$' -or $recipe.source_commit -notmatch '^[0-9a-f]{40}$' -or
    $recipe.bytes -le 0 -or $recipe.bytes -gt 4MB -or
    $recipe.url -cne ("https://github.com/winfsp/winfsp/releases/download/" + $recipe.tag + "/" + $recipe.msi_name)) {
    throw 'Invalid source-bound WinFsp SDK lock.'
}

$base = [IO.Path]::GetFullPath($env:RUNNER_TEMP)
$drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($base))
if ($drive.AvailableFreeSpace -lt 1GB) { throw 'Insufficient temporary storage headroom.' }
$root = Join-Path $base ('flitzfs-sdk-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $root) { throw 'Fresh SDK staging directory already exists.' }
[IO.Directory]::CreateDirectory($root) | Out-Null
$msi = Join-Path $root 'winfsp.msi'
$url = $recipe.url
$expected = $recipe.sha256
# Limit the entire response to 4 MiB and 60 seconds, including streaming.
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
        if ($total -gt 4MB) { throw 'SDK download exceeds cap.' }
        $outputStream.Write($buffer, 0, $n)
    }
} finally {
    if ($outputStream) { $outputStream.Dispose() }
    if ($inputStream) { $inputStream.Dispose() }
    if ($response) { $response.Dispose() }
    $client.Dispose(); $cancel.Dispose()
}
if ($total -ne $recipe.bytes -or (Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'WinFsp SDK hash does not match the pinned official release.'
}
$seven = (Get-Command 7z.exe -ErrorAction Stop).Source
$unpacked = Join-Path $root 'unpacked'
& $seven x $msi "-o$unpacked" -aos -y | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'MSI archive extraction failed.' }
$cabinets = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter '*.cab')
if ($cabinets.Count -gt 16) { throw 'Unexpected cabinet count.' }
foreach ($cabinet in $cabinets) {
    $destination = Join-Path $root ('cab-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    & "$env:SystemRoot\System32\expand.exe" $cabinet.FullName '-F:*' $destination | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'SDK cabinet extraction failed.' }
}
$all = @(Get-ChildItem -LiteralPath $root -Recurse -File)
if ($all.Count -gt 2048 -or ($all | Measure-Object Length -Sum).Sum -gt 64MB) {
    throw 'Unexpected SDK extracted size.'
}
# Windows Installer database mode 0 is read-only. No install/custom actions run.
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.OpenDatabase($msi, 0)
$view = $database.OpenView('SELECT `File`, `FileName`, `Component_` FROM `File`')
$view.Execute()
$wanted = @{
    'winfsp.h' = 'include/winfsp/winfsp.h'
    'fsctl.h' = 'include/winfsp/fsctl.h'
    'winfsp-x64.lib' = 'lib/winfsp-x64.lib'
    'winfsp-x64.dll' = 'bin/winfsp-x64.dll'
}
# Component identities are verified in the selected v2.2B5 Product.wxs (source_commit in the lock). The
# optional fsext library has the same basename but is NOT the public SDK library.
$components = @{
    'winfsp.h' = 'C.winfsp.h'
    'fsctl.h' = 'C.fsctl.h'
    'winfsp-x64.lib' = 'C.winfsp_x64.lib'
    'winfsp-x64.dll' = 'C.winfsp_x64.dll.x64'
}
$sdk = Join-Path $root 'sdk'
$copied = @{}
try {
    while ($record = $view.Fetch()) {
        $key = $record.StringData(1)
        $filename = ($record.StringData(2) -split '\|')[-1]
        if (-not $wanted.ContainsKey($filename)) { continue }
        if ($record.StringData(3) -ne $components[$filename]) { continue }
        if ($copied.ContainsKey($filename)) { throw "Ambiguous SDK table entry: $filename" }
        $matches = @($all | Where-Object { $_.Name -eq $key })
        if ($matches.Count -eq 0) { $matches = @($all | Where-Object { $_.Name -eq $filename }) }
        if ($matches.Count -ne 1) { throw "Expected exactly one extracted payload for $filename ($key); found $($matches.Count)." }
        $target = Join-Path $sdk $wanted[$filename]
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        [IO.File]::Copy($matches[0].FullName, $target, $false)
        $copied[$filename] = $true
    }
} finally {
    $view.Close()
    [Runtime.InteropServices.Marshal]::FinalReleaseComObject($view) | Out-Null
    [Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) | Out-Null
    [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) | Out-Null
}
if ($copied.Count -ne $wanted.Count) { throw 'Required SDK headers or user-mode library are missing.' }
# No .sys file is selected, installed or loaded; only four build/runtime files.
Add-Content -LiteralPath $env:GITHUB_ENV -Value "FLITZFS_WINFSP_SDK=$sdk" -Encoding utf8
$payloads = @()
foreach ($relative in $wanted.Values) {
    $item = Get-Item -LiteralPath (Join-Path $sdk $relative)
    $payloads += [ordered]@{ path=$relative; bytes=$item.Length; sha256=(Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$provenance = [ordered]@{ commit=$env:GITHUB_SHA; dependency=$recipe; msi_bytes=$total; payloads=$payloads; driver_installed=$false; driver_loaded=$false }
$receipt = Join-Path $root 'sdk-provenance.json'
[IO.File]::WriteAllText($receipt, ($provenance | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
Add-Content -LiteralPath $env:GITHUB_ENV -Value "FLITZFS_WINFSP_PROVENANCE=$receipt" -Encoding utf8
Write-Output ("Verified source-bound WinFsp " + $recipe.tag + " candidate SDK staged; no driver/service installation.")
