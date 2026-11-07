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
$base = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') + '\'
$msi = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $env:FLITZFS_WINFSP_SDK) 'winfsp.msi'))
if (-not $msi.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) { throw 'MSI is outside runner staging.' }
if ((Get-Item -LiteralPath $msi).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'MSI is a reparse point.' }
$expected = '073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a'
if ((Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Pinned MSI hash mismatch.' }
if ((Get-AuthenticodeSignature -LiteralPath $msi).Status -ne 'Valid') { throw 'MSI Authenticode verification failed.' }
if (Test-Path 'HKLM:\SOFTWARE\WOW6432Node\WinFsp') { throw 'Refusing to alter an existing WinFsp installation.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try {
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Hosted installer requires administrator context; no elevation is attempted.' }
} finally { $identity.Dispose() }
$arguments = @('/i', ('"' + $msi + '"'), '/qn', '/norestart', 'REBOOT=ReallySuppress', 'ADDLOCAL=F.Main,F.User')
$process = Start-Process -FilePath "$env:SystemRoot\System32\msiexec.exe" -ArgumentList $arguments -PassThru -NoNewWindow
try {
    if (-not $process.WaitForExit(90000)) {
        $process.Kill()
        throw 'Disposable installer timed out; stop this job and discard the VM.'
    }
    if ($process.ExitCode -ne 0) { throw "Installer returned $($process.ExitCode); no reboot or retry is allowed." }
} finally { $process.Dispose() }
Write-Output 'Pinned WinFsp provisioned on disposable hosted CI only. The VM is discarded after the job.'
