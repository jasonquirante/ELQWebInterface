# Test uploading files in different order to diagnose issue

param([string]$Port = "COM4")

$ErrorActionPreference = "Stop"

function WaitForPattern {
  param([string]$Pattern, [int]$Timeout = 5000)
  $lineBuffer = ""
  $startTime = [System.Diagnostics.Stopwatch]::StartNew()
  
  while ($startTime.ElapsedMilliseconds -lt $Timeout) {
    if ($port.BytesToRead -gt 0) {
      $char = [char]$port.ReadByte()
      $lineBuffer += $char
      if ($lineBuffer.EndsWith($Pattern)) {
        return $lineBuffer
      }
    }
    [System.Threading.Thread]::Sleep(10)
  }
  throw "Timed out waiting for '$Pattern'."
}

function UploadFile {
  param([string]$FilePath, [string]$DestPath)
  
  $file = Get-Item -Path $FilePath -ErrorAction Stop
  $fileSize = $file.Length
  
  Write-Host "Uploading $FilePath ($fileSize bytes) → $DestPath..."
  
  # Send FILE command
  $fileCmd = "FILE $DestPath $fileSize`n"
  $port.Write($fileCmd, 0, $fileCmd.Length)
  
  WaitForPattern "FILEOK" | Out-Null
  
  # Send file data
  $data = [System.IO.File]::ReadAllBytes($FilePath)
  $port.Write($data, 0, $data.Length)
  
  WaitForPattern "DONE" 5000 | Out-Null
  Write-Host "✅ OK`n"
}

try {
  $port = New-Object System.IO.Ports.SerialPort($Port, 115200)
  $port.ReadTimeout = 2000
  $port.Open()
  
  Start-Sleep -Milliseconds 500
  WaitForPattern "READY" | Out-Null
  Write-Host "Connected`n"
  
  # Try uploading in REVERSE order
  Write-Host "=== UPLOADING IN REVERSE ORDER ==="
  
  UploadFile "data/www/styles.css" "/www/styles.css"
  UploadFile "data/www/favicon.svg" "/www/favicon.svg"
  UploadFile "data/www/app.js" "/www/app.js"
  UploadFile "data/www/index.html" "/www/index.html"
  
  Write-Host "✅ ALL FILES UPLOADED"
  
}
catch {
  throw $_
}
