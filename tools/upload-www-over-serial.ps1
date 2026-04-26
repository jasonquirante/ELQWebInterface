param(
  [string]$Port = "COM3",
  [string]$SourceDir = "data/www",
  [int]$BaudRate = 115200,
  [int]$ReadyTimeoutMs = 180000,
  [int]$TxChunkSize = 256,
  [int]$InterChunkDelayMs = 1,
  [int]$DoneTimeoutMs = 600000,
  [switch]$ResetOnOpen
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -Path $SourceDir)) {
  throw "Source directory not found: $SourceDir"
}

$root = (Resolve-Path $SourceDir).Path
$portName = $Port

$serial = New-Object System.IO.Ports.SerialPort $portName, $BaudRate, 'None', 8, 'One'
$serial.NewLine = "`n"
$serial.ReadTimeout = 500
$serial.WriteTimeout = 5000
$serial.DtrEnable = $false
$serial.RtsEnable = $false

function Send-Line {
  param([string]$Text)
  $serial.WriteLine($Text)
}

function Wait-ForPattern {
  param(
    [string]$Pattern,
    [int]$TimeoutMs = 10000
  )

  $buffer = ""
  $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    if ($serial.BytesToRead -gt 0) {
      $chunk = $serial.ReadExisting()
      if ($chunk) {
        $buffer += $chunk
        Write-Host -NoNewline $chunk
        if ($buffer -match $Pattern) {
          return $true
        }
      }
    }
  }

  throw "Timed out waiting for '$Pattern'."
}

try {
  $serial.Open()

  if ($ResetOnOpen) {
    Write-Host "Pulsing RTS to reset target before upload..."
    $serial.DtrEnable = $false
    $serial.RtsEnable = $true
    Start-Sleep -Milliseconds 120
    $serial.RtsEnable = $false
    Start-Sleep -Milliseconds 600
  }

  Write-Host "Opened $portName. Starting SD upload mode..."
  $readyBuffer = ""
  $readyDeadline = [DateTime]::UtcNow.AddMilliseconds($ReadyTimeoutMs)
  $nextBeginSend = [DateTime]::UtcNow

  while ([DateTime]::UtcNow -lt $readyDeadline) {
    if ([DateTime]::UtcNow -ge $nextBeginSend) {
      Send-Line "BEGINSDUPLOAD"
      $nextBeginSend = [DateTime]::UtcNow.AddMilliseconds(1000)
    }

    if ($serial.BytesToRead -gt 0) {
      $chunk = $serial.ReadExisting()
      if ($chunk) {
        $readyBuffer += $chunk
        Write-Host -NoNewline $chunk
        if ($readyBuffer -match "READY") {
          break
        }
      }
    }
  }

  if ($readyBuffer -notmatch "READY") {
    throw "Timed out waiting for 'READY'."
  }

  $dirs = Get-ChildItem -Path $root -Directory -Recurse | Sort-Object FullName
  foreach ($dir in $dirs) {
    $rel = $dir.FullName.Substring($root.Length).TrimStart('\\') -replace '\\','/'
    $sdDir = "/www/$rel"
    Write-Host "Directory already implied by file upload: $sdDir"
  }

  $files = Get-ChildItem -Path $root -File -Recurse | Sort-Object FullName
  foreach ($file in $files) {
    $rel = $file.FullName.Substring($root.Length).TrimStart('\\') -replace '\\','/'
    $sdPath = "/www/$rel"
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)

    Write-Host "Uploading $sdPath ($($bytes.Length) bytes)..."
    Send-Line ("FILE {0} {1}" -f $sdPath, $bytes.Length)
    Wait-ForPattern "FILEOK" 10000 | Out-Null

    $offset = 0
    while ($offset -lt $bytes.Length) {
      $chunkSize = [Math]::Min($TxChunkSize, $bytes.Length - $offset)
      $serial.BaseStream.Write($bytes, $offset, $chunkSize)
      if ($InterChunkDelayMs -gt 0) {
        Start-Sleep -Milliseconds $InterChunkDelayMs
      }
      $offset += $chunkSize
    }
    $serial.BaseStream.Flush()

    Wait-ForPattern ([regex]::Escape("DONE $sdPath")) $DoneTimeoutMs | Out-Null
  }

  Send-Line "ENDSDUPLOAD"
  Wait-ForPattern "OK" 10000 | Out-Null
  Write-Host "Upload complete. Files are now on SD under /www."
} finally {
  if ($serial.IsOpen) {
    $serial.Close()
  }
}
