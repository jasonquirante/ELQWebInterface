# Attempt network attach checks and PPP dial on COM11
$portName = 'COM11'
$baud = 115200
$p = New-Object System.IO.Ports.SerialPort $portName, $baud, 'None', 8, 'One'
$p.ReadTimeout = 500
$p.NewLine = "`r`n"
try {
  Write-Output "Opening $portName @ $baud"
  $p.Open()
  Start-Sleep -Milliseconds 200

  function SendCmd($cmd, $waitMs) {
    $p.DiscardInBuffer()
    Write-Output "\n>>> $cmd"
    $p.Write($cmd + "`r")
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $out = ""
    while ($sw.ElapsedMilliseconds -lt $waitMs) {
      try { $chunk = $p.ReadExisting() } catch { $chunk = "" }
      if ($chunk.Length -gt 0) { $out += $chunk }
      Start-Sleep -Milliseconds 50
    }
    if ($out.Length -eq 0) { return "(no response)" } else { return $out.Trim() }
  }

  # Basic attach/APN checks
  $resp = SendCmd 'AT+CGATT?' 1200
  Write-Output $resp
  $resp = SendCmd 'AT+CGDCONT?' 1200
  Write-Output $resp

  # Try PPP dial commands (be cautious - modem may enter data mode)
  $connectCommands = @('ATD*99***1#','ATD*99#','AT+CGDATA="PPP",1')
  foreach ($cmd in $connectCommands) {
    Write-Output "\nAttempting: $cmd"
    $p.DiscardInBuffer()
    $p.Write($cmd + "`r")

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $acc = ""
    $found = $false
    while ($sw.ElapsedMilliseconds -lt 20000) {
      try { $chunk = $p.ReadExisting() } catch { $chunk = "" }
      if ($chunk.Length -gt 0) { $acc += $chunk; Write-Output $chunk }
      if ($acc -match 'CONNECT') { $found = 'CONNECT'; break }
      if ($acc -match 'NO CARRIER' -or $acc -match 'ERROR' -or $acc -match 'NO DIALTONE' -or $acc -match 'NO ANSWER') { $found = 'FAIL'; break }
      Start-Sleep -Milliseconds 100
    }

    if ($found -eq 'CONNECT') {
      Write-Output "=== CONNECT detected ==="
      # Try to return to command mode: send escape sequence + ATH
      Start-Sleep -Milliseconds 1000
      Write-Output "Sending escape sequence '+++'"
      $p.Write('+++')
      Start-Sleep -Milliseconds 1200
      Write-Output "Sending ATH to hang up"
      $p.Write('ATH`r')
      Start-Sleep -Milliseconds 800
      $dump = $p.ReadExisting()
      if ($dump.Length -gt 0) { Write-Output $dump }
      Write-Output "Attempted to hang up and return to command mode."
      break
    } else {
      if ($acc.Length -eq 0) { Write-Output "(no response)" } else { Write-Output "Dial response:"; Write-Output $acc }
    }

    Start-Sleep -Milliseconds 300
  }

  Write-Output "\nDone. Closing port."
  $p.Close()
} catch {
  Write-Output "Exception: $($_.Exception.Message)"
  if ($p -and $p.IsOpen) { $p.Close() }
  exit 1
}
