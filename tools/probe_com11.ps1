$portName = 'COM11'
$baud = 115200
Write-Output "Probing $portName @ $baud"
$p = New-Object System.IO.Ports.SerialPort $portName, $baud, 'None', 8, 'One'
$p.ReadTimeout = 1500
try {
  $p.Open()
  Write-Output "Port opened"
  $p.DiscardInBuffer()
  Write-Output "Wrote AT"
  $p.Write("AT`r")
  Start-Sleep -Milliseconds 300
  $r = $p.ReadExisting()
  $p.Close()
  Write-Output "Port closed"
  if ($r.Length -eq 0) { Write-Output "(no response)" } else { Write-Output "RESPONSE:"; Write-Output $r }
} catch {
  Write-Output "EXCEPTION: $($_.Exception.Message)"
  if ($p -and $p.IsOpen) { $p.Close() }
  exit 1
}
