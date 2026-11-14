<#
.SYNOPSIS
  Runs AIGW "PC as AP" on Windows through WSL2: hands the USB Wi-Fi adapters to
  WSL (one, or two for a dual-band access point), keeps WSL running, and starts
  the access point.

.EXAMPLE
  aigw-wsl.cmd                  # start
  aigw-wsl.cmd -Stop            # stop and give the adapters back to Windows
  aigw-wsl.cmd -BusId 1-6,1-7   # pick the USB devices yourself (see: usbipd list)
  aigw-wsl.cmd -Install         # start by itself at every logon, and keep it up
  aigw-wsl.cmd -Uninstall       # no longer start by itself

.NOTES
  Needs usbipd-win (winget install usbipd) and AIGW installed inside WSL
  (sudo ./install.sh in the cloned repository). Sharing a new adapter asks for
  administrator approval once; after that `sudo aigw-ap up` inside WSL also
  attaches the adapters by itself.

  -Install registers a Task Scheduler task (administrator approval, once) that
  runs this script with -Boot at your logon: it waits for the adapters (USB
  hubs and extensions come up late), shares them again if Windows forgot
  (another USB port, another USB driver), attaches them, keeps WSL running and
  starts the access point; then it checks every minute and starts it again if
  it went down (WSL shut down, an adapter dropped off USB). It only starts an
  access point that is down; a running one is left alone. The adapters it
  looks after are the Wi-Fi adapters shared when -Install ran (by USB id, so a
  move to another port is fine). Log: %LOCALAPPDATA%\aigw\aigw-wsl.log.
  It starts at logon: after a power cut, Windows must log you in (or be set
  to log in by itself) for the access point to come back.
#>
param(
    [switch]$Stop,
    [switch]$Install,
    [switch]$Uninstall,
    [switch]$Boot,
    [string]$BusId = "",
    [string]$UsbId = "",
    [string]$Distro = ""
)

# "Continue", not "Stop": Windows PowerShell 5.1 turns any stderr line from a
# native program (usbipd prints harmless warnings) into a fatal error under
# "Stop". Failures are checked explicitly below instead.
$ErrorActionPreference = "Continue"
$TaskName = "AIGW OS access point"

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

function Test-Admin {
    $id = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    return $id.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Every Wi-Fi adapter usbipd lists (or the bus ids, or USB ids, given), as their list lines.
function Find-Adapters {
    $lines = & $Usbipd list 2>$null | Where-Object { $_ -match '^\s*\d+-\d+\s' }
    if ($BusId) {
        $want = $BusId -split '[,\s]+' | Where-Object { $_ }
        return @($lines | Where-Object { $want -contains ($_.ToString().Trim() -split '\s+')[0] })
    }
    if ($UsbId) {
        $want = $UsbId -split '[,\s]+' | Where-Object { $_ }
        return @($lines | Where-Object { $want -contains ($_.ToString().Trim() -split '\s+')[1] })
    }
    return @($lines | Where-Object { $_ -match 'Wireless|WLAN|Wi-?Fi|802\.11' })
}

function Bus-Of($line) { return ($line.ToString().Trim() -split '\s+')[0] }
function UsbId-Of($line) { return ($line.ToString().Trim() -split '\s+')[1] }

# WSL shuts the distribution down when no session is open; keep one alive.
function Start-KeepAlive {
    $running = & wsl.exe @WslArgs -u root -- sh -c "pgrep -f '^aigw-keepalive' >/dev/null && echo yes"
    if ($running -ne "yes") {
        $argList = @($WslArgs + @("-u", "root", "--", "bash", "-c", "exec -a aigw-keepalive sleep infinity"))
        Start-Process wsl.exe -ArgumentList $argList -WindowStyle Hidden | Out-Null
        Start-Sleep 3
        return $true
    }
    return $false
}

function Attach-Adapters {
    $attached = $false
    foreach ($line in Find-Adapters) {
        if ($line -match 'Attached' -or $line -match 'Not shared') { continue }
        $bus = Bus-Of $line
        Write-Host "Attaching adapter $bus to WSL..."
        & $Usbipd attach --wsl --busid $bus 2>$null
        $attached = $true
    }
    if ($attached) { Start-Sleep 4 }
}

if ($Install) {
    if (-not (Test-Admin)) {
        # the task runs with the highest rights (to share an adapter again): needs approval once
        $a = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Install"
        if ($Distro) { $a += " -Distro $Distro" }
        if ($UsbId) { $a += " -UsbId $UsbId" }
        Write-Host "Registering the task needs administrator approval..."
        Start-Process powershell.exe -ArgumentList $a -Verb RunAs -Wait
        if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
            Write-Host "Installed: the access point starts at every logon and is kept up."
            Write-Host "Log: $env:LOCALAPPDATA\aigw\aigw-wsl.log   Remove: aigw-wsl.cmd -Uninstall"
        } else { throw "The task was not registered." }
        return
    }
    if (-not $UsbId) {
        # the adapters shared now, by USB id: they may move to another port
        $UsbId = (Find-Adapters | Where-Object { $_ -notmatch 'Not shared' } | ForEach-Object { UsbId-Of $_ } |
                  Sort-Object -Unique) -join ","
        if (-not $UsbId) { throw "No shared USB Wi-Fi adapter: run aigw-wsl.cmd once first (it shares them)." }
    }
    $user = (Get-CimInstance Win32_ComputerSystem).UserName
    if (-not $user) { $user = "$env:USERDOMAIN\$env:USERNAME" }
    $a = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Boot -UsbId $UsbId"
    if ($Distro) { $a += " -Distro $Distro" }
    $action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $a
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
        -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal `
        -Settings $settings -Description "AIGW OS: PC as AP through WSL2 (aigw-wsl.ps1 -Boot)" -Force | Out-Null
    Start-ScheduledTask -TaskName $TaskName
    return
}

if ($Uninstall) {
    if (-not (Test-Admin)) {
        Start-Process powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Uninstall" -Verb RunAs -Wait
        return
    }
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "The access point no longer starts by itself (it keeps running until aigw-wsl.cmd -Stop)."
    return
}

if ($Boot) {
    # the watchdog: started by the task at logon, runs until logoff
    $logDir = Join-Path $env:LOCALAPPDATA "aigw"
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $log = Join-Path $logDir "aigw-wsl.log"
    function Log([string]$m) {
        $line = "{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $m
        try {
            if ((Test-Path $log) -and (Get-Item $log).Length -gt 1MB) { Move-Item -Force $log "$log.1" }
            Add-Content -Path $log -Value $line
        } catch {}
    }
    Log "watchdog started (adapters: $(if ($UsbId) { $UsbId } elseif ($BusId) { $BusId } else { 'every Wi-Fi adapter' }))"
    $fails = 0
    $last = ""
    while ($true) {
        $adapters = Find-Adapters
        if ($adapters.Count -eq 0) {
            if ($last -ne "none") { Log "no adapter plugged in yet; waiting" }
            $last = "none"
            Start-Sleep 20
            continue
        }
        # Windows can forget a share (another port, another USB driver taking the device)
        foreach ($line in $adapters) {
            if ($line -match 'Not shared') {
                $bus = Bus-Of $line
                Log "adapter $bus is not shared: sharing it again"
                & $Usbipd bind --force --busid $bus 2>$null | Out-Null
            }
        }
        if (Start-KeepAlive) { Log "WSL started (keep-alive)" }
        Attach-Adapters | Out-Null
        $state = (Invoke-Wsl "aigw-ap check 2>&1" | Out-String).Trim()
        if ($LASTEXITCODE -eq 0) {
            if ($last -ne "up") { Log "access point up" }
            $last = "up"
            $fails = 0
        } else {
            # twice in a row (a minute apart): agd changing a channel restarts one access point for seconds
            $fails++
            if ($state -eq "down" -or $fails -ge 2) {
                Log "access point not whole ($($state -replace '\s+', '; ')): starting it"
                $out = (Invoke-Wsl "aigw-ap down >/dev/null 2>&1; aigw-ap up 2>&1" | Out-String).Trim()
                if ($LASTEXITCODE -eq 0) { Log "access point started" } else { Log "start failed: $($out -replace '\s+', ' ')" }
                $fails = 0
            }
            $last = "fail"
        }
        Start-Sleep 60
    }
}

$adapters = Find-Adapters
if ($adapters.Count -eq 0) {
    throw "No USB Wi-Fi adapter found. Plug one in, or pass -BusId (see 'usbipd list')."
}

if ($Stop) {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Write-Host "(the task '$TaskName' starts it again at the next logon; aigw-wsl.cmd -Uninstall removes it)"
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    }
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

Start-KeepAlive | Out-Null
Attach-Adapters

Invoke-Wsl "aigw-ap up" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "aigw-ap up failed (see messages above)." }
Write-Host ""
Write-Host "AIGW is running. Stop it with: aigw-wsl.cmd -Stop"
