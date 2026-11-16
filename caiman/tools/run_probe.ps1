# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
  Boot caiman-probe on a connected Pixel 9 Pro and save its log.
  Windows version of run_probe.sh (same steps, same exit codes).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File caiman\tools\run_probe.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File caiman\tools\run_probe.ps1 -Image caiman\prebuilt\probe-boot-lz4.img
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File caiman\tools\run_probe.ps1 -Fetch

.NOTES
  Picks the device whose `adb devices -l` line says device:caiman, so other
  phones or emulators can stay connected; set $env:ANDROID_SERIAL to override.
  Needs Android platform-tools (adb, fastboot) on PATH and root (su) on the phone.
#>
param(
    [string]$Image = (Join-Path $PSScriptRoot '..\prebuilt\probe-boot.img'),
    [string]$Log = 'probe-log.txt',
    [switch]$Fetch,   # only read the log (after a manual force-restart)
    [switch]$Uart     # run `fastboot oem uart enable` first
)

# Native tools write progress to stderr; don't let that abort the script.
$ErrorActionPreference = 'Continue'

foreach ($t in 'adb', 'fastboot') {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
        Write-Host "$t not found: install Android platform-tools and add its folder to PATH"
        exit 1
    }
}
if (-not $Fetch -and -not (Test-Path $Image)) {
    Write-Host "image not found: $Image"
    exit 1
}

if (-not $env:ANDROID_SERIAL) {
    $line = adb devices -l | Where-Object { $_ -match '^\S+\s+device\s' -and $_ -match 'device:caiman' } |
        Select-Object -First 1
    if (-not $line) {
        Write-Host 'no Pixel 9 Pro (device:caiman) found. adb sees:'
        adb devices -l
        Write-Host 'set $env:ANDROID_SERIAL = "<serial>" to pick one by hand'
        exit 1
    }
    $env:ANDROID_SERIAL = ($line -split '\s+')[0]   # adb and fastboot both honour it
}
$serial = $env:ANDROID_SERIAL
Write-Host "device: $serial"

function Test-Fastboot {
    $out = fastboot devices 2>$null
    return [bool]($out | Where-Object { $_ -match ('^' + [regex]::Escape($serial) + '\s') })
}

function Wait-Android([int]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        $v = adb shell getprop sys.boot_completed 2>$null
        if ("$v".Trim() -eq '1') { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

if (-not $Fetch) {
    Write-Host 'rebooting to the bootloader...'
    adb reboot bootloader
    $seen = $false
    for ($i = 0; $i -lt 60 -and -not $seen; $i++) {
        $seen = Test-Fastboot
        if (-not $seen) { Start-Sleep -Seconds 1 }
    }
    if (-not $seen) {
        Write-Host 'the phone did not show up in fastboot within 60 s.'
        Write-Host 'Windows needs the Google USB driver for bootloader mode (Device Manager: "Android Bootloader Interface").'
        exit 1
    }
    if ($Uart) {
        fastboot oem uart enable
        if ($LASTEXITCODE -ne 0) { Write-Host '(oem uart enable failed; continuing)' }
    }
    Write-Host "booting $(Split-Path $Image -Leaf) from RAM (nothing is flashed)..."
    fastboot boot $Image
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'fastboot boot failed. Try: -Image caiman\prebuilt\probe-boot-lz4.img'
        exit 1
    }
    Write-Host 'probe started; it reboots into Android by itself. Waiting up to 4 minutes...'
    if (-not (Wait-Android 240)) {
        Write-Host 'Android did not come back. The probe may be hanging: force-restart the phone'
        Write-Host '(hold Power until it restarts), let Android boot, then run this script with -Fetch'
        exit 2
    }
} elseif (-not (Wait-Android 30)) {
    Write-Host 'phone is not booted into Android'
    exit 1
}

Write-Host 'reading pstore...'
adb shell su -c "'ls -l /sys/fs/pstore'"
$text = adb shell su -c "'cat /sys/fs/pstore/console-ramoops*'"
$lines = @($text | ForEach-Object { "$_".TrimEnd("`r") })
$lines | Set-Content -Path $Log -Encoding UTF8

if ($lines -match '=== caiman-probe') {
    Write-Host "saved $($lines.Count) lines to $Log"
    $start = [array]::IndexOf($lines, '=== SUMMARY ===')
    if ($start -ge 0) {
        $lines[$start..($lines.Count - 1)] | ForEach-Object { Write-Host $_ }
    } else {
        Write-Host 'no SUMMARY: the probe stopped early. Last lines:'
        $lines | Select-Object -Last 15 | ForEach-Object { Write-Host $_ }
    }
    exit 0
}
Write-Host "no caiman-probe text in pstore (saved what was there to $Log)."
Write-Host 'the reset may have cleared RAM: try -Image caiman\prebuilt\probe-boot-hang.img,'
Write-Host 'force-restart by hand when it stops, then run with -Fetch'
exit 3
