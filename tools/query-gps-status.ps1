param(
  [string]$Port = "COM3",
  [int]$BaudRate = 115200
)

$ErrorActionPreference = "Stop"

$serial = New-Object System.IO.Ports.SerialPort $Port, $BaudRate, 'None', 8, 'One'
$serial.NewLine = "`n"
$serial.ReadTimeout = 500
$serial.WriteTimeout = 2000
$serial.DtrEnable = $false
$serial.RtsEnable = $false

function Read-UntilMarker {
  param(
    [string]$StartMarker,
    [string]$EndMarker,
    [int]$TimeoutMs = 15000
  )

  $buffer = ""
  $capturing = $false
  $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)

  while ([DateTime]::UtcNow -lt $deadline) {
    if ($serial.BytesToRead -gt 0) {
      $chunk = $serial.ReadExisting()
      if ($chunk) {
        $buffer += $chunk
        if (-not $capturing -and $buffer -match [regex]::Escape($StartMarker)) {
          $capturing = $true
        }

        if ($capturing) {
          $trimmed = $buffer.Substring($buffer.IndexOf($StartMarker))
          Write-Host -NoNewline $chunk
          if ($trimmed -match [regex]::Escape($EndMarker)) {
            return
          }
        }
      }
    }
  }

  throw "Timed out waiting for GPS status from ESP32."
}

try {
  $serial.Open()
  $serial.DiscardInBuffer()
  $serial.WriteLine("GPSSTATUS")
  Read-UntilMarker -StartMarker "GPSSTATUS BEGIN" -EndMarker "GPSSTATUS END" | Out-Null
} finally {
  if ($serial.IsOpen) {
    $serial.Close()
  }
}
