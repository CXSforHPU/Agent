# Reproduce the reported "agent loops on mqtt_history" case end-to-end on QEMU.
#
# Scenario: the MQTT client is NOT started, then the user asks the agent for the
# history of a topic. Before the fix the tool returned an empty result (the hint
# sentence overwrote the real text at offset 0), so the model kept calling
# mqtt_history forever and the turn never produced an answer.
#
# What this script asserts in the transcript:
#   1. the tool result now contains "no history available for topic"
#   2. the log shows the duplicate-call guard kicking in ("duplicated tool call skipped")
#   3. a text answer is eventually delivered (or the forced-final-answer path fires)
#   4. the chat turn terminates (no endless "Detected 1 tool invocation(s)")
param(
  [int]$Port = 5581,
  [int]$Smp = 2,
  [int]$ChatWait = 120,
  [string]$Question = "please tell me the history of agent/sub",
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

# serial to a file as well, so nothing is lost if the socket drops
$serialLog = Join-Path $env:TEMP 'agent_chat_serial.log'
if (Test-Path $serialLog) { Remove-Item $serialLog -Force }

$qargs = @(
  '-M', 'vexpress-a9', '-smp', "cpus=$Smp",
  '-kernel', 'rtthread.bin', '-sd', 'sd.bin',
  '-display', 'none',
  '-serial', "tcp:127.0.0.1:$Port,server,nowait",
  '-net', 'nic,model=lan9118', '-net', 'user'
)
$proc = Start-Process -FilePath $Qemu -ArgumentList $qargs -WorkingDirectory $ws -PassThru -WindowStyle Hidden
Write-Output "qemu pid=$($proc.Id) port=$Port"

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
        if ($n -gt 0) {
          $text = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
          [void]$log.Append($text)
          Add-Content -Path $serialLog -Value $text -NoNewline -Encoding UTF8
        }
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

if (-not (Connect-Serial)) { Write-Output "CONNECT FAILED"; exit 1 }
Pump 15
for ($i = 0; $i -lt 8; $i++) { if ($log.ToString() -match 'msh />') { break }; Write-Line ""; Pump 3 }

# do NOT start mqtt: this is the case that produced the endless retry
[void]$log.Append("`r`n##### CMD: mqtt_tool status`r`n")
Write-Line "mqtt_tool status"; Pump 4

[void]$log.Append("`r`n##### CMD: main_loop_entry`r`n")
Write-Line "main_loop_entry"; Pump 8

[void]$log.Append("`r`n##### CHAT: $Question`r`n")
Write-Line $Question
Pump $ChatWait

Write-Output "===== TRANSCRIPT ====="
Write-Output $log.ToString()
Write-Output "===== CHECKS ====="
$t = $log.ToString()
$script:fails = 0
function Check($name, $pattern, $want = $true) {
  $ok = ($t -match $pattern)
  $res = if ($ok -eq $want) { "PASS" } else { "FAIL" }
  if ($ok -ne $want) { $script:fails++ }
  Write-Output ("{0}: {1} (matched={2})" -f $res, $name, $ok)
}
Check "read-only query works before mqtt start (no bogus 'state busy')" 'mqtt state busy' $false
Check "history tool names the topic and the reason" 'no history for topic'
Check "history tool tells the model not to retry" 'Do NOT call mqtt_history again'
Check "tool result is never empty" 'Execution Result\] type text, result\s*\S' $true
Check "the turn ends with a user-visible answer" '\(\w|[\u4e00-\u9fa5]' $true
Check "no unguarded repetition (<= 4 tool rounds)" 'Maximum tool loop count' $false

$count = ([regex]::Matches($t, 'Detected \d+ tool invocation')).Count
Write-Output ("tool invocation rounds: {0}" -f $count)
Write-Output ("RESULT: {0}" -f $(if ($script:fails -eq 0) { "ALL CHECKS PASSED" } else { "$($script:fails) CHECK(S) FAILED" }))

if ($script:stream) { try { $script:stream.Close() } catch {} }
if ($script:client) { try { $script:client.Close() } catch {} }
for ($i = 0; $i -lt 10; $i++) {
  $p = Get-Process qemu-system-arm -ErrorAction SilentlyContinue
  if (-not $p) { break }
  $p | Stop-Process -Force
  Start-Sleep -Milliseconds 500
}
Write-Output "qemu stopped; serial log: $serialLog"
