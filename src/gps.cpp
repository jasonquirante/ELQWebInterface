#include <gps.h>

#include <Arduino.h>

#include <SD.h>

#include "lte.h"

extern bool sdCardAvailable;

namespace {
constexpr uint32_t GPS_STATUS_INTERVAL_MS = 10000;
constexpr uint32_t GPS_LOG_INTERVAL_MS = 30000;
constexpr uint32_t GNSS_LTE_PAUSE_MIN_MS = 45000;

unsigned long lastStatusMs = 0;
unsigned long lastLogMs = 0;
unsigned long lastPowerRetryMs = 0;
unsigned long gnssEarliestInitMs = 0;
unsigned long gnssPauseUntilMs = 0;
bool gnssPowerOn = false;
bool gnssPausedForLte = false;
GpsData currentGpsData = {
    false, 0, 0, 0.0, 0.0, 0.0, 0.0, "", "", ""};

double ddmmToDecimal(const String& ddmm) {
  if (ddmm.length() < 4) {
    return 0.0;
  }

  const double value = ddmm.toDouble();
  const int degrees = static_cast<int>(value / 100.0);
  const double minutes = value - (degrees * 100.0);
  return static_cast<double>(degrees) + (minutes / 60.0);
}

bool looksLikeDecimalCoordinate(const String& value) {
  return value.length() > 0 && value.indexOf('N') == -1 && value.indexOf('S') == -1 &&
         value.indexOf('E') == -1 && value.indexOf('W') == -1;
}

bool splitCsv(const String& input, String* fields, size_t maxFields, size_t& outCount) {
  outCount = 0;
  int start = 0;
  while (start <= input.length() && outCount < maxFields) {
    const int comma = input.indexOf(',', start);
    if (comma == -1) {
      fields[outCount++] = input.substring(start);
      return true;
    }
    fields[outCount++] = input.substring(start, comma);
    start = comma + 1;
  }
  return outCount > 0;
}

String extractGnsLine(const String& response, const char* prefix) {
  const int start = response.indexOf(prefix);
  if (start == -1) {
    return "";
  }

  int end = response.indexOf('\n', start);
  if (end == -1) {
    end = response.length();
  }

  String line = response.substring(start, end);
  line.trim();
  return line;
}

bool ensureGnssPowerOn() {
  if (gnssPausedForLte && millis() < gnssPauseUntilMs) {
    return false;
  }

  if (gnssPowerOn) {
    return true;
  }

  const unsigned long now = millis();
  if (now - lastPowerRetryMs < 5000) {
    return false;
  }
  lastPowerRetryMs = now;

  if (!lteIsResponsive()) {
    Serial.println("[GPS] Waiting for LTE modem before enabling GNSS...");
    return false;
  }

  if (now < gnssEarliestInitMs) {
    return false;
  }

  String response;

  // Check if GNSS is already on
  if (lteSendCommand("AT+CGNSPWR?", response, 2000) && response.indexOf(": 1") != -1) {
    gnssPowerOn = true;
    Serial.println("[GPS] GNSS already enabled.");
    return true;
  }

  // Try primary power command
  if (lteSendCommand("AT+CGNSPWR=1", response, 4000)) {
    gnssPowerOn = true;
    Serial.println("[GPS] GNSS power enabled via AT+CGNSPWR=1.");
    return true;
  }

  // Fallback: Try alternate command
  if (lteSendCommand("AT+CGPS=1", response, 4000)) {
    gnssPowerOn = true;
    Serial.println("[GPS] GNSS power enabled via AT+CGPS=1.");
    return true;
  }

  Serial.println("[GPS] Failed to enable GNSS. Will retry.");
  return false;
}

bool lteNeedsPriority(const LteData& lte) {
  if (!lte.responsive) {
    return true;
  }

  // Keep GNSS available even when LTE registration is unstable.
  // The modem can still acquire satellites without full packet attach.
  return false;
}

void enforceLteFirstCoexistence() {
  const unsigned long now = millis();
  const LteData lte = lteGetData();
  const bool weakOrSearching = lteNeedsPriority(lte);

  if (weakOrSearching) {
    if (!gnssPausedForLte) {
      gnssPausedForLte = true;
      gnssPauseUntilMs = now + GNSS_LTE_PAUSE_MIN_MS;
      if (gnssPowerOn) {
        String response;
        if (lteSendCommand("AT+CGNSPWR=0", response, 2500)) {
          gnssPowerOn = false;
          Serial.println("[GPS] GNSS paused to prioritize LTE stability.");
        }
      }
    } else if (now >= gnssPauseUntilMs) {
      gnssPauseUntilMs = now + 15000;
    }
    return;
  }

  if (gnssPausedForLte && now >= gnssPauseUntilMs) {
    gnssPausedForLte = false;
    Serial.println("[GPS] LTE recovered. GNSS may resume.");
  }
}

void parseGnsInfo(const String& line) {
  currentGpsData.rawInfo = line;
  currentGpsData.hasFix = false;
  currentGpsData.latitude = 0.0;
  currentGpsData.longitude = 0.0;
  currentGpsData.speedKph = 0.0;
  currentGpsData.satellitesUsed = 0;

  const int colon = line.indexOf(':');
  if (colon == -1 || colon + 1 >= line.length()) {
    return;
  }

  const String payload = line.substring(colon + 1);
  String fields[20];
  size_t count = 0;
  if (!splitCsv(payload, fields, 20, count)) {
    return;
  }

  if (line.startsWith("+CGNSINF:")) {
    if (count < 16) {
      return;
    }
    const int fixStatus = fields[1].toInt();
    currentGpsData.fixType = fields[8].toInt();
    currentGpsData.satellitesUsed = fields[15].toInt();
    currentGpsData.utcDate = fields[2].length() >= 8 ? fields[2].substring(0, 8) : fields[2];
    currentGpsData.utcTime = fields[2].length() >= 14 ? fields[2].substring(8, 14) : fields[2];
    currentGpsData.altitudeMeters = fields[5].toDouble();
    currentGpsData.speedKph = fields[6].toDouble();

    if (looksLikeDecimalCoordinate(fields[3]) && looksLikeDecimalCoordinate(fields[4])) {
      currentGpsData.latitude = fields[3].toDouble();
      currentGpsData.longitude = fields[4].toDouble();
      currentGpsData.hasFix = fixStatus == 1;
    }
    return;
  }

  if (line.startsWith("+CGPSINFO:")) {
    if (count < 8) {
      return;
    }

    const String latValue = fields[0];
    const String latHemisphere = fields[1];
    const String lonValue = fields[2];
    const String lonHemisphere = fields[3];

    if (latValue.length() == 0 || lonValue.length() == 0) {
      return;
    }

    double lat = ddmmToDecimal(latValue);
    double lon = ddmmToDecimal(lonValue);

    if (latHemisphere.equalsIgnoreCase("S")) {
      lat = -lat;
    }
    if (lonHemisphere.equalsIgnoreCase("W")) {
      lon = -lon;
    }

    currentGpsData.fixType = 1;
    currentGpsData.latitude = lat;
    currentGpsData.longitude = lon;
    currentGpsData.hasFix = true;
    currentGpsData.utcDate = fields[4];
    currentGpsData.utcTime = fields[5];
    currentGpsData.altitudeMeters = fields[6].toDouble();
    currentGpsData.speedKph = fields[7].toDouble();
    return;
  }

  if (count < 12) {
    return;
  }

  currentGpsData.fixType = fields[0].toInt();
  currentGpsData.satellitesUsed = fields[1].toInt();

  if (fields[4].length() > 0 && fields[6].length() > 0) {
    double lat = ddmmToDecimal(fields[4]);
    double lon = ddmmToDecimal(fields[6]);

    if (fields[5].equalsIgnoreCase("S")) {
      lat = -lat;
    }
    if (fields[7].equalsIgnoreCase("W")) {
      lon = -lon;
    }

    currentGpsData.latitude = lat;
    currentGpsData.longitude = lon;
    currentGpsData.hasFix = true;
  }

  currentGpsData.utcDate = fields[8];
  currentGpsData.utcTime = fields[9];
  currentGpsData.altitudeMeters = fields[10].toDouble();
  currentGpsData.speedKph = fields[11].toDouble();
}

void maybeLogGpsToSd() {
  if (!currentGpsData.hasFix) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastLogMs < GPS_LOG_INTERVAL_MS) {
    return;
  }
  lastLogMs = now;

  File logFile = SD.open("/gps_log.csv", FILE_APPEND);
  if (!logFile) {
    Serial.println("[GPS] Failed to open /gps_log.csv for append.");
    return;
  }

  logFile.print(currentGpsData.utcDate);
  logFile.print(",");
  logFile.print(currentGpsData.utcTime);
  logFile.print(",");
  logFile.print(currentGpsData.latitude, 6);
  logFile.print(",");
  logFile.print(currentGpsData.longitude, 6);
  logFile.print(",");
  logFile.print(currentGpsData.altitudeMeters, 2);
  logFile.print(",");
  logFile.print(currentGpsData.speedKph, 2);
  logFile.print(",");
  logFile.println(currentGpsData.satellitesUsed);
  logFile.close();
}

bool pollGnssLocation(bool logToSerial) {
  if (lteDataModeActive()) {
    if (logToSerial) {
      Serial.println("[GPS] Skipping GNSS refresh while PPP/data mode is active.");
    }
    return false;
  }

  String response;
  const char* commandCandidates[] = {
      "AT+CGNSINF",
      "AT+CGNSSINFO",
      "AT+CGPSINFO",
  };

  const char* lineCandidates[] = {
      "+CGNSINF:",
      "+CGNSSINFO:",
      "+CGPSINFO:",
  };

  for (size_t i = 0; i < 3; ++i) {
    if (!lteSendCommand(commandCandidates[i], response, 2000)) {
      continue;
    }

    const String line = extractGnsLine(response, lineCandidates[i]);
    if (line.length() == 0) {
      continue;
    }

    const int colon = line.indexOf(':');
    if (colon != -1) {
      String payload = line.substring(colon + 1);
      payload.trim();
      payload.replace(",", "");
      if (payload.length() == 0) {
        continue;
      }
    }

    parseGnsInfo(line);
    gnssPowerOn = true;

    if (logToSerial) {
      Serial.print("[GPS] ");
      Serial.println(line);
      if (currentGpsData.hasFix) {
        Serial.print("[GPS] Fix: YES  Lat: ");
        Serial.print(currentGpsData.latitude, 6);
        Serial.print("  Lon: ");
        Serial.print(currentGpsData.longitude, 6);
        Serial.print("  Sats: ");
        Serial.println(currentGpsData.satellitesUsed);
      } else {
        Serial.println("[GPS] Fix: NO (GNSS running, waiting for satellites)");
      }
    }
    return true;
  }

  return false;
}

void printGpsStatus() {
  const bool powerReady = ensureGnssPowerOn();
  if (!powerReady) {
    Serial.println("[GPS] GNSS power command unavailable; trying location queries anyway.");
  }

  if (pollGnssLocation(true)) {
    return;
  }

  Serial.println("[GPS] No GNSS location response yet.");
}
}  // namespace

void gpsInit() {
  Serial.println("[GPS] Using SIM7600G built-in GNSS over AT commands.");
  gnssEarliestInitMs = millis() + 8000;
}

void gpsLoop() {
  if (lteDataModeActive()) {
    return;
  }

  const unsigned long now = millis();
  enforceLteFirstCoexistence();

  if (now - lastStatusMs >= GPS_STATUS_INTERVAL_MS) {
    lastStatusMs = now;
    printGpsStatus();
  }

  maybeLogGpsToSd();
}

bool gpsRefreshNow() {
  enforceLteFirstCoexistence();
  (void)ensureGnssPowerOn();
  return pollGnssLocation(false);
}

GpsData gpsGetData() {
  return currentGpsData;
}
