<#
.SYNOPSIS
  Runs AIGW "PC as AP" on Windows through WSL2: hands the USB Wi-Fi adapter to
  WSL, keeps WSL running, and starts the access point.

.EXAMPLE
  aigw-wsl.cmd              # start
  aigw-wsl.cmd -Stop        # stop and give the adapter back to Windows
  aigw-wsl.cmd -BusId 1-6   # pick the USB device yourself (see: usbipd list)

.NOTES
  Needs usbipd-win (winget install usbipd) and AIGW installed inside WSL
  (sudo ./install.sh in the cloned repository).
#>
param(
    [switch]$Stop,
    [string]$BusId = "",
    [string]$Distro = ""
)

$ErrorActionPreference = "Stop"

$Usbipd = (Get-Command usbipd -ErrorAction SilentlyContinue).Source
if (-not $Usbipd) { $Usbipd = "C:\Program Files\usbipd-win\usbipd.exe" }
if (-not (Test-Path $Usbipd)) {
    throw "usbipd-win is not installed. Install it with: winget install usbipd"
}

$WslArgs = @()
if ($Distro) { $WslArgs = @("-d", $Distro) }

function Invoke-Wsl([string]$Command) {
    & wsl.exe @WslArgs -u root -- sh -c $Command
}

function Find-Adapter {
    $lines = & $Usbipd list 2>$null | Where-Object { $_ -match '^\s*\d+-\d+' }
    if ($BusId) { return $lines | Where-Object { $_ -match "^\s*$([regex]::Escape($BusId))\s" } | Select-Object -First 1 }
    return $lines | Where-Object { $_ -match 'Wireless|WLAN|Wi-?Fi|802\.11' } | Select-Object -First 1
}

$line = Find-Adapter
if (-not $line) {
    throw "No USB Wi-Fi adapter found. Plug one in, or pass -BusId (see 'usbipd list')."
}
$bus = ($line.ToString().Trim() -split '\s+')[0]

if ($Stop) {
    Invoke-Wsl "aigw-ap down; pkill -f '^aigw-keepalive'" | Out-Host
    if ($line -match 'Attached') { & $Usbipd detach --busid $bus 2>$null }
    Write-Host "AIGW stopped; adapter $bus returned to Windows."
    return
}

if ($line -match 'Not shared') {
    Write-Host "Sharing adapter $bus with WSL (one time, needs administrator approval)..."
    Start-Process $Usbipd -ArgumentList "bind --busid $bus" -Verb RunAs -Wait
}

# WSL shuts the distribution down when no session is open; keep one alive.
$running = & wsl.exe @WslArgs -u root -- sh -c "pgrep -f '^aigw-keepalive' >/dev/null && echo yes"
if ($running -ne "yes") {
    $argList = @($WslArgs + @("-u", "root", "--", "bash", "-c", "exec -a aigw-keepalive sleep infinity"))
    Start-Process wsl.exe -ArgumentList $argList -WindowStyle Hidden | Out-Null
    Start-Sleep 3
}

if (-not ((Find-Adapter) -match 'Attached')) {
    Write-Host "Attaching adapter $bus to WSL..."
    & $Usbipd attach --wsl --busid $bus 2>$null
    Start-Sleep 4
}

Invoke-Wsl "aigw-ap up" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "aigw-ap up failed (see messages above)." }
Write-Host ""
Write-Host "AIGW is running. Stop it with: aigw-wsl.cmd -Stop"
