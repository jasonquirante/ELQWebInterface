#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#include "gps.h"
#include "lte.h"
#include "web.h"

#define SD_CS 5

bool sdCardAvailable = false;

namespace {
bool serialUploadMode = false;
bool receivingFile = false;
File serialUploadFile;
String serialUploadPath;
size_t serialBytesRemaining = 0;
String serialLineBuffer;
bool atBridgeMode = false;
String atBridgeLineBuffer;
bool txPatternMode = false;
unsigned long lastTxPatternMs = 0;
bool setupComplete = false;
bool lteInitStarted = false;
unsigned long lteInitEarliestMs = 0;

bool isPathSafe(const String& path) {
  if (!path.startsWith("/")) {
    return false;
  }
  if (path.indexOf("..") >= 0) {
    return false;
  }
  return true;
}

bool ensureDirectoryTree(const String& fullPath) {
  if (!fullPath.startsWith("/")) {
    return false;
  }

  int idx = 1;
  while (true) {
    idx = fullPath.indexOf('/', idx);
    if (idx < 0) {
      break;
    }

    String partial = fullPath.substring(0, idx);
    if (partial.length() > 0 && !SD.exists(partial.c_str())) {
      if (!SD.mkdir(partial.c_str())) {
        return false;
      }
    }
    ++idx;
  }

  return true;
}

void startSerialUploadMode() {
  serialUploadMode = true;
  receivingFile = false;
  serialBytesRemaining = 0;
  serialLineBuffer = "";
  if (serialUploadFile) {
    serialUploadFile.close();
  }
  Serial.println("READY");
}

void finishSerialUploadMode() {
  if (serialUploadFile) {
    serialUploadFile.close();
  }
  receivingFile = false;
  serialBytesRemaining = 0;
  serialUploadPath = "";
  serialLineBuffer = "";
  serialUploadMode = false;
  Serial.println("OK");
}

void printGpsStatusToSerial() {
  const GpsData gps = gpsGetData();

  Serial.println("GPSSTATUS BEGIN");
  Serial.print("fix=");
  Serial.println(gps.hasFix ? "true" : "false");
  Serial.print("fixType=");
  Serial.println(gps.fixType);
  Serial.print("satellites=");
  Serial.println(gps.satellitesUsed);
  Serial.print("latitude=");
  Serial.println(gps.latitude, 6);
  Serial.print("longitude=");
  Serial.println(gps.longitude, 6);
  Serial.print("altitudeMeters=");
  Serial.println(gps.altitudeMeters, 2);
  Serial.print("speedKph=");
  Serial.println(gps.speedKph, 2);
  Serial.print("utcDate=");
  Serial.println(gps.utcDate);
  Serial.print("utcTime=");
  Serial.println(gps.utcTime);
  Serial.print("raw=");
  Serial.println(gps.rawInfo);
  Serial.println("GPSSTATUS END");
}

void printApnStatusToSerial() {
  Serial.print("APN ");
  Serial.println(lteGetApn());
}

bool isBootReady() {
  return setupComplete;
}

void startAtBridgeMode() {
  txPatternMode = false;
  atBridgeMode = true;
  atBridgeLineBuffer = "";
  Serial.println("[ATBRIDGE] Ready. Type AT commands and press Enter.");
  Serial.println("[ATBRIDGE] Type EXIT to leave bridge mode.");
}

void stopAtBridgeMode() {
  atBridgeMode = false;
  atBridgeLineBuffer = "";
  Serial.println("[ATBRIDGE] Exited bridge mode.");
}

void setTxPatternMode(bool enabled) {
  txPatternMode = enabled;
  lastTxPatternMs = 0;
  if (enabled) {
    atBridgeMode = false;
    atBridgeLineBuffer = "";
    Serial.println("[TXPATTERN] ON. Streaming pattern to modem RX line (GPIO17 -> R).");
    Serial.println("[TXPATTERN] Use TXPATTERN OFF to stop.");
  } else {
    Serial.println("[TXPATTERN] OFF.");
  }
}

void handleAtBridgeLine(const String& line) {
  if (line == "EXIT" || line == "ATEXIT") {
    stopAtBridgeMode();
    return;
  }

  if (line.length() == 0) {
    return;
  }

  Serial.print("[ATBRIDGE] >> ");
  Serial.println(line);

  const String command = line + "\r\n";
  const size_t written = lteRawWrite(reinterpret_cast<const uint8_t*>(command.c_str()), command.length());
  if (written != command.length()) {
    Serial.println("[ATBRIDGE] !! Failed to send full command");
  }
}

void setApnFromSerialLine(const String& line) {
  String trimmed = line.substring(4);
  trimmed.trim();
  if (trimmed.length() == 0) {
    Serial.println("ERROR empty APN");
    return;
  }

  if (lteDataModeActive()) {
    Serial.println("ERROR cannot change APN while data mode is active");
    return;
  }

  lteSetApn(trimmed);
  Serial.println("OK");
}

void handleSerialUploadLine(const String& line) {
  if (line == "BEGINSDUPLOAD") {
    startSerialUploadMode();
    return;
  }

  if (line == "ENDSDUPLOAD") {
    finishSerialUploadMode();
    return;
  }

  if (line == "GPSSTATUS") {
    printGpsStatusToSerial();
    return;
  }

  if (line == "APN?") {
    printApnStatusToSerial();
    return;
  }

  if (line == "STARTPPP") {
    if (!isBootReady()) {
      Serial.println("BUSY setup not complete");
      return;
    }
    lteStartInternetGateway();
    return;
  }

  if (line == "ATBRIDGE") {
    if (!isBootReady()) {
      Serial.println("BUSY setup not complete");
      return;
    }
    startAtBridgeMode();
    return;
  }

  if (line == "TXPATTERN ON") {
    setTxPatternMode(true);
    return;
  }

  if (line == "TXPATTERN OFF") {
    setTxPatternMode(false);
    return;
  }

  if (line.startsWith("APN ")) {
    setApnFromSerialLine(line);
    return;
  }

  if (line == "LSDIR") {
    File dir = SD.open("/www");
    if (!dir || !dir.isDirectory()) {
      Serial.println("[LSDIR] /www directory not found or not a directory");
      return;
    }

    File file = dir.openNextFile();
    while (file) {
      Serial.print("[LSDIR] ");
      Serial.print(file.name());
      Serial.print(" - ");
      Serial.print(file.size());
      Serial.println(" bytes");
      file.close();
      file = dir.openNextFile();
    }
    dir.close();
    Serial.println("[LSDIR] Done");
    return;
  }

  if (!serialUploadMode || receivingFile) {
    return;
  }

  if (!line.startsWith("FILE ")) {
    return;
  }

  const String payload = line.substring(5);
  const int separator = payload.lastIndexOf(' ');
  if (separator <= 0) {
    Serial.println("ERROR bad FILE command");
    return;
  }

  const String path = payload.substring(0, separator);
  const String sizeText = payload.substring(separator + 1);
  const size_t expectedBytes = static_cast<size_t>(sizeText.toInt());
  if (!isPathSafe(path) || expectedBytes == 0) {
    Serial.println("ERROR invalid path or size");
    return;
  }

  if (!ensureDirectoryTree(path)) {
    Serial.println("ERROR mkdir failed");
    return;
  }

  if (serialUploadFile) {
    serialUploadFile.close();
  }

  Serial.print("[UPLOAD-LOG] File exists before remove: ");
  Serial.println(SD.exists(path.c_str()) ? "yes" : "no");

  if (SD.exists(path.c_str())) {
    Serial.print("[UPLOAD-LOG] Removing existing file: ");
    Serial.print(path);
    Serial.print("... result: ");
    const bool removeResult = SD.remove(path.c_str());
    Serial.println(removeResult ? "success" : "failed");
  }

  // Retries help when the web task briefly has the same file open.
  serialUploadFile = File();
  for (int attempt = 1; attempt <= 20; ++attempt) {
    Serial.print("[UPLOAD-LOG] Opening: ");
    Serial.print(path);
    Serial.print(" attempt ");
    Serial.print(attempt);
    Serial.print("/20 (exists after remove: ");
    Serial.print(SD.exists(path.c_str()) ? "yes" : "no");
    Serial.println(")");

    serialUploadFile = SD.open(path.c_str(), FILE_WRITE);
    if (serialUploadFile) {
      break;
    }
    delay(150);
  }

  if (!serialUploadFile) {
    Serial.print("[ERROR] Failed to open file: ");
    Serial.print(path);
    Serial.println(" for writing.");
    Serial.println("[ERROR] Suggestion: pause web clients and retry upload.");
    return;
  }

  serialUploadPath = path;
  serialBytesRemaining = expectedBytes;
  receivingFile = true;
  Serial.println("FILEOK");
}

void processSerialUpload() {

  if (receivingFile) {
    size_t processedThisPass = 0;
    const size_t maxPerPass = 4096;
    while (serialBytesRemaining > 0 && Serial.available() > 0) {
      const size_t availableBytes = static_cast<size_t>(Serial.available());
      const size_t chunkSize = min(serialBytesRemaining, min(availableBytes, static_cast<size_t>(1024)));
      uint8_t buffer[1024];
      const size_t readCount = Serial.readBytes(reinterpret_cast<char*>(buffer), chunkSize);
      if (readCount == 0) {
        break;
      }

      if (serialUploadFile) {
        serialUploadFile.write(buffer, readCount);
      }
      serialBytesRemaining -= readCount;
      processedThisPass += readCount;
      if (processedThisPass >= maxPerPass) {
        break;
      }
    }

    if (serialBytesRemaining == 0) {
      if (serialUploadFile) {
        serialUploadFile.close();
      }
      Serial.print("DONE ");
      Serial.println(serialUploadPath);
      serialUploadPath = "";
      receivingFile = false;
    }
    return;
  }

  if (atBridgeMode) {
    while (lteRawAvailable() > 0) {
      const int modemByte = lteRawRead();
      if (modemByte >= 0) {
        Serial.write(static_cast<uint8_t>(modemByte));
      }
    }

    while (Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        if (atBridgeLineBuffer.length() > 0) {
          handleAtBridgeLine(atBridgeLineBuffer);
          atBridgeLineBuffer = "";
        }
        continue;
      }

      atBridgeLineBuffer += c;
      if (atBridgeLineBuffer.length() > 160) {
        atBridgeLineBuffer = "";
      }
    }

    delay(5);
    return;
  }

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (serialLineBuffer.length() > 0) {
        handleSerialUploadLine(serialLineBuffer);
        serialLineBuffer = "";
      }
      continue;
    }

    serialLineBuffer += c;
    if (serialLineBuffer.length() > 160) {
      serialLineBuffer = "";
    }
  }
}
}  // namespace

static const char* sdCardTypeToString(uint8_t cardType) {
  switch (cardType) {
    case CARD_MMC:
      return "MMC";
    case CARD_SD:
      return "SDSC";
    case CARD_SDHC:
      return "SDHC/SDXC";
    case CARD_NONE:
    default:
      return "UNKNOWN";
  }
}

static void printSdCardInfo() {
  const uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("SD diagnostics: No card detected.");
    return;
  }

  Serial.print("SD diagnostics: Card type = ");
  Serial.println(sdCardTypeToString(cardType));

  const uint64_t cardSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
  const uint64_t totalMB = SD.totalBytes() / (1024ULL * 1024ULL);
  const uint64_t usedMB = SD.usedBytes() / (1024ULL * 1024ULL);

  Serial.print("SD diagnostics: Card size (MB) = ");
  Serial.println((unsigned long)cardSizeMB);
  Serial.print("SD diagnostics: Total bytes (MB) = ");
  Serial.println((unsigned long)totalMB);
  Serial.print("SD diagnostics: Used bytes (MB) = ");
  Serial.println((unsigned long)usedMB);
}

static bool testSdWriteRead() {
  const char* path = "/test.txt";

  File testFile = SD.open(path, FILE_WRITE);
  if (!testFile) {
    Serial.println("Failed to open file for writing.");
    return false;
  }

  testFile.println("Hello from ESP32!");
  testFile.close();
  Serial.println("File written successfully.");

  File readFile = SD.open(path, FILE_READ);
  if (!readFile) {
    Serial.println("Failed to open file for reading.");
    return false;
  }

  Serial.println("Readback from /test.txt:");
  while (readFile.available()) {
    Serial.write(readFile.read());
  }
  Serial.println();
  readFile.close();

  return true;
}

static bool initializeSdCard() {
  SPI.begin(18, 19, 23, SD_CS);

  if (SD.begin(SD_CS)) {
    if (testSdWriteRead()) {
      printSdCardInfo();
      return true;
    }
    Serial.println("[SETUP WARN] SD mounted, but read/write verification failed.");
    return false;
  }

  Serial.println("[SETUP WARN] SD init failed at default speed; retrying at 1 MHz.");
  if (SD.begin(SD_CS, SPI, 1000000)) {
    if (testSdWriteRead()) {
      printSdCardInfo();
      return true;
    }
    Serial.println("[SETUP WARN] SD mounted at fallback speed, but read/write verification failed.");
    return false;
  }

  Serial.println("[SETUP WARN] SD init failed.");
  return false;
}

void setup() {
  Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n========== ELQDRONE STARTUP ==========\n");

  Serial.println("[SETUP] Starting Wi-Fi AP before modem and storage init...");
  webInit();
  setupComplete = true;
  Serial.println("[SETUP] Wi-Fi AP is live. Continuing background initialization.");

  Serial.println("[SETUP] Initializing SD card...");
  sdCardAvailable = initializeSdCard();
  Serial.println(sdCardAvailable ? "[SETUP] SD card ready." : "[SETUP] SD card unavailable; captive portal only.");

  // Defer LTE init to loop so serial upload mode can start promptly.
  lteInitEarliestMs = millis() + 30000;
  Serial.println("[SETUP] LTE init deferred to main loop.");
  Serial.println("[SETUP] LTE init grace period: 30s for serial commands/upload mode.");
  gpsInit();
  Serial.println("[SETUP] System ready.");
}

void loop() {
  processSerialUpload();
  if (serialUploadMode) {
    delay(1);
    return;
  }

  if (txPatternMode) {
    const unsigned long now = millis();
    if (now - lastTxPatternMs >= 200) {
      const char* pattern = "UATTEST12345\r\n";
      lteRawWrite(reinterpret_cast<const uint8_t*>(pattern), strlen(pattern));
      lastTxPatternMs = now;
    }
    delay(5);
    return;
  }

  if (atBridgeMode) {
    // Keep UART dedicated to bridge traffic; background tasks can contend for AT channel.
    delay(5);
    return;
  }

  if (!lteInitStarted && millis() >= lteInitEarliestMs) {
    lteInitStarted = true;
    lteInit();
    Serial.println("[SETUP] PPP gateway autostart is enabled when modem becomes responsive.");
  }

  lteLoop();
  gpsLoop();
  webLoop();
  delay(100);
}

