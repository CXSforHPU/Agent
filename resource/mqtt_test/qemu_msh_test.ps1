# MQTT tool smoke test on QEMU: drives MSH over a TCP serial port.
#  - kills leftover QEMU instances (they hold the serial port and share the MQTT client id)
#  - reconnects the serial link automatically if it drops
#  - runs: start / status / multi-topic subscribe / wildcard routing / publish round trip / poll buffer / unsubscribe
param(
  [int]$Port = 5580,
  [int]$Smp = 2,
  [string]$Qemu = "D:\rtt\env-windows\tools\qemu\qemu64\qemu-system-arm.exe"
)

$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$ws = (Resolve-Path (Join-Path $here '..\..\..\..')).Path   # BSP root

for ($i = 0; $i -lt 10; $i++) {
  $p = Get-Process qemu-system-arm -ErrorAction SilentlyContinue
  if (-not $p) { break }
  $p | Stop-Process -Force
  Start-Sleep -Milliseconds 700
}

$qargs = @(
  '-M', 'vexpress-a9', '-smp', "cpus=$Smp",
  '-kernel', 'rtthread.bin', '-sd', 'sd.bin',
  '-display', 'none',
  '-serial', "tcp:127.0.0.1:$Port,server,nowait",
  '-net', 'nic,model=lan9118', '-net', 'user'
)
$proc = Start-Process -FilePath $Qemu -ArgumentList $qargs -WorkingDirectory $ws -PassThru -WindowStyle Hidden
Write-Output "qemu pid=$($proc.Id) port=$Port smp=$Smp"

$script:client = $null
$script:stream = $null
$log = New-Object System.Text.StringBuilder

function Connect-Serial {
  if ($script:stream) { try { $script:stream.Close() } catch {} }
  if ($script:client) { try { $script:client.Close() } catch {} }
  $script:client = $null; $script:stream = $null
  for ($i = 0; $i -lt 60; $i++) {
    try {
      $c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port)
      $c.ReceiveTimeout = 1000
      $script:client = $c; $script:stream = $c.GetStream()
      return $true
    } catch { Start-Sleep -Milliseconds 500 }
  }
  return $false
}

function Pump([int]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $buf = New-Object byte[] 8192
  while ((Get-Date) -lt $deadline) {
    if ($null -eq $script:stream) { if (-not (Connect-Serial)) { Start-Sleep -Milliseconds 500; continue } }
    try {
      if ($script:stream.DataAvailable) {
        $n = $script:stream.Read($buf, 0, $buf.Length)
        if ($n -gt 0) { [void]$log.Append([System.Text.Encoding]::UTF8.GetString($buf, 0, $n)) }
        elseif ($n -eq 0) { $script:stream = $null }
      } else { Start-Sleep -Milliseconds 50 }
    } catch { $script:stream = $null }
  }
}

function Write-Line([string]$c) {
  if ($null -eq $script:stream) { if (-not (Connect-Serial)) { return } }
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($c + "`r`n")
  try { $script:stream.Write($bytes, 0, $bytes.Length); $script:stream.Flush() } catch { $script:stream = $null }
}

function Cmd([string]$c, [int]$waitAfter = 3) {
  [void]$log.Append("`r`n##### CMD: $c`r`n")
  Write-Line $c
  Pump (3 + $waitAfter)
}

if (-not (Connect-Serial)) { Write-Output "CONNECT FAILED"; exit 1 }
Pump 15
for ($i = 0; $i -lt 8; $i++) { if ($log.ToString() -match 'msh />') { break }; Write-Line ""; Pump 3 }

Cmd "mqtt_tool status" 2
Cmd "mqtt_tool start" 8
Cmd "mqtt_tool status" 2
Cmd "mqtt_tool sub agent/test auto" 4
Cmd "mqtt_tool sub device/+/data poll" 4
Cmd "mqtt_tool sub extra/topic auto" 4
Cmd "mqtt_tool status" 2
Cmd "mqtt_tool pub agent/sub board-hello-1" 4
Cmd "mqtt_tool recv" 3
Cmd "mqtt_tool sim device/1/data {""temp"":25}" 3
Cmd "mqtt_tool sim device/2/data {""temp"":31}" 3
Cmd "mqtt_tool recv 5 device/+/data" 3
Cmd "mqtt_tool status" 2
Cmd "mqtt_tool unsub device/+/data" 3
Cmd "mqtt_tool status" 2
Cmd "free" 2

Write-Output "===== TRANSCRIPT ====="
Write-Output $log.ToString()

if ($script:stream) { try { $script:stream.Close() } catch {} }
if ($script:client) { try { $script:client.Close() } catch {} }
for ($i = 0; $i -lt 10; $i++) {
  $p = Get-Process qemu-system-arm -ErrorAction SilentlyContinue
  if (-not $p) { break }
  $p | Stop-Process -Force
  Start-Sleep -Milliseconds 500
}
Write-Output "qemu stopped"
