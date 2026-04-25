param([string]$Port = "COM4")

$file = Get-Item "data/www/index.html"
$fileData = [System.IO.File]::ReadAllBytes($file.FullName)

$serial = New-Object System.IO.Ports.SerialPort($Port, 115200, 8, 1, 1)
$serial.Open()
Start-Sleep -Milliseconds 500

# Wait for READY
$buf = ""
$timeout = [System.Diagnostics.Stopwatch]::StartNew()
while ($timeout.ElapsedMilliseconds -lt 5000) {
  if ($serial.BytesToRead -gt 0) {
    $char = [char]$serial.ReadByte()
    $buf += $char
    if ($buf -match "READY") { break }
  }
  Start-Sleep -Milliseconds 10
}

Write-Host "Sending FILE command for index.html $($fileData.Length) bytes..."
$cmd = "FILE /www/index.html $($fileData.Length)`n"
$serial.Write($cmd, 0, $cmd.Length)

# Wait for FILEOK
Start-Sleep -Milliseconds 100
$buf = ""
$timeout = [System.Diagnostics.Stopwatch]::StartNew()
while ($timeout.ElapsedMilliseconds -lt 5000) {
  if ($serial.BytesToRead -gt 0) {
    $char = [char]$serial.ReadByte()
    Write-Host -NoNewline $char
    $buf += $char
    if ($buf.EndsWith("FILEOK")) { break }
  }
  Start-Sleep -Milliseconds 10
}

Write-Host "`n`nSending file data..."
$serial.Write($fileData, 0, $fileData.Length)

# Wait for DONE
Start-Sleep -Milliseconds 100
$buf = ""
$timeout = [System.Diagnostics.Stopwatch]::StartNew()
while ($timeout.ElapsedMilliseconds -lt 5000) {
  if ($serial.BytesToRead -gt 0) {
    $char = [char]$serial.ReadByte()
    Write-Host -NoNewline $char
    $buf += $char
    if ($buf.EndsWith("DONE")) { break }
  }
  Start-Sleep -Milliseconds 10
}

Write-Host "`n`nDone!"
