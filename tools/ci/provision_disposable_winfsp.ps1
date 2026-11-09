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
} finally { $process.Dispose() }
Write-Output ("Pinned WinFsp " + $recipe.tag + " development candidate provisioned on disposable hosted CI only; Windows 11 production qualification remains separate.")
