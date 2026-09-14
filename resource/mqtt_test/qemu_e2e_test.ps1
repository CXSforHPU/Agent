# Test 4: agent-driven MQTT, verified from an independent MQTT client on the PC.
#   1) boot, start mqtt tool, subscribe device/1/cmd (auto)
#   2) PC subscribes to device/1/cmd as an independent observer
#   3) start agent; ask it in natural language to publish a command via its mqtt tool
#   4) PC must receive the command -> proves agent-initiated publish works
#   5) also push a message from the PC to agent/sub and capture the agent's analysis
param([int]$Port = 5582, [int]$Smp = 2)

$here = $PSScriptRoot
$ErrorActionPreference = 'Continue'
$qemu = "D:\rtt\env-windows\tools\qemu\qemu64\qemu-system-arm.exe"
$ws = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path

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
$proc = Start-Process -FilePath $qemu -ArgumentList $qargs -WorkingDirectory $ws -PassThru -WindowStyle Hidden
Write-Output "qemu pid=$($proc.Id) port=$Port smp=$Smp"

$client = $null
for ($i = 0; $i -lt 60; $i++) {
  try { $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port); break }
  catch { Start-Sleep -Milliseconds 500 }
}
if ($null -eq $client) { Write-Output "CONNECT FAILED"; exit 1 }
$stream = $client.GetStream()
$client.ReceiveTimeout = 1000
$log = New-Object System.Text.StringBuilder

function Pump([int]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $buf = New-Object byte[] 8192
  while ((Get-Date) -lt $deadline) {
    try {
      if ($stream.DataAvailable) {
        $n = $stream.Read($buf, 0, $buf.Length)
        if ($n -gt 0) { [void]$log.Append([System.Text.Encoding]::UTF8.GetString($buf, 0, $n)) }
      } else { Start-Sleep -Milliseconds 50 }
    } catch { Start-Sleep -Milliseconds 50 }
  }
}

function Write-Line([string]$c) {
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($c + "`r`n")
  try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush() } catch {}
}

function Cmd([string]$c, [int]$wait = 3) {
  [void]$log.Append("`r`n##### CMD: $c`r`n")
  Write-Line $c
  Pump $wait
}

Pump 15
Cmd "mqtt_tool start" 8
Cmd "mqtt_tool sub device/1/cmd auto" 4
Cmd "mqtt_tool status" 2

# independent observer on the PC
$subOut = Join-Path $ws "pc_sub_out.txt"
Remove-Item $subOut -ErrorAction SilentlyContinue
$subProc = Start-Process -FilePath "python" -ArgumentList @("$ws\mqtt_sub_test.py", "device/1/cmd", "150") -PassThru -WindowStyle Hidden -RedirectStandardOutput $subOut -RedirectStandardError "$subOut.err"
Write-Output "pc subscriber pid=$($subProc.Id)"
Start-Sleep -Seconds 4

# start the agent; from now on the CLI channel owns the console
Cmd "main_loop_entry" 12

# natural-language request -> the model should call its MQTT tools
$ask = 'Please publish the command {"led":1,"src":"agent"} to the MQTT topic device/1/cmd, and after that tell me the publish result.'
[void]$log.Append("`r`n##### ASK: $ask`r`n")
Write-Line $ask
Pump 75

# inbound message -> agent analysis
[void]$log.Append("`r`n##### PC publish -> agent/sub`r`n")
$py = & python "$here\mqtt_pub.py" "agent/sub" '{"device":"sensor-7","temp":42,"note":"analyse this reading"}'
[void]$log.Append(($py -join "`r`n"))
Pump 60

Write-Output "===== BOARD TRANSCRIPT ====="
Write-Output $log.ToString()
Write-Output "===== PC SUBSCRIBER (independent MQTT client) ====="
if (Test-Path $subOut) { Get-Content $subOut | Write-Output }
if (Test-Path "$subOut.err") { Get-Content "$subOut.err" | Write-Output }

if (-not $subProc.HasExited) { $subProc | Stop-Process -Force }
$client.Close()
for ($i = 0; $i -lt 10; $i++) {
  $p = Get-Process qemu-system-arm -ErrorAction SilentlyContinue
  if (-not $p) { break }
  $p | Stop-Process -Force
  Start-Sleep -Milliseconds 500
}
Write-Output "done"
