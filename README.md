# ELQDrone ESP32 PlatformIO Starter

This project scaffolds an ESP32 firmware base with:
- SD card initialization and write/read verification
- GPS via SIM7600G built-in GNSS over AT commands
- LTE (SIM7600G) AT diagnostics and status checks
- Plain Wi-Fi AP access without captive-portal redirects
- Clear pin mapping and extension points for later integration

## Project Structure

- `platformio.ini`
- `src/main.cpp`
- `src/gps.cpp`
- `src/lte.cpp`
- `src/portal.cpp`
- `include/gps.h`
- `include/lte.h`
- `include/portal.h`

## Pin Mapping

Current mapping (update as hardware evolves):
- SD card chip-select (`SD_CS`): GPIO 5
- LTE UART RX2/TX2: GPIO 16 / GPIO 17
- LTE PWRKEY: GPIO 4

Typical ESP32 VSPI defaults (if unchanged):
- SCK: GPIO 18
- MISO: GPIO 19
- MOSI: GPIO 23
- CS: GPIO 5

## What `main.cpp` Does

1. Starts serial at 115200 baud.
2. Tries to mount SD card at default SPI speed.
3. Retries at 1 MHz if default init fails.
4. Writes `Hello from ESP32!` to `/test.txt`.
5. Reopens `/test.txt` and prints content to serial monitor.
6. Initializes modules: GPS parser, LTE diagnostics, and the web server.

## GPS Feature

`gps.cpp` now:
- Uses SIM7600G GNSS commands (`AT+CGNSSPWR=1`, `AT+CGNSSINFO`, fallback `AT+CGPSINFO`).
- Prints GNSS status every 10 seconds.
- Reports when GNSS is still waiting for satellites.

No separate GPS UART wiring is required when using SIM7600G built-in GNSS.

## Build and Upload (PlatformIO)

From this folder:

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## Web Interface (Served From SD Card)

The ESP32 now runs an HTTP server and serves frontend files from SD card path `/www`.

API endpoints:
- `/gps` -> live GNSS fields (lat/lon/sats/fix/raw)
- `/netinfo` -> LTE state (RSSI, SIM ready, registration, attach, APN, IP)
- `/logs` -> SD logs (`/gps_log.csv`, `/sessions.log`)

Web server behavior:
- ESP32 starts a Wi-Fi AP: `ELQDrone`
- Open network (no password)
- Browse to `http://192.168.4.1`
- No captive-portal redirects are installed, so the AP behaves like a normal local-only network.

Modem data session behavior:
- The firmware now tries to open a SIM7600G PDP/network session using APN `internet`.
- If your carrier uses a different APN, update `LTE_APN` in `src/lte.cpp`.
- The AP itself is still local-only; the modem gets internet on the LTE side.

SD website files required:
- `/www/index.html`
- `/www/styles.css`
- `/www/app.js`

Result on SD should look like:
- `/www/index.html`
- `/www/styles.css`
- `/www/app.js`

### Upload `data/www` directly to SD card via ESP32 (no SD removal)

1. Upload firmware to ESP32:
	- `pio run -t upload`
2. Run the USB serial uploader:
	- `powershell -ExecutionPolicy Bypass -File tools/upload-www-over-serial.ps1`

Optional parameters:
- `-Port COM3` (default)
- `-SourceDir data/www` (default)
- `-BaudRate 115200` (default)

This script sends each website file over USB serial, and the ESP32 writes them to external SD card under `/www`.

## SD Debug Checklist

- Confirm SD wiring and CS pin (`SD_CS`) match hardware.
- Use FAT32 format on the card.
- If mount fails, keep the 1 MHz fallback in place.
- Verify serial output shows write and readback.

## Next Expansion Steps

- Add SD-backed GPS coordinate logging with timestamps.
- Add SIM7600G data session setup (APN) and HTTP/MQTT workflows.
- Add SD-backed session logs.
