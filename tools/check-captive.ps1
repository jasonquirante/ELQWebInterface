param(
  [string]$Gateway = "192.168.4.1"
)

$ErrorActionPreference = "Stop"

function Invoke-CaptiveCheck {
  param(
    [string]$Path
  )

  $url = "http://$Gateway$Path"
  $response = $null

  try {
    $request = [System.Net.HttpWebRequest]::Create($url)
    $request.Method = "GET"
    $request.AllowAutoRedirect = $false
    $request.Timeout = 5000
    $response = $request.GetResponse()
  }
  catch [System.Net.WebException] {
    if ($_.Exception.Response -ne $null) {
      $response = $_.Exception.Response
    } else {
      Write-Host ("[FAIL] {0,-30} -> network error: {1}" -f $Path, $_.Exception.Message)
      return
    }
  }

  try {
    $status = [int]$response.StatusCode
    $location = $response.Headers["Location"]
    $captiveHeader = $response.Headers["Captive-Portal"]

    if ($status -eq 302 -or $status -eq 301) {
      Write-Host ("[OK]   {0,-30} -> {1} Location={2}" -f $Path, $status, $location)
    } else {
      Write-Host ("[WARN] {0,-30} -> {1} Captive-Portal={2}" -f $Path, $status, $captiveHeader)
    }
  }
  finally {
    $response.Close()
  }
}

Write-Host ("Checking captive portal at http://{0}" -f $Gateway)
Write-Host ""

$probePaths = @(
  "/generate_204",
  "/gen_204",
  "/hotspot-detect.html",
  "/library/test/success.html",
  "/ncsi.txt",
  "/connecttest.txt",
  "/redirect",
  "/fwlink"
)

foreach ($path in $probePaths) {
  Invoke-CaptiveCheck -Path $path
}

Write-Host ""
Write-Host "Portal status endpoint:"
try {
  $statusResponse = Invoke-RestMethod -Uri ("http://{0}/portal/status" -f $Gateway) -Method Get -TimeoutSec 5
  $json = $statusResponse | ConvertTo-Json -Compress
  Write-Host ("[INFO] /portal/status -> {0}" -f $json)
}
catch {
  Write-Host ("[FAIL] /portal/status -> {0}" -f $_.Exception.Message)
}
