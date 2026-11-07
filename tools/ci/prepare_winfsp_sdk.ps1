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
$base = [IO.Path]::GetFullPath($env:RUNNER_TEMP)
$drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($base))
if ($drive.AvailableFreeSpace -lt 1GB) { throw 'Insufficient temporary storage headroom.' }
$root = Join-Path $base ('flitzfs-sdk-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $root) { throw 'Fresh SDK staging directory already exists.' }
[IO.Directory]::CreateDirectory($root) | Out-Null
$msi = Join-Path $root 'winfsp.msi'
$url = 'https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi'
$expected = '073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a'
# Limit the entire response to 4 MiB and 60 seconds, including streaming.
$cancel = [Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(60))
$client = [Net.Http.HttpClient]::new()
$response = $null; $inputStream = $null; $outputStream = $null
try {
    $response = $client.GetAsync($url, [Net.Http.HttpCompletionOption]::ResponseHeadersRead, $cancel.Token).GetAwaiter().GetResult()
    $response.EnsureSuccessStatusCode() | Out-Null
    if ($response.Content.Headers.ContentLength -gt 4MB) { throw 'SDK download exceeds cap.' }
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
if ((Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
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
$view = $database.OpenView('SELECT `File`, `FileName` FROM `File`')
$view.Execute()
$wanted = @{
    'winfsp.h' = 'include/winfsp/winfsp.h'
    'fsctl.h' = 'include/winfsp/fsctl.h'
    'winfsp-x64.lib' = 'lib/winfsp-x64.lib'
    'winfsp-x64.dll' = 'bin/winfsp-x64.dll'
}
$sdk = Join-Path $root 'sdk'
$copied = @{}
try {
    while ($record = $view.Fetch()) {
        $key = $record.StringData(1)
        $filename = ($record.StringData(2) -split '\|')[-1]
        if (-not $wanted.ContainsKey($filename)) { continue }
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
Write-Output 'Verified WinFsp 2.1 SDK staged without installing a driver or service.'
