#include "web.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include "gps.h"
#include "lte.h"

extern bool sdCardAvailable;

namespace {
constexpr const char* AP_SSID = "RESQLinkDrone";
constexpr uint8_t DNS_PORT = 53;
constexpr bool FORCE_CAPTIVE_PORTAL = false;
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_NETMASK(255, 255, 255, 0);

WebServer server(80);
DNSServer dnsServer;
bool dnsCaptiveActive = false;

bool isInternetReady() {
  const LteData lte = lteGetData();
  return lte.dataConnected && lte.ipAddress.length() > 0 && lte.ipAddress != "0.0.0.0";
}

bool isModemAlive() {
  return lteIsResponsive();
}

bool isNetworkRegistered(const LteData& lte) {
  const bool csRegistered = (lte.creg == 1 || lte.creg == 5);
  const bool psRegistered = (lte.cereg == 1 || lte.cereg == 5);
  return csRegistered || psRegistered;
}

String signalQualityFromCsq(int csq) {
  if (csq < 0) {
    return "Unknown";
  }
  if (csq == 99) {
    return "Unknown / not ready";
  }
  if (csq >= 31) {
    return "Excellent";
  }
  if (csq >= 20) {
    return "Good";
  }
  if (csq >= 10) {
    return "Fair";
  }
  if (csq >= 1) {
    return "Weak";
  }
  return "No signal";
}

bool shouldUseCaptivePortal() {
  return FORCE_CAPTIVE_PORTAL || !isInternetReady();
}

void sendPortalLandingPage() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.sendHeader("X-Captive-Portal", "true");
  server.sendHeader("Captive-Portal", "true");
  server.sendHeader("Content-Type", "text/html; charset=utf-8");
  
  const String html = String(
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "<meta charset='utf-8'>\n"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
    "<script>\n"
    "if (window.location.hostname !== '192.168.4.1' || window.location.pathname !== '/') {\n"
    "  window.location.replace('http://192.168.4.1/');\n"
    "}\n"
    "</script>\n"
    "<title>RESQLinkDrone Setup</title>\n"
    "<style>\n"
    "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; margin: 0; padding: 20px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; display: flex; align-items: center; justify-content: center; }\n"
    "#container { background: white; border-radius: 12px; box-shadow: 0 20px 60px rgba(0,0,0,0.3); padding: 40px; max-width: 500px; width: 100%; }\n"
    "h1 { color: #333; margin: 0 0 20px 0; text-align: center; }\n"
    "p { color: #666; line-height: 1.6; }\n"
    ".status { background: #f0f4ff; padding: 15px; border-radius: 8px; margin: 20px 0; border-left: 4px solid #667eea; }\n"
    ".info { font-size: 14px; color: #999; text-align: center; margin-top: 20px; }\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<div id='container'>\n"
    "<h1>RESQLinkDrone Network</h1>\n"
    "<p>You are connected to the <strong>RESQLinkDrone</strong> access point.</p>\n"
    "<div class='status'>\n"
    "<strong>Network Status:</strong><br>\n"
    "Setting up internet gateway...\n"
    "</div>\n"
    "<p>This is a captive portal. Your connection will be upgraded to internet access once the LTE modem connects.</p>\n"
    "<p>You can configure the device at <a href='http://192.168.4.1/'>http://192.168.4.1/</a></p>\n"
    "<div class='info'>Portal ready. Keep this tab open while setup completes.</div>\n"
    "</body>\n"
    "</html>\n"
  );
  
  server.send(200, "text/html; charset=utf-8", html);
}

String jsonEscape(const String& input) {
  auto appendHex4 = [](String& target, uint8_t value) {
    static const char* hex = "0123456789ABCDEF";
    target += "\\u00";
    target += hex[(value >> 4) & 0x0F];
    target += hex[value & 0x0F];
  };

  String out;
  out.reserve(input.length() + 16);
  for (size_t i = 0; i < input.length(); ++i) {
    const uint8_t uc = static_cast<uint8_t>(input[i]);
    const char c = static_cast<char>(uc);
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (uc < 0x20 || uc == 0x7F) {
          appendHex4(out, uc);
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

String extractJsonStringField(const String& json, const char* key) {
  const String quotedKey = String("\"") + key + "\"";
  const int keyPos = json.indexOf(quotedKey);
  if (keyPos == -1) {
    return "";
  }

  const int colonPos = json.indexOf(':', keyPos + quotedKey.length());
  if (colonPos == -1) {
    return "";
  }

  int firstQuote = json.indexOf('"', colonPos + 1);
  if (firstQuote == -1) {
    return "";
  }

  String value;
  bool escaping = false;
  for (int i = firstQuote + 1; i < json.length(); ++i) {
    const char c = json[i];
    if (escaping) {
      switch (c) {
        case 'n':
          value += '\n';
          break;
        case 'r':
          value += '\r';
          break;
        case 't':
          value += '\t';
          break;
        default:
          value += c;
          break;
      }
      escaping = false;
      continue;
    }

    if (c == '\\') {
      escaping = true;
      continue;
    }

    if (c == '"') {
      return value;
    }

    value += c;
  }

  return "";
}

bool extractJsonBoolField(const String& json, const char* key, bool defaultValue) {
  const String quotedKey = String("\"") + key + "\"";
  const int keyPos = json.indexOf(quotedKey);
  if (keyPos == -1) {
    return defaultValue;
  }

  const int colonPos = json.indexOf(':', keyPos + quotedKey.length());
  if (colonPos == -1) {
    return defaultValue;
  }

  const String tail = json.substring(colonPos + 1);
  if (tail.indexOf("true") == 0) {
    return true;
  }
  if (tail.indexOf("false") == 0) {
    return false;
  }
  return defaultValue;
}

String buildSmsTemplate(const String& senderName, const String& senderContact, const String& message,
                        bool includeLocation, bool includeMapLink = true) {
  const GpsData gps = gpsGetData();

  String composed;
  composed.reserve(200);
  composed += "RESQLinkDrone\n";

  composed += "From:";
  composed += senderName.length() > 0 ? senderName : String("Unknown");
  if (senderContact.length() > 0) {
    composed += " ";
    composed += senderContact;
  }
  composed += "\n";

  if (includeLocation && gps.hasFix) {
    composed += "Loc:";
    composed += String(gps.latitude, 6);
    composed += ",";
    composed += String(gps.longitude, 6);
    composed += "\n";
    if (includeMapLink) {
      composed += "Map:https://maps.google.com/?q=";
      composed += String(gps.latitude, 6);
      composed += ",";
      composed += String(gps.longitude, 6);
      composed += "\n";
    }
  }

  composed += "Msg:";
  composed += message;
  return composed;
}

String normalizePhoneNumber(String input) {
  input.trim();

  String cleaned;
  cleaned.reserve(input.length());
  for (int i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if ((c >= '0' && c <= '9') || (c == '+' && cleaned.length() == 0)) {
      cleaned += c;
    }
  }

  if (cleaned.startsWith("+63") && cleaned.length() == 13) {
    return cleaned;
  }

  if (cleaned.startsWith("63") && cleaned.length() == 12) {
    return String("+") + cleaned;
  }

  if (cleaned.startsWith("09") && cleaned.length() == 11) {
    return String("+63") + cleaned.substring(1);
  }

  if (cleaned.startsWith("9") && cleaned.length() == 10) {
    return String("+63") + cleaned;
  }

  return cleaned;
}

String smsTrimToSingleSegment(const String& message, bool& truncated) {
  truncated = false;
  if (message.length() <= 160) {
    return message;
  }
  truncated = true;
  return message.substring(0, 157) + "...";
}

String extractQuotedToken(const String& input, int tokenIndex) {
  int current = -1;
  int start = -1;

  for (int i = 0; i < input.length(); ++i) {
    if (input[i] != '"') {
      continue;
    }

    if (start == -1) {
      start = i + 1;
      current++;
      continue;
    }

    if (current == tokenIndex) {
      return input.substring(start, i);
    }
    start = -1;
  }

  return "";
}

String parseCmglToJsonArray(const String& cmglRaw) {
  String normalized = cmglRaw;
  normalized.replace("\r", "");

  String json = "[";
  bool hasAny = false;

  String line;
  String currentIndex;
  String currentStatus;
  String currentNumber;
  String currentTimestamp;
  String currentBody;
  bool hasCurrent = false;

  auto flushCurrent = [&]() {
    if (!hasCurrent) {
      return;
    }

    currentBody.trim();
    if (hasAny) {
      json += ",";
    }
    hasAny = true;
    json += "{";
    json += "\"index\":\"" + jsonEscape(currentIndex) + "\"";
    json += ",\"status\":\"" + jsonEscape(currentStatus) + "\"";
    json += ",\"number\":\"" + jsonEscape(currentNumber) + "\"";
    json += ",\"timestamp\":\"" + jsonEscape(currentTimestamp) + "\"";
    json += ",\"body\":\"" + jsonEscape(currentBody) + "\"";
    json += "}";

    hasCurrent = false;
    currentIndex = "";
    currentStatus = "";
    currentNumber = "";
    currentTimestamp = "";
    currentBody = "";
  };

  for (int i = 0; i <= normalized.length(); ++i) {
    const bool isLineEnd = (i == normalized.length()) || (normalized[i] == '\n');
    if (!isLineEnd) {
      line += normalized[i];
      continue;
    }

    String trimmed = line;
    trimmed.trim();
    line = "";

    if (trimmed.length() == 0) {
      continue;
    }

    if (trimmed.startsWith("+CMGL:")) {
      flushCurrent();

      const int colon = trimmed.indexOf(':');
      const int firstComma = trimmed.indexOf(',', colon + 1);
      currentIndex = firstComma > (colon + 1) ? trimmed.substring(colon + 1, firstComma) : "";
      currentIndex.trim();
      currentStatus = extractQuotedToken(trimmed, 0);
      currentNumber = extractQuotedToken(trimmed, 1);
      currentTimestamp = extractQuotedToken(trimmed, 3);
      hasCurrent = true;
      continue;
    }

    if (trimmed == "OK") {
      continue;
    }

    if (hasCurrent) {
      if (currentBody.length() > 0) {
        currentBody += "\n";
      }
      currentBody += trimmed;
    }
  }

  flushCurrent();
  json += "]";
  return json;
}

String readFileContent(const char* path, size_t maxBytes = 4096) {
  if (!sdCardAvailable) {
    return "";
  }

  File file = SD.open(path, FILE_READ);
  if (!file) {
    return "";
  }

  String content;
  while (file.available() && content.length() < maxBytes) {
    content += static_cast<char>(file.read());
  }
  file.close();
  return content;
}

String contentTypeFor(const String& path) {
  if (path.endsWith(".html")) {
    return "text/html";
  }
  if (path.endsWith(".css")) {
    return "text/css";
  }
  if (path.endsWith(".js")) {
    return "application/javascript";
  }
  if (path.endsWith(".json")) {
    return "application/json";
  }
  if (path.endsWith(".csv")) {
    return "text/csv";
  }
  return "text/plain";
}

bool serveFromSd(const String& requestPath) {
  if (!sdCardAvailable) {
    return false;
  }

  String path = requestPath;
  if (path == "/") {
    path = "/www/index.html";
  } else if (!path.startsWith("/www/") && !path.startsWith("/api/") &&
             !path.startsWith("/gps") && !path.startsWith("/netinfo") &&
             !path.startsWith("/logs")) {
    path = "/www" + path;
  }

  if (!SD.exists(path.c_str())) {
    return false;
  }

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    return false;
  }

  server.streamFile(file, contentTypeFor(path));
  file.close();
  return true;
}

void handleGps() {
  (void)gpsRefreshNow();
  const GpsData gps = gpsGetData();

  String payload = "{";
  payload += "\"hasFix\":" + String(gps.hasFix ? "true" : "false");
  payload += ",\"fixType\":" + String(gps.fixType);
  payload += ",\"satellites\":" + String(gps.satellitesUsed);
  payload += ",\"latitude\":" + String(gps.latitude, 6);
  payload += ",\"longitude\":" + String(gps.longitude, 6);
  payload += ",\"altitudeMeters\":" + String(gps.altitudeMeters, 2);
  payload += ",\"speedKph\":" + String(gps.speedKph, 2);
  payload += ",\"utcDate\":\"" + jsonEscape(gps.utcDate) + "\"";
  payload += ",\"utcTime\":\"" + jsonEscape(gps.utcTime) + "\"";
  payload += ",\"raw\":\"" + jsonEscape(gps.rawInfo) + "\"";
  payload += "}";

  server.send(200, "application/json", payload);
}

String buildGpsDiagCommandResult(const char* cmd, uint32_t timeoutMs) {
  String response;
  const bool ok = lteSendCommand(cmd, response, timeoutMs);

  String payload = "{";
  payload += "\"cmd\":\"" + jsonEscape(String(cmd)) + "\"";
  payload += ",\"ok\":" + String(ok ? "true" : "false");
  payload += ",\"response\":\"" + jsonEscape(response) + "\"";
  payload += "}";
  return payload;
}

void handleGpsDiag() {
  bool recoveryAttempted = false;
  bool recoveryOk = false;

  if (!lteIsResponsive()) {
    recoveryAttempted = true;
    recoveryOk = lteRecoverNow();
  }

  (void)gpsRefreshNow();
  const GpsData gps = gpsGetData();
  const LteData lte = lteGetData();

  String payload = "{";
  payload += "\"ok\":true";
  payload += ",\"recoveryAttempted\":" + String(recoveryAttempted ? "true" : "false");
  payload += ",\"recoveryOk\":" + String(recoveryOk ? "true" : "false");
  payload += ",\"gps\":{";
  payload += "\"hasFix\":" + String(gps.hasFix ? "true" : "false");
  payload += ",\"fixType\":" + String(gps.fixType);
  payload += ",\"satellites\":" + String(gps.satellitesUsed);
  payload += ",\"latitude\":" + String(gps.latitude, 6);
  payload += ",\"longitude\":" + String(gps.longitude, 6);
  payload += ",\"raw\":\"" + jsonEscape(gps.rawInfo) + "\"";
  payload += "}";
  payload += ",\"modem\":{";
  payload += "\"responsive\":" + String(lte.responsive ? "true" : "false");
  payload += ",\"modemAlive\":" + String(isModemAlive() ? "true" : "false");
  payload += ",\"simReady\":" + String(lte.simReady ? "true" : "false");
  payload += ",\"networkRegistered\":" + String(isNetworkRegistered(lte) ? "true" : "false");
  payload += ",\"creg\":" + String(lte.creg);
  payload += ",\"cereg\":" + String(lte.cereg);
  payload += ",\"cgatt\":" + String(lte.cgatt);
  payload += ",\"rssi\":" + String(lte.rssi);
  payload += "}";
  payload += ",\"commands\":[";
  payload += buildGpsDiagCommandResult("AT+CGNSPWR?", 2000);
  payload += ",";
  payload += buildGpsDiagCommandResult("AT+CGNSINF", 2500);
  payload += ",";
  payload += buildGpsDiagCommandResult("AT+CGNSSINFO", 2500);
  payload += ",";
  payload += buildGpsDiagCommandResult("AT+CGPSINFO", 2500);
  payload += "]";
  payload += "}";

  server.send(200, "application/json", payload);
}

void handleNetInfo() {
  const LteData lte = lteGetData();
  const bool networkRegistered = isNetworkRegistered(lte);

  String payload = "{";
  payload += "\"responsive\":" + String(lte.responsive ? "true" : "false");
  payload += ",\"modemAlive\":" + String(isModemAlive() ? "true" : "false");
  payload += ",\"simReady\":" + String(lte.simReady ? "true" : "false");
  payload += ",\"networkRegistered\":" + String(networkRegistered ? "true" : "false");
  payload += ",\"dataConnected\":" + String(lte.dataConnected ? "true" : "false");
  payload += ",\"rssi\":" + String(lte.rssi);
  payload += ",\"ber\":" + String(lte.ber);
  payload += ",\"creg\":" + String(lte.creg);
  payload += ",\"cereg\":" + String(lte.cereg);
  payload += ",\"cgatt\":" + String(lte.cgatt);
  payload += ",\"apn\":\"" + jsonEscape(lte.apn) + "\"";
  payload += ",\"ipAddress\":\"" + jsonEscape(lte.ipAddress) + "\"";
  if (!lte.responsive) {
    payload += ",\"networkState\":\"Modem offline\"";
  } else if (!lte.simReady) {
    payload += ",\"networkState\":\"SIM not ready\"";
  } else if (!networkRegistered) {
    payload += ",\"networkState\":\"Waiting for network registration\"";
  } else if (!lte.dataConnected) {
    payload += ",\"networkState\":\"Registered, waiting for data session\"";
  } else {
    payload += ",\"networkState\":\"Internet up\"";
  }
  payload += ",\"downloadMbps\":-1";
  payload += ",\"uploadMbps\":-1";
  payload += ",\"note\":\"Speed test not yet implemented on modem side\"";
  payload += "}";

  server.send(200, "application/json", payload);
}

void handleLogs() {
  const String gpsLog = readFileContent("/gps_log.csv");
  const String sessionLog = readFileContent("/sessions.log");

  String payload = "{";
  payload += "\"gpsLog\":\"" + jsonEscape(gpsLog) + "\"";
  payload += ",\"sessionLog\":\"" + jsonEscape(sessionLog) + "\"";
  payload += "}";

  server.send(200, "application/json", payload);
}

void handlePortalSignIn() {
  lteStartInternetGateway();
  server.send(200, "application/json", "{\"ok\":true,\"signedIn\":true}");
}

void handlePortalStatus() {
  String payload = "{";
  payload += "\"signedIn\":false";
  payload += ",\"captiveActive\":" + String(shouldUseCaptivePortal() ? "true" : "false");
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleModemHealth() {
  const LteData lte = lteGetData();
  const unsigned long uptime = millis();
  const bool networkRegistered = isNetworkRegistered(lte);

  String payload = "{";
  payload += "\"ok\":true";
  payload += ",\"timestamp\":" + String(millis());
  payload += ",\"uptime\":" + String(uptime / 1000);
  payload += ",\"responsive\":" + String(lte.responsive ? "true" : "false");
  payload += ",\"modemAlive\":" + String(isModemAlive() ? "true" : "false");
  payload += ",\"simReady\":" + String(lte.simReady ? "true" : "false");
  payload += ",\"networkRegistered\":" + String(networkRegistered ? "true" : "false");
  payload += ",\"dataConnected\":" + String(lte.dataConnected ? "true" : "false");
  payload += ",\"rssi\":" + String(lte.rssi);
  payload += ",\"csq\":" + String(lte.rssi);
  payload += ",\"ber\":" + String(lte.ber);
  payload += ",\"creg\":" + String(lte.creg);
  payload += ",\"cereg\":" + String(lte.cereg);
  payload += ",\"cgatt\":" + String(lte.cgatt);
  payload += ",\"apn\":\"" + jsonEscape(lte.apn) + "\"";
  payload += ",\"ipAddress\":\"" + jsonEscape(lte.ipAddress) + "\"";
  payload += ",\"signalQuality\":\"" + signalQualityFromCsq(lte.rssi) + "\"";
  if (!lte.responsive) {
    payload += ",\"networkState\":\"Modem offline\"";
  } else if (!lte.simReady) {
    payload += ",\"networkState\":\"SIM not ready\"";
  } else if (!networkRegistered) {
    payload += ",\"networkState\":\"Waiting for network registration\"";
  } else if (!lte.dataConnected) {
    payload += ",\"networkState\":\"Registered, waiting for data session\"";
  } else {
    payload += ",\"networkState\":\"Internet up\"";
  }
  payload += "}";

  server.send(200, "application/json", payload);
}

void handleGpsInterferenceTest() {
  const String action = server.arg("action");
  
  if (action.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing action parameter (on/off)\"}");
    return;
  }

  String modemResponse;
  bool ok = false;
  
  if (action == "off") {
    // Disable GNSS/GPS on SIM7600G
    ok = lteSendCommand("AT+CGNSPWR=0", modemResponse, 2000);
    delay(1000);
  } else if (action == "on") {
    // Enable GNSS/GPS on SIM7600G
    ok = lteSendCommand("AT+CGNSPWR=1", modemResponse, 2000);
    delay(1000);
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid action. Use on or off\"}");
    return;
  }

  const LteData lte = lteGetData();
  
  String payload = "{";
  payload += "\"ok\":" + String(ok ? "true" : "false");
  payload += ",\"action\":\"" + jsonEscape(action) + "\"";
  payload += ",\"response\":\"" + jsonEscape(modemResponse) + "\"";
  payload += ",\"modemState\":{";
  payload += "\"rssi\":" + String(lte.rssi);
  payload += ",\"creg\":" + String(lte.creg);
  payload += ",\"cereg\":" + String(lte.cereg);
  payload += ",\"cgatt\":" + String(lte.cgatt);
  payload += ",\"simReady\":" + String(lte.simReady ? "true" : "false");
  payload += ",\"dataConnected\":" + String(lte.dataConnected ? "true" : "false");
  payload += "}";
  payload += "}";

  server.send(ok ? 200 : 500, "application/json", payload);
}

void handleSmsSend() {
  String to = server.arg("to");
  String message = server.arg("message");
  String senderName = server.arg("senderName");
  String senderContact = server.arg("senderContact");
  bool includeLocation = true;

  const String body = server.arg("plain");
  if (body.length() > 0) {
    const String jsonTo = extractJsonStringField(body, "to");
    const String jsonMessage = extractJsonStringField(body, "message");
    const String jsonSenderName = extractJsonStringField(body, "senderName");
    const String jsonSenderContact = extractJsonStringField(body, "senderContact");
    if (jsonTo.length() > 0) {
      to = jsonTo;
    }
    if (jsonMessage.length() > 0) {
      message = jsonMessage;
    }
    if (jsonSenderName.length() > 0) {
      senderName = jsonSenderName;
    }
    if (jsonSenderContact.length() > 0) {
      senderContact = jsonSenderContact;
    }
    includeLocation = extractJsonBoolField(body, "includeLocation", true);
  }

  to = normalizePhoneNumber(to);
  message.trim();
  senderName.trim();
  senderContact.trim();

  if (to.length() == 0 || message.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing to/message\"}");
    return;
  }

  if (to.length() < 10) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid destination number\"}");
    return;
  }

  const LteData net = lteGetData();
  const bool isCircuitRegistered = (net.creg == 1 || net.creg == 5);
  const bool isPacketRegistered = (net.cereg == 1 || net.cereg == 5);
  if (!net.simReady || (!isCircuitRegistered && !isPacketRegistered)) {
    String err = "{\"ok\":false,\"error\":\"Network not ready for SMS";
    err += " (SIM=" + String(net.simReady ? "ready" : "not-ready");
    err += ", CREG=" + String(net.creg);
    err += ", CEREG=" + String(net.cereg) + ")\"}";
    server.send(503, "application/json", err);
    return;
  }

  String composedMessage = buildSmsTemplate(senderName, senderContact, message, includeLocation, false);
  bool truncated = false;
  composedMessage = smsTrimToSingleSegment(composedMessage, truncated);

  String modemResponse;
  bool ok = lteSendSms(to, composedMessage, modemResponse);
  bool usedLocationFallback = false;
  String fallbackReason = "";

  // Fallback: If send failed with location block, retry without location entirely.
  if (!ok && includeLocation) {
    bool noLocTruncated = false;
    String noLocMessage = buildSmsTemplate(senderName, senderContact, message, false, false);
    noLocMessage = smsTrimToSingleSegment(noLocMessage, noLocTruncated);

    String noLocResponse;
    const bool noLocOk = lteSendSms(to, noLocMessage, noLocResponse);
    if (noLocOk) {
      ok = true;
      usedLocationFallback = true;
      fallbackReason = "Retried without location";
      truncated = noLocTruncated;
      composedMessage = noLocMessage;
      modemResponse = noLocResponse;
    } else {
      modemResponse += "\n[retry-without-location]\n";
      modemResponse += noLocResponse;
    }
  }

  String payload = "{";
  payload += "\"ok\":" + String(ok ? "true" : "false");
  payload += ",\"to\":\"" + jsonEscape(to) + "\"";
  payload += ",\"template\":\"" + jsonEscape(composedMessage) + "\"";
  payload += ",\"length\":" + String(composedMessage.length());
  payload += ",\"truncated\":" + String(truncated ? "true" : "false");
  payload += ",\"usedLocationFallback\":" + String(usedLocationFallback ? "true" : "false");
  payload += ",\"fallbackReason\":\"" + jsonEscape(fallbackReason) + "\"";
  payload += ",\"response\":\"" + jsonEscape(modemResponse) + "\"";
  payload += "}";

  if (ok) {
    server.send(200, "application/json", payload);
  } else {
    server.send(500, "application/json", payload);
  }
}

void handleSmsInbox() {
  String response;
  String diagnostics;

  if (!lteSendCommand("AT+CMGF=1", response, 2500)) {
    String payload = "{";
    payload += "\"ok\":false";
    payload += ",\"error\":\"Failed to set SMS text mode\"";
    payload += ",\"response\":\"" + jsonEscape(response) + "\"";
    payload += "}";
    server.send(200, "application/json", payload);
    return;
  }

  String storageResp;
  if (!lteSendCommand("AT+CPMS=\"SM\",\"SM\",\"SM\"", storageResp, 2500)) {
    diagnostics += "[CPMS SM failed]\n";
    diagnostics += storageResp;
    diagnostics += "\n";
    if (!lteSendCommand("AT+CPMS=\"ME\",\"ME\",\"ME\"", storageResp, 2500)) {
      diagnostics += "[CPMS ME failed]\n";
      diagnostics += storageResp;
      diagnostics += "\n";
    }
  }

  bool inboxOk = lteSendCommand("AT+CMGL=\"ALL\"", response, 8000);
  if (!inboxOk) {
    diagnostics += "[CMGL ALL failed]\n";
    diagnostics += response;
    diagnostics += "\n";

    String alt;
    if (lteSendCommand("AT+CMGL=\"REC UNREAD\"", alt, 8000)) {
      inboxOk = true;
      response = alt;
    } else {
      diagnostics += "[CMGL REC UNREAD failed]\n";
      diagnostics += alt;
      diagnostics += "\n";

      if (lteSendCommand("AT+CMGL=\"REC READ\"", alt, 8000)) {
        inboxOk = true;
        response = alt;
      } else {
        diagnostics += "[CMGL REC READ failed]\n";
        diagnostics += alt;
        diagnostics += "\n";
      }
    }
  }

  if (!inboxOk) {
    String payload = "{";
    payload += "\"ok\":false";
    payload += ",\"error\":\"Failed to query inbox\"";
    payload += ",\"response\":\"" + jsonEscape(diagnostics) + "\"";
    payload += "}";
    server.send(200, "application/json", payload);
    return;
  }

  String payload = "{";
  payload += "\"ok\":true";
  payload += ",\"messagesRaw\":\"" + jsonEscape(response) + "\"";
  payload += ",\"messages\":" + parseCmglToJsonArray(response);
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleRoot() {
  const bool captiveMode = shouldUseCaptivePortal();
  if (captiveMode) {
    // Keep captive headers on the root response so OS captive detection still
    // recognizes this network while the SD-hosted app is being served.
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.sendHeader("X-Captive-Portal", "true");
    server.sendHeader("Captive-Portal", "true");
  }

  if (serveFromSd("/")) {
    return;
  }

  if (captiveMode) {
    sendPortalLandingPage();
    return;
  }

  sendPortalLandingPage();
}

void handleNotFound() {
  if (serveFromSd(server.uri())) {
    return;
  }

  if (shouldUseCaptivePortal()) {
    // Captive mode should only handle requests that are not part of the SD app.
    sendPortalLandingPage();
    return;
  }

  // File not found - provide helpful diagnostic info
  String errorHtml = String(
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "<meta charset='utf-8'>\n"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
    "<title>File Not Found</title>\n"
    "<style>\n"
    "body { font-family: monospace; margin: 20px; padding: 20px; background: #f5f5f5; }\n"
    ".error { background: #ffe6e6; padding: 15px; border: 1px solid #cc0000; border-radius: 4px; }\n"
    ".info { background: #e6f2ff; padding: 15px; margin-top: 15px; border: 1px solid #0066cc; border-radius: 4px; }\n"
    "h1 { color: #cc0000; margin: 0 0 10px 0; }\n"
    "p { margin: 5px 0; }\n"
    "code { background: white; padding: 2px 5px; }\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<div class='error'>\n"
    "<h1>404 - File Not Found</h1>\n"
    "<p><strong>Requested:</strong> <code>" + jsonEscape(server.uri()) + "</code></p>\n"
    "</div>\n"
    "<div class='info'>\n"
    "<p><strong>Diagnostics:</strong></p>\n"
    "<p>• SD Card Available: " + String(sdCardAvailable ? "✓ YES" : "✗ NO") + "</p>\n"
  );

  if (!sdCardAvailable) {
    errorHtml += "<p><strong style='color: #cc0000;'>⚠ SD card not initialized!</strong></p>\n";
    errorHtml += "<p>The web portal files have not been uploaded to the SD card yet.</p>\n";
    errorHtml += "<p>To fix this:</p>\n";
    errorHtml += "<ol>\n";
    errorHtml += "<li>Connect via serial console (115200 baud)</li>\n";
    errorHtml += "<li>Wait for boot to complete (~30 seconds)</li>\n";
    errorHtml += "<li>Run: <code>powershell -ExecutionPolicy Bypass -File tools/upload-www-over-serial.ps1</code></li>\n";
    errorHtml += "</ol>\n";
  } else {
    errorHtml += "<p>• Requested path exists on SD: ✗ NO</p>\n";
    errorHtml += "<p>The file does not exist on the SD card.</p>\n";
    errorHtml += "<p>Ensure web portal files are uploaded to <code>/www/</code></p>\n";
  }

  errorHtml += "</div>\n"
    "</body>\n"
    "</html>\n";

  server.send(404, "text/html; charset=utf-8", errorHtml);
}

void handleCaptiveProbe() {
  // If SD-hosted app is available, prefer serving it so client captive
  // assistants show the real portal UI instead of the setup landing page.
  if (sdCardAvailable) {
    // Try serving the exact requested probe path first (some clients request
    // well-known probe URLs under root). If that fails, fall back to the
    // SD-hosted root index.
    if (serveFromSd(server.uri())) {
      Serial.print("[WEB] Captive probe served SD path: ");
      Serial.println(server.uri());
      return;
    }

    if (serveFromSd("/")) {
      Serial.println("[WEB] Captive probe served SD index for client.");
      return;
    }
  }

  if (shouldUseCaptivePortal()) {
    // Return landing content while captive mode is active so OS probe checks
    // detect a login page and launch their captive portal assistant.
    Serial.print("[WEB] Captive probe served portal: ");
    Serial.print(server.uri());
    Serial.print(" from ");
    Serial.println(server.client().remoteIP());
    sendPortalLandingPage();
    return;
  }

  const String uri = server.uri();
  if (uri == "/ncsi.txt") {
    server.send(200, "text/plain", "Microsoft NCSI");
    return;
  }
  if (uri == "/connecttest.txt") {
    server.send(200, "text/plain", "Microsoft Connect Test");
    return;
  }
  if (uri == "/hotspot-detect.html" || uri == "/library/test/success.html" ||
      uri == "/success.txt" || uri == "/captive.html") {
    server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    return;
  }

  server.send(204, "text/plain", "");
}
}  // namespace

void webInit() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  bool apStarted = false;
  bool apConfigOk = false;
  constexpr uint8_t AP_CHANNEL = 1;
  constexpr uint8_t AP_MAX_CONN = 4;

  for (int attempt = 1; attempt <= 3 && !apStarted; ++attempt) {
    apConfigOk = WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_NETMASK);
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONN);

    if (!apConfigOk || !apStarted) {
      Serial.print("[WEB] AP start attempt ");
      Serial.print(attempt);
      Serial.println(" failed. Retrying...");
      WiFi.mode(WIFI_OFF);
      delay(200);
      WiFi.mode(WIFI_AP);
      WiFi.setSleep(false);
      delay(200);
    }
  }

  Serial.print("[WEB] AP SSID: ");
  Serial.println(AP_SSID);
  Serial.print("[WEB] AP config result: ");
  Serial.println(apConfigOk ? "OK" : "FAILED");
  Serial.print("[WEB] AP start result: ");
  Serial.println(apStarted ? "OK" : "FAILED");
  Serial.print("[WEB] AP channel: ");
  Serial.println(WiFi.channel());
  Serial.print("[WEB] AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());
  Serial.print("[WEB] AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (!apStarted) {
    Serial.println("[WEB] ERROR: Access point did not start. Wi-Fi will not be visible.");
  }

  server.on("/gps", HTTP_GET, handleGps);
  server.on("/gps/diag", HTTP_GET, handleGpsDiag);
  server.on("/netinfo", HTTP_GET, handleNetInfo);
  server.on("/modem/health", HTTP_GET, handleModemHealth);
  server.on("/modem/gps-test", HTTP_POST, handleGpsInterferenceTest);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/portal/signin", HTTP_POST, handlePortalSignIn);
  server.on("/portal/status", HTTP_GET, handlePortalStatus);
  server.on("/sms/send", HTTP_POST, handleSmsSend);
  server.on("/sms/inbox", HTTP_GET, handleSmsInbox);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/portal", HTTP_GET, handleRoot);
  server.on("/portal/", HTTP_GET, handleRoot);

  // Captive portal probe endpoints for major clients.
  // Android
  server.on("/generate_204", HTTP_ANY, handleCaptiveProbe);
  server.on("/gen_204", HTTP_ANY, handleCaptiveProbe);
  server.on("/gstatic/generate_204", HTTP_ANY, handleCaptiveProbe);
  
  // Apple / iOS
  server.on("/hotspot-detect.html", HTTP_ANY, handleCaptiveProbe);
  server.on("/library/test/success.html", HTTP_ANY, handleCaptiveProbe);
  server.on("/success.txt", HTTP_ANY, handleCaptiveProbe);
  server.on("/captive.html", HTTP_ANY, handleCaptiveProbe);
  
  // Windows / NCSI
  server.on("/ncsi.txt", HTTP_ANY, handleCaptiveProbe);
  server.on("/connecttest.txt", HTTP_ANY, handleCaptiveProbe);
  
  // Generic / Browser
  server.on("/redirect", HTTP_ANY, handleCaptiveProbe);
  server.on("/fwlink", HTTP_ANY, handleCaptiveProbe);
  server.on("/favicon.ico", HTTP_ANY, handleCaptiveProbe);
  server.on("/apple-touch-icon.png", HTTP_ANY, handleCaptiveProbe);
  server.on("/wpad.dat", HTTP_ANY, handleCaptiveProbe);
  
  // Google / Ubuntu
  server.on("/chrome-variations/seed", HTTP_ANY, handleCaptiveProbe);
  server.on("/canonical.html", HTTP_ANY, handleCaptiveProbe);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("[WEB] HTTP server started.");

  // Start in captive mode by default until LTE internet is available.
  dnsServer.start(DNS_PORT, "*", AP_IP);
  dnsCaptiveActive = true;
  Serial.println("[WEB] DNS captive portal started.");
}

void webLoop() {
  const bool shouldBeCaptive = shouldUseCaptivePortal();
  if (shouldBeCaptive && !dnsCaptiveActive) {
    dnsServer.start(DNS_PORT, "*", AP_IP);
    dnsCaptiveActive = true;
    Serial.println("[WEB] DNS captive portal enabled (no internet).");
  } else if (!shouldBeCaptive && dnsCaptiveActive) {
    dnsServer.stop();
    dnsCaptiveActive = false;
    Serial.println("[WEB] DNS captive portal disabled (internet ready).");
  }

  if (dnsCaptiveActive) {
    dnsServer.processNextRequest();  // Handle DNS queries only in captive mode
  }
  server.handleClient();
}
