<#
.SYNOPSIS
  Runs AIGW "PC as AP" on Windows through WSL2: hands the USB Wi-Fi adapters to
  WSL (one, or two for a dual-band access point), keeps WSL running, and starts
  the access point.

.EXAMPLE
  aigw-wsl.cmd                  # start
  aigw-wsl.cmd -Stop            # stop and give the adapters back to Windows
  aigw-wsl.cmd -BusId 1-6,1-7   # pick the USB devices yourself (see: usbipd list)

.NOTES
  Needs usbipd-win (winget install usbipd) and AIGW installed inside WSL
  (sudo ./install.sh in the cloned repository). Sharing a new adapter asks for
  administrator approval once; after that `sudo aigw-ap up` inside WSL also
  attaches the adapters by itself.
#>
param(
    [switch]$Stop,
    [string]$BusId = "",
    [string]$Distro = ""
)

# "Continue", not "Stop": Windows PowerShell 5.1 turns any stderr line from a
# native program (usbipd prints harmless warnings) into a fatal error under
# "Stop". Failures are checked explicitly below instead.
$ErrorActionPreference = "Continue"

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

# Every Wi-Fi adapter usbipd lists (or the bus ids given), as their list lines.
function Find-Adapters {
    $lines = & $Usbipd list 2>$null | Where-Object { $_ -match '^\s*\d+-\d+\s' }
    if ($BusId) {
        $want = $BusId -split '[,\s]+' | Where-Object { $_ }
        return @($lines | Where-Object { $want -contains ($_.ToString().Trim() -split '\s+')[0] })
    }
    return @($lines | Where-Object { $_ -match 'Wireless|WLAN|Wi-?Fi|802\.11' })
}

function Bus-Of($line) { return ($line.ToString().Trim() -split '\s+')[0] }

$adapters = Find-Adapters
if ($adapters.Count -eq 0) {
    throw "No USB Wi-Fi adapter found. Plug one in, or pass -BusId (see 'usbipd list')."
}

if ($Stop) {
    Invoke-Wsl "aigw-ap down; pkill -f '^aigw-keepalive'" | Out-Host
    foreach ($line in $adapters) {
        if ($line -match 'Attached') { & $Usbipd detach --busid (Bus-Of $line) 2>$null }
    }
    Write-Host "AIGW stopped; adapters returned to Windows."
    return
}

# Share new adapters with WSL (administrator approval, once per adapter). A
# USB filter driver such as USBPcap makes a plain bind fail; then --force.
foreach ($line in $adapters) {
    if ($line -notmatch 'Not shared') { continue }
    $bus = Bus-Of $line
    Write-Host "Sharing adapter $bus with WSL (one time, needs administrator approval)..."
    Start-Process $Usbipd -ArgumentList "bind --busid $bus" -Verb RunAs -Wait
    $now = Find-Adapters | Where-Object { (Bus-Of $_) -eq $bus }
    if ($now -match 'Not shared') {
        Write-Host "Plain sharing was refused (a USB filter driver?); sharing with --force..."
        Start-Process $Usbipd -ArgumentList "bind --force --busid $bus" -Verb RunAs -Wait
    }
}

# WSL shuts the distribution down when no session is open; keep one alive.
$running = & wsl.exe @WslArgs -u root -- sh -c "pgrep -f '^aigw-keepalive' >/dev/null && echo yes"
if ($running -ne "yes") {
    $argList = @($WslArgs + @("-u", "root", "--", "bash", "-c", "exec -a aigw-keepalive sleep infinity"))
    Start-Process wsl.exe -ArgumentList $argList -WindowStyle Hidden | Out-Null
    Start-Sleep 3
}

$attached = $false
foreach ($line in Find-Adapters) {
    if ($line -match 'Attached' -or $line -match 'Not shared') { continue }
    $bus = Bus-Of $line
    Write-Host "Attaching adapter $bus to WSL..."
    & $Usbipd attach --wsl --busid $bus 2>$null
    $attached = $true
}
if ($attached) { Start-Sleep 4 }

Invoke-Wsl "aigw-ap up" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "aigw-ap up failed (see messages above)." }
Write-Host ""
Write-Host "AIGW is running. Stop it with: aigw-wsl.cmd -Stop"
