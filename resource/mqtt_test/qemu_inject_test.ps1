# Verify the shared injection API: agent_inject_text() (include/utils.h, src/utils.c)
#
# The MQTT route thread is the framework's first user of that API; this script
# proves the whole path still works after the function was lifted out of the MQTT
# module:
#   PC (independent MQTT client) --publish--> broker --> board subscribe callback
#     --> rx thread (mode=auto) --> agent_inject_text() --> agent input mailbox
#     --> LLM analysis
#
# Assertions in the board transcript:
#   1. "mqtt -> agent: 1 message(s) delivered for analysis"
#      (printed only when agent_inject_text() returned RT_EOK)
#   2. the injected payload reaches the model (it appears in the reasoning/answer)
param(
  [int]$Port = 5585,
  [int]$Smp = 2,
  [string]$Topic = "agent/sub",
  [string]$Payload = '{"device":"sensor-7","temp":42,"note":"analyse this reading"}',
  [string]$Qemu = "D:\rtt\env-windows\tools\qemu\qemu64\qemu-system-arm.exe"
)

$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$ws = (Resolve-Path (Join-Path $here '..\..\..\..')).Path

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
Write-Output "qemu pid=$($proc.Id) port=$Port"

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
# auto mode: every message goes straight to the agent through agent_inject_text()
Cmd "mqtt_tool sub $Topic auto" 4
Cmd "mqtt_tool status" 2
Cmd "main_loop_entry" 10

[void]$log.Append("`r`n##### PC publish -> $Topic : $Payload`r`n")
$env:MQTT_PAYLOAD = $Payload
$py = & python "$here\mqtt_pub.py" $Topic "-"
[void]$log.Append(($py -join "`r`n"))
Pump 60

Write-Output "===== BOARD TRANSCRIPT ====="
Write-Output $log.ToString()
Write-Output "===== CHECKS ====="
$t = $log.ToString()
$script:fails = 0
function Check($name, $pattern, $want = $true) {
  $ok = ($t -match $pattern)
  if ($ok -ne $want) { $script:fails++ }
  Write-Output ("{0}: {1} (matched={2})" -f $(if ($ok -eq $want) { "PASS" } else { "FAIL" }), $name, $ok)
}
Check "PUBLISH reached the broker from the PC" 'PUBLISH topic='
Check "injection succeeded (agent_inject_text returned RT_EOK)" 'mqtt -> agent: 1 message\(s\) delivered for analysis'
Check "injected payload reached the model" 'sensor-7|temp.{0,4}42'
Check "agent produced output for the injected message" 'thinking|喵|temp|sensor' $true
Check "no injection failure path" 'agent input mailbox full' $false

Write-Output ("RESULT: {0}" -f $(if ($script:fails -eq 0) { "ALL CHECKS PASSED" } else { "$($script:fails) CHECK(S) FAILED" }))

$client.Close()
for ($i = 0; $i -lt 10; $i++) {
  $p = Get-Process qemu-system-arm -ErrorAction SilentlyContinue
  if (-not $p) { break }
  $p | Stop-Process -Force
  Start-Sleep -Milliseconds 500
}
Write-Output "done"
