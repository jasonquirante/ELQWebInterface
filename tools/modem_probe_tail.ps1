# Probe modem on COM11 and then tail for 15 seconds
$portName = 'COM11'
$baud = 115200
$commands = @('AT', 'AT+CGMM', 'AT+CGSN', 'AT+CPIN?', 'AT+CSQ', 'AT+CREG?', 'AT+CEREG?')
$p = New-Object System.IO.Ports.SerialPort $portName, $baud, 'None', 8, 'One'
$p.ReadTimeout = 500
$p.NewLine = "`r`n"
try {
  Write-Output "Opening $portName @ $baud"
  $p.Open()
  Start-Sleep -Milliseconds 200

  function SendCmd($cmd, $waitMs) {
    $p.DiscardInBuffer()
    $p.Write($cmd + "`r")
    Start-Sleep -Milliseconds 150
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $out = ""
    while ($sw.ElapsedMilliseconds -lt $waitMs) {
      try { $chunk = $p.ReadExisting() } catch { $chunk = "" }
      if ($chunk.Length -gt 0) { $out += $chunk }
      Start-Sleep -Milliseconds 50
    }
    return $out.Trim()
  }

  foreach ($cmd in $commands) {
    Write-Output "\n>>> $cmd"
    $resp = SendCmd $cmd 1200
    if ([string]::IsNullOrEmpty($resp)) { Write-Output "(no response)" } else { Write-Output $resp }
  }

  Write-Output "\nTailing serial for 15 seconds (live data)..."
  $end = (Get-Date).AddSeconds(15)
  while ((Get-Date) -lt $end) {
    try { $d = $p.ReadExisting() } catch { $d = "" }
    if ($d.Length -gt 0) { Write-Output $d }
    Start-Sleep -Milliseconds 200
  }

  Write-Output "Done tailing. Closing port."
  $p.Close()
} catch {
  Write-Error "Exception: $($_.Exception.Message)"
  if ($p -and $p.IsOpen) { $p.Close() }
  exit 1
}
