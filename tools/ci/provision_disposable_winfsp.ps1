# DANGER BOUNDARY: installs the pinned WinFsp driver ONLY on disposable hosted CI.
# Never run this script on a developer/owner PC or a self-hosted runner.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or
    $env:RUNNER_OS -ne 'Windows' -or $env:FLITZFS_ALLOW_DISPOSABLE_MOUNT -ne '1') {
    throw 'Driver provisioning is restricted to explicitly opted-in disposable hosted Windows CI.'
}
if (-not $env:RUNNER_TEMP -or -not $env:FLITZFS_WINFSP_SDK) { throw 'Pinned MSI staging is missing.' }

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

$base = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') + '\'
$msi = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $env:FLITZFS_WINFSP_SDK) 'winfsp.msi'))
if (-not $msi.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) { throw 'MSI is outside runner staging.' }
if ((Get-Item -LiteralPath $msi).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'MSI is a reparse point.' }
$expected = $recipe.sha256
if ((Get-Item -LiteralPath $msi).Length -ne $recipe.bytes -or (Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Pinned MSI hash mismatch.' }
if ((Get-AuthenticodeSignature -LiteralPath $msi).Status -ne 'Valid') { throw 'MSI Authenticode verification failed.' }
# Inspect the exact AMD64 driver from the already verified, bounded MSI payload.
# This is evidence only: Authenticode does not establish Windows 11 policy/HVCI.
$stage = Split-Path -Parent $msi
$payloads = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
if ($payloads.Count -gt 2048 -or ($payloads | Measure-Object Length -Sum).Sum -gt 64MB) {
    throw 'Driver evidence payload budget.'
}
$driver = $null
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.OpenDatabase($msi, 0)
$view = $database.OpenView('SELECT `File`, `FileName`, `Component_` FROM `File`')
$view.Execute()
try {
    while ($record = $view.Fetch()) {
        if ($record.StringData(3) -ne 'C.winfsp_x64.sys') { continue }
        if (($record.StringData(2) -split '\|')[-1] -ne 'winfsp-x64.sys' -or $driver) {
            throw 'Ambiguous AMD64 driver MSI identity.'
        }
        $key = $record.StringData(1)
        $matches = @($payloads | Where-Object { $_.Name -eq $key })
        if ($matches.Count -eq 0) { $matches = @($payloads | Where-Object { $_.Name -eq 'winfsp-x64.sys' }) }
        if ($matches.Count -ne 1) { throw 'Exactly one verified AMD64 driver payload required.' }
        $driver = $matches[0]
    }
} finally {
    $view.Close()
    [Runtime.InteropServices.Marshal]::FinalReleaseComObject($view) | Out-Null
    [Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) | Out-Null
    [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) | Out-Null
}
if (-not $driver -or ($driver.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Verified regular AMD64 driver payload missing.'
}
$signature = Get-AuthenticodeSignature -LiteralPath $driver.FullName
$signer = $signature.SignerCertificate
$evidence = [ordered]@{
    schema=1; commit=$env:GITHUB_SHA; candidate=$recipe; phase='before-installer'
    os_description=[Runtime.InteropServices.RuntimeInformation]::OSDescription
    driver=[ordered]@{
        name='winfsp-x64.sys'; bytes=$driver.Length
        sha256=(Get-FileHash -LiteralPath $driver.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        file_version=$driver.VersionInfo.FileVersion
        signature_status=[string]$signature.Status; signature_type=[string]$signature.SignatureType
        signer_subject=if ($signer) { $signer.Subject } else { $null }
        signer_issuer=if ($signer) { $signer.Issuer } else { $null }
        signer_thumbprint=if ($signer) { $signer.Thumbprint } else { $null }
    }
    driver_installed=$false; windows11_policy_qualified=$false
}
$evidencePath = Join-Path $stage 'driver-provenance.json'
if (Test-Path -LiteralPath $evidencePath) { throw 'Driver evidence output already exists.' }
[IO.File]::WriteAllText($evidencePath, ($evidence | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
Write-Output ('WinFsp driver package evidence: ' + ($evidence | ConvertTo-Json -Depth 8 -Compress))

if (Test-Path 'HKLM:\SOFTWARE\WOW6432Node\WinFsp') { throw 'Refusing to alter an existing WinFsp installation.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try {
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Hosted installer requires administrator context; no elevation is attempted.' }
} finally { $identity.Dispose() }
$arguments = @('/i', ('"' + $msi + '"'), '/qn', '/norestart', 'REBOOT=ReallySuppress', 'ADDLOCAL=F.Main,F.User')
$process = Start-Process -FilePath "$env:SystemRoot\System32\msiexec.exe" -ArgumentList $arguments -PassThru -WindowStyle Hidden
try {
    if (-not $process.WaitForExit(90000)) {
        $process.Kill()
        throw 'Disposable installer timed out; stop this job and discard the VM.'
    }
    if ($process.ExitCode -ne 0) { throw "Installer returned $($process.ExitCode); no reboot or retry is allowed." }
    $evidence.driver_installed = $true
    $evidence.phase = 'installer-completed'
} catch {
    $evidence.phase = 'installer-failed'
    $evidence['error'] = $_.Exception.Message
    throw
} finally {
    $process.Dispose()
    # This file was created by THIS invocation inside its freshly staged VM fixture.
    [IO.File]::WriteAllText($evidencePath, ($evidence | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}
Write-Output ("Pinned WinFsp " + $recipe.tag + " development candidate provisioned on disposable hosted CI only; Windows 11 production qualification remains separate.")
