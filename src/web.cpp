#include "web.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <SD.h>
#include <ArduinoJson.h>

#include "lte.h"

namespace {
constexpr const char* kApSsid = "ELQWifi";
constexpr uint8_t kDnsPort = 53;
const IPAddress kApIp(192, 168, 4, 1);
const IPAddress kApGateway(192, 168, 4, 1);
const IPAddress kApNetmask(255, 255, 255, 0);

WebServer server(80);
DNSServer dnsServer;
bool dnsCaptiveActive = false;
bool portalSignedIn = false;
bool spiffsReady = false;
unsigned long activeSessionUntilMs = 0;
String activeVoucherCode;

const char* kGpsLogPath = "/gps_log.csv";
const char* kSessionLogPath = "/sessions.log";
const char* kSmsLogPath = "/sms_messages.json";
const char* kVouchersJsonPath = "/data/vouchers.json";
const char* kSessionsJsonPath = "/data/sessions.json";

bool isInternetReady() {
  const LteData lte = lteGetData();
  return lte.dataConnected && lte.ipAddress.length() > 0 && lte.ipAddress != "0.0.0.0";
}

bool shouldUseCaptivePortal() {
  return true;
}

bool hasActiveVoucherSession() {
  return activeSessionUntilMs != 0 && millis() < activeSessionUntilMs;
}

String htmlEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value[i];
    switch (ch) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '"': escaped += "&quot;"; break;
      case '\'': escaped += "&#39;"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value[i];
    switch (ch) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

String resolveRequestField(const JsonDocument& req, const char* key, const String& fallback = "") {
  if (req.containsKey(key)) {
    const JsonVariantConst value = req[key];
    if (!value.isNull()) {
      return String(value.as<const char*>());
    }
  }
  if (server.hasArg(key)) {
    return server.arg(key);
  }
  return fallback;
}

String getContentType(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".json")) return "application/json";
  return "text/plain";
}

bool tryServeFromSpiffs(const String& requestPath) {
  if (!spiffsReady) {
    return false;
  }

  String path = requestPath;
  if (path.length() == 0 || path == "/") {
    path = "/index.html";
  }

  if (SPIFFS.exists(path)) {
    File file = SPIFFS.open(path, FILE_READ);
    if (!file) {
      return false;
    }
    server.streamFile(file, getContentType(path));
    file.close();
    return true;
  }

  if (SPIFFS.exists(path + ".html")) {
    File file = SPIFFS.open(path + ".html", FILE_READ);
    if (!file) {
      return false;
    }
    server.streamFile(file, "text/html");
    file.close();
    return true;
  }

  return false;
}

bool tryServeFromSd(const String& requestPath) {
  String path = requestPath;
  if (path.length() == 0 || path == "/") {
    path = "/index.html";
  }

  String candidates[4];
  size_t candidateCount = 0;
  candidates[candidateCount++] = path;
  if (path.startsWith("/portal/")) {
    candidates[candidateCount++] = path.substring(strlen("/portal"));
  }

  const String roots[2] = {"/www", "/sd/www"};
  for (size_t i = 0; i < 2; ++i) {
    for (size_t c = 0; c < candidateCount; ++c) {
      const String effectivePath = candidates[c];
      String sdPath = roots[i] + effectivePath;
      if (SD.exists(sdPath)) {
        File file = SD.open(sdPath, FILE_READ);
        if (!file) {
          continue;
        }
        server.streamFile(file, getContentType(effectivePath));
        file.close();
        return true;
      }

      sdPath = roots[i] + effectivePath + ".html";
      if (SD.exists(sdPath)) {
        File file = SD.open(sdPath, FILE_READ);
        if (!file) {
          continue;
        }
        server.streamFile(file, "text/html");
        file.close();
        return true;
      }
    }
  }

  return false;
}

String readSdFileSnippet(const char* path, size_t maxLen) {
  File file = SD.open(path, FILE_READ);
  if (!file) {
    return "";
  }

  String content;
  content.reserve(maxLen + 1);
  while (file.available() && content.length() < maxLen) {
    content += static_cast<char>(file.read());
  }
  file.close();
  return content;
}

void sendJson(const String& payload) {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", payload);
}

File openSdFileAnyRoot(const char* relativePath, const char* mode) {
  const String roots[2] = {"", "/sd"};
  for (size_t i = 0; i < 2; ++i) {
    String full = roots[i] + String(relativePath);
    File f = SD.open(full, mode);
    if (f) {
      return f;
    }
  }
  return File();
}

bool readJsonFromSd(const char* relativePath, JsonDocument& doc) {
  File file = openSdFileAnyRoot(relativePath, FILE_READ);
  if (!file) {
    return false;
  }
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  return !err;
}

bool writeJsonToSd(const char* relativePath, const JsonDocument& doc) {
  const String roots[2] = {"", "/sd"};
  for (size_t i = 0; i < 2; ++i) {
    const String full = roots[i] + String(relativePath);
    if (SD.exists(full)) {
      SD.remove(full);
    }
    File file = SD.open(full, FILE_WRITE);
    if (!file) {
      continue;
    }
    serializeJson(doc, file);
    file.close();
    return true;
  }
  return false;
}

String isoTimestamp() {
  const unsigned long totalSeconds = millis() / 1000UL;
  const unsigned long seconds = totalSeconds % 60UL;
  const unsigned long minutes = (totalSeconds / 60UL) % 60UL;
  const unsigned long hours = (totalSeconds / 3600UL) % 24UL;
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "2026-04-24 %02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(buffer);
}

bool appendSmsRecord(const String& number, const String& body, const String& status) {
  JsonDocument doc;
  if (!readJsonFromSd(kSmsLogPath, doc)) {
    doc.to<JsonArray>();
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    arr = doc.to<JsonArray>();
  }

  JsonObject entry = arr.add<JsonObject>();
  entry["index"] = arr.size();
  entry["status"] = status;
  entry["number"] = number;
  entry["timestamp"] = isoTimestamp();
  entry["body"] = body;

  return writeJsonToSd(kSmsLogPath, doc);
}

bool findVoucher(const String& code, int& limitMinutes, int& limitMb) {
  JsonDocument doc;
  if (!readJsonFromSd(kVouchersJsonPath, doc)) {
    return false;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    return false;
  }
  for (JsonObject obj : arr) {
    const String voucherCode = String(obj["code"] | "");
    if (voucherCode.equalsIgnoreCase(code)) {
      limitMinutes = obj["limit_minutes"] | 60;
      limitMb = obj["limit_mb"] | 500;
      return true;
    }
  }
  return false;
}

bool appendSession(const String& code, int limitMinutes, int limitMb) {
  JsonDocument doc;
  if (!readJsonFromSd(kSessionsJsonPath, doc)) {
    doc.to<JsonArray>();
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    arr = doc.to<JsonArray>();
  }

  JsonObject entry = arr.add<JsonObject>();
  entry["code"] = code;
  entry["limit_minutes"] = limitMinutes;
  entry["limit_mb"] = limitMb;
  entry["start_ms"] = millis();
  entry["active"] = true;

  return writeJsonToSd(kSessionsJsonPath, doc);
}

void sendPortalRedirect() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.sendHeader("Location", "http://192.168.4.1/");
  server.send(302, "text/plain", "Redirecting to captive portal...");
}

void sendCaptiveLandingPage() {
  const LteData status = lteGetData();
  String html;
  html.reserve(3800);
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>ELQWifi Portal</title>";
  html += "<style>body{margin:0;font-family:Verdana,sans-serif;background:linear-gradient(135deg,#d8efe8 0%,#f9f7ee 100%);color:#173231;}";
  html += ".wrap{max-width:900px;margin:0 auto;padding:20px;}";
  html += ".hero{background:#124734;color:#e8fff6;padding:20px;border-radius:14px;box-shadow:0 10px 25px rgba(18,71,52,.2);}";
  html += ".grid{display:grid;grid-template-columns:1fr;gap:14px;margin-top:14px;}@media(min-width:840px){.grid{grid-template-columns:1fr 1fr;}}";
  html += ".card{background:#fff;border-radius:12px;padding:16px;box-shadow:0 8px 18px rgba(0,0,0,.08);}dt{font-weight:700;margin-top:8px;}dd{margin:4px 0 8px;}";
  html += "input,button{font-size:14px;padding:10px;border-radius:8px;border:1px solid #b8cec4;}button{background:#1f6e50;color:#fff;border:none;cursor:pointer;}";
  html += "small{opacity:.85;}</style></head><body><div class=\"wrap\">";
  html += "<div class=\"hero\"><h1 style=\"margin:0 0 8px\">ELQWifi Captive Portal</h1>";
  html += "<div>SSID: <strong>" + String(kApSsid) + "</strong> | Gateway: <strong>" + WiFi.softAPIP().toString() + "</strong></div>";
  html += "<small>Open this page to monitor LTE status and update APN.</small></div>";
  html += "<div class=\"grid\">";
  html += "<div class=\"card\"><h2 style=\"margin-top:0\">LTE Status</h2><dl>";
  html += "<dt>Modem responsive</dt><dd>" + String(status.responsive ? "yes" : "no") + "</dd>";
  html += "<dt>SIM ready</dt><dd>" + String(status.simReady ? "yes" : "no") + "</dd>";
  html += "<dt>Data connected</dt><dd>" + String(status.dataConnected ? "yes" : "no") + "</dd>";
  html += "<dt>APN</dt><dd>" + htmlEscape(status.apn) + "</dd>";
  html += "<dt>IP</dt><dd>" + htmlEscape(status.ipAddress) + "</dd>";
  html += "<dt>RSSI</dt><dd>" + String(status.rssi) + "</dd>";
  html += "<dt>CGATT</dt><dd>" + String(status.cgatt) + "</dd>";
  html += "</dl></div>";
  html += "<div class=\"card\"><h2 style=\"margin-top:0\">APN Setup</h2>";
  html += "<form method=\"POST\" action=\"/apn\">";
  html += "<label for=\"apn\">APN</label><br><input id=\"apn\" name=\"apn\" value=\"" + htmlEscape(status.apn) + "\" style=\"width:100%;box-sizing:border-box\"><br><br>";
  html += "<button type=\"submit\">Save APN</button></form>";
  html += "<p><small>Common PH APNs: tm, internet, internet.globe.com.ph, mnet</small></p>";
  html += "<p><a href=\"/status\">JSON status</a></p></div></div></div></body></html>";
  server.send(200, "text/html", html);
}

void handleRoot() {
  sendCaptiveLandingPage();
}

void handleStatus() {
  const LteData status = lteGetData();
  String payload;
  payload.reserve(256);
  payload += "{";
  payload += "\"responsive\":" + String(status.responsive ? "true" : "false");
  payload += ",\"simReady\":" + String(status.simReady ? "true" : "false");
  payload += ",\"dataConnected\":" + String(status.dataConnected ? "true" : "false");
  payload += ",\"pdpActive\":" + String(status.pdpActive ? "true" : "false");
  payload += ",\"rssi\":" + String(status.rssi);
  payload += ",\"cgatt\":" + String(status.cgatt);
  payload += ",\"apn\":\"" + escapeJson(status.apn) + "\"";
  payload += ",\"ipAddress\":\"" + escapeJson(status.ipAddress) + "\"";
  payload += "}";
  sendJson(payload);
}

void handlePortalStatus() {
  const LteData status = lteGetData();
  const bool online = isInternetReady() && hasActiveVoucherSession();
  String reason = "Waiting for modem";
  if (!status.responsive) {
    reason = "No AT response";
  } else if (!status.simReady) {
    reason = "SIM not ready";
  } else if (!status.pdpActive) {
    reason = "PDP inactive (check APN)";
  } else if (status.cgatt != 1) {
    reason = "Packet not attached";
  } else if (!hasActiveVoucherSession()) {
    reason = "Voucher required";
  } else {
    reason = "PPP not established yet";
  }

  String payload;
  payload.reserve(220);
  payload += "{\"ok\":true,\"signedIn\":";
  payload += (portalSignedIn || online) ? "true" : "false";
  payload += ",\"online\":";
  payload += online ? "true" : "false";
  payload += ",\"reason\":\"" + escapeJson(reason) + "\"";
  payload += ",\"voucher\":\"" + escapeJson(activeVoucherCode) + "\"";
  payload += ",\"pdpActive\":" + String(status.pdpActive ? "true" : "false");
  payload += ",\"cgatt\":" + String(status.cgatt);
  payload += ",\"ipAddress\":\"" + escapeJson(status.ipAddress) + "\"";
  payload += "}";
  sendJson(payload);
}

void handlePortalSignin() {
  String voucherCode;
  if (server.hasArg("plain")) {
    JsonDocument req;
    if (!deserializeJson(req, server.arg("plain"))) {
      voucherCode = String(req["code"] | "");
    }
  }
  if (voucherCode.length() == 0 && server.hasArg("code")) {
    voucherCode = server.arg("code");
  }
  voucherCode.trim();

  if (voucherCode.length() == 0) {
    sendJson("{\"ok\":false,\"reason\":\"Voucher code required\"}");
    return;
  }

  int limitMinutes = 0;
  int limitMb = 0;
  if (!findVoucher(voucherCode, limitMinutes, limitMb)) {
    sendJson("{\"ok\":false,\"reason\":\"Invalid voucher\"}");
    return;
  }

  if (!appendSession(voucherCode, limitMinutes, limitMb)) {
    sendJson("{\"ok\":false,\"reason\":\"Failed to persist session\"}");
    return;
  }

  portalSignedIn = true;
  activeVoucherCode = voucherCode;
  activeSessionUntilMs = millis() + static_cast<unsigned long>(limitMinutes) * 60000UL;
  lteStartInternetGateway();
  handlePortalStatus();
}

void handleSmsInbox() {
  JsonDocument doc;
  if (!readJsonFromSd(kSmsLogPath, doc)) {
    sendJson("{\"ok\":true,\"messages\":[]}");
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    sendJson("{\"ok\":true,\"messages\":[]}");
    return;
  }

  String payload;
  payload.reserve(512);
  payload += "{\"ok\":true,\"messages\":";
  serializeJson(arr, payload);
  payload += "}";
  sendJson(payload);
}

void handleSmsSend() {
  JsonDocument req;
  if (server.hasArg("plain")) {
    if (deserializeJson(req, server.arg("plain"))) {
      sendJson("{\"ok\":false,\"error\":\"Invalid JSON body\"}");
      return;
    }
  }

  const String to = resolveRequestField(req, "to");
  const String message = resolveRequestField(req, "message");
  const String senderName = resolveRequestField(req, "senderName");
  const String senderContact = resolveRequestField(req, "senderContact");

  if (to.length() == 0 || message.length() == 0) {
    sendJson("{\"ok\":false,\"error\":\"Phone number and message are required\"}");
    return;
  }

  String outbound = message;
  bool truncated = false;
  if (outbound.length() > 160) {
    outbound = outbound.substring(0, 160);
    truncated = true;
  }

  String modemResponse;
  bool modemSent = false;
  String sendNote = "Queued locally for delivery";
  if (!lteDataModeActive()) {
    modemSent = lteSendSms(to, outbound, modemResponse);
    if (modemSent) {
      sendNote = "Submitted to modem successfully";
    } else if (modemResponse.length() > 0) {
      sendNote = "Queued locally; modem send failed";
    }
  } else {
    sendNote = "Queued locally while data mode is active";
  }

  const String status = modemSent ? "SENT" : "QUEUED";
  (void)appendSmsRecord(to, outbound, status);

  String payload;
  payload.reserve(384);
  payload += "{\"ok\":true";
  payload += ",\"to\":\"" + escapeJson(to) + "\"";
  payload += ",\"response\":\"" + escapeJson(modemResponse.length() > 0 ? modemResponse : sendNote) + "\"";
  payload += ",\"truncated\":";
  payload += truncated ? "true" : "false";
  payload += ",\"usedLocationFallback\":false";
  payload += ",\"sent\":";
  payload += modemSent ? "true" : "false";
  payload += ",\"senderName\":\"" + escapeJson(senderName) + "\"";
  payload += ",\"senderContact\":\"" + escapeJson(senderContact) + "\"";
  payload += ",\"queued\":";
  payload += modemSent ? "false" : "true";
  payload += "}";
  sendJson(payload);
}

void handleModemHealth() {
  const LteData status = lteGetData();
  String payload;
  payload.reserve(280);
  payload += "{\"ok\":true";
  payload += ",\"signalQuality\":\"";
  payload += (status.rssi >= -85) ? "Excellent" : (status.rssi >= -95) ? "Good" : (status.rssi >= -105) ? "Fair" : "Weak";
  payload += "\"";
  payload += ",\"rssi\":" + String(status.rssi);
  payload += ",\"creg\":" + String(status.creg);
  payload += ",\"cereg\":" + String(status.cereg);
  payload += ",\"cgatt\":" + String(status.cgatt);
  payload += ",\"simReady\":" + String(status.simReady ? "true" : "false");
  payload += ",\"dataConnected\":" + String(status.dataConnected ? "true" : "false");
  payload += ",\"pdpActive\":" + String(status.pdpActive ? "true" : "false");
  payload += ",\"ipAddress\":\"" + escapeJson(status.ipAddress) + "\"";
  payload += "}";
  sendJson(payload);
}

void handleGps() {
  const LteGpsData gps = lteGetGpsData();
  String payload;
  payload.reserve(360);
  payload += "{\"gnssActive\":";
  payload += gps.gnssActive ? "true" : "false";
  payload += ",\"hasFix\":";
  payload += gps.hasFix ? "true" : "false";
  payload += ",\"fixType\":" + String(gps.fixType);
  payload += ",\"satellites\":" + String(gps.satellites);
  payload += ",\"latitude\":" + String(gps.latitude, 6);
  payload += ",\"longitude\":" + String(gps.longitude, 6);
  payload += ",\"altitudeMeters\":" + String(gps.altitudeMeters, 2);
  payload += ",\"speedKph\":" + String(gps.speedKph, 2);
  payload += ",\"raw\":\"" + escapeJson(gps.raw) + "\"}";
  sendJson(payload);
}

void handleNetInfo() {
  const LteData status = lteGetData();
  String payload;
  payload.reserve(260);
  payload += "{\"dataConnected\":" + String(status.dataConnected ? "true" : "false");
  payload += ",\"rssi\":" + String(status.rssi);
  payload += ",\"simReady\":" + String(status.simReady ? "true" : "false");
  payload += ",\"pdpActive\":" + String(status.pdpActive ? "true" : "false");
  payload += ",\"creg\":" + String(status.creg);
  payload += ",\"cereg\":" + String(status.cereg);
  payload += ",\"cgatt\":" + String(status.cgatt);
  payload += ",\"downloadMbps\":0";
  payload += ",\"uploadMbps\":0";
  payload += ",\"apn\":\"" + escapeJson(status.apn) + "\"";
  payload += ",\"ipAddress\":\"" + escapeJson(status.ipAddress) + "\"";
  payload += "}";
  sendJson(payload);
}

void handleLogs() {
  const String gpsLog = readSdFileSnippet(kGpsLogPath, 3000);
  const String sessionLog = readSdFileSnippet(kSessionLogPath, 3000);

  String payload;
  payload.reserve(6400);
  payload += "{\"gpsLog\":\"" + escapeJson(gpsLog) + "\"";
  payload += ",\"sessionLog\":\"" + escapeJson(sessionLog) + "\"";
  payload += "}";
  sendJson(payload);
}

void handleGpsTest() {
  const LteData status = lteGetData();
  String payload;
  payload.reserve(220);
  payload += "{\"ok\":true,\"response\":\"GPS RF test endpoint is informational in this build\"";
  payload += ",\"modemState\":{";
  payload += "\"rssi\":" + String(status.rssi);
  payload += ",\"creg\":" + String(status.creg);
  payload += ",\"cereg\":" + String(status.cereg);
  payload += ",\"cgatt\":" + String(status.cgatt);
  payload += "}}";
  sendJson(payload);
}

void handleCaptiveProbe() {
  if (shouldUseCaptivePortal()) {
    sendCaptiveLandingPage();
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
  if (uri == "/hotspot-detect.html") {
    server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    return;
  }
  if (uri == "/success.txt") {
    server.send(200, "text/plain", "success");
    return;
  }

  server.send(204, "text/plain", "");
}

void handleApnPost() {
  if (!server.hasArg("apn")) {
    server.send(400, "text/plain", "Missing apn field");
    return;
  }
  const String apn = server.arg("apn");
  lteSetApn(apn);
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "APN updated");
}

void handleNotFound() {
  if (tryServeFromSd(server.uri())) {
    return;
  }

  if (tryServeFromSpiffs(server.uri())) {
    return;
  }

  sendPortalRedirect();
  return;
}
}  // namespace

void webInit() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(kApIp, kApGateway, kApNetmask);
  WiFi.softAP(kApSsid);
  delay(200);

  Serial.print("[WEB] AP SSID: ");
  Serial.println(kApSsid);
  Serial.print("[WEB] AP IP: ");
  Serial.println(WiFi.softAPIP());

  spiffsReady = SPIFFS.begin(true);
  if (spiffsReady) {
    Serial.println("[WEB] SPIFFS mounted (/data assets available after Upload File System Image).");
  } else {
    Serial.println("[WEB] SPIFFS mount failed, using fallback inline portal page.");
  }

  server.on("/", HTTP_ANY, handleRoot);
  server.on("/portal", HTTP_ANY, handleRoot);
  server.on("/portal/", HTTP_ANY, handleRoot);
  server.on("/status", HTTP_ANY, handleStatus);
  server.on("/portal/status", HTTP_GET, handlePortalStatus);
  server.on("/portal/signin", HTTP_POST, handlePortalSignin);
  server.on("/sms/inbox", HTTP_GET, handleSmsInbox);
  server.on("/sms/send", HTTP_POST, handleSmsSend);
  server.on("/modem/health", HTTP_GET, handleModemHealth);
  server.on("/modem/gps-test", HTTP_POST, handleGpsTest);
  server.on("/gps", HTTP_GET, handleGps);
  server.on("/netinfo", HTTP_GET, handleNetInfo);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/apn", HTTP_POST, handleApnPost);
  server.on("/generate_204", HTTP_ANY, handleCaptiveProbe);
  server.on("/gen_204", HTTP_ANY, handleCaptiveProbe);
  server.on("/hotspot-detect.html", HTTP_ANY, handleCaptiveProbe);
  server.on("/success.txt", HTTP_ANY, handleCaptiveProbe);
  server.on("/connecttest.txt", HTTP_ANY, handleCaptiveProbe);
  server.on("/redirect", HTTP_ANY, handleCaptiveProbe);
  server.on("/canonical.html", HTTP_ANY, handleCaptiveProbe);
  server.on("/fwlink", HTTP_ANY, handleCaptiveProbe);
  server.on("/ncsi.txt", HTTP_ANY, handleCaptiveProbe);
  server.onNotFound(handleNotFound);
  server.begin();

  dnsServer.start(kDnsPort, "*", kApIp);
  dnsCaptiveActive = true;
  Serial.println("[WEB] HTTP server started.");
  Serial.println("[WEB] Captive DNS started.");
}

void webLoop() {
  const bool captive = shouldUseCaptivePortal();
  if (captive && !dnsCaptiveActive) {
    dnsServer.start(kDnsPort, "*", kApIp);
    dnsCaptiveActive = true;
    Serial.println("[WEB] Captive DNS enabled.");
  }

  if (dnsCaptiveActive) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
}
