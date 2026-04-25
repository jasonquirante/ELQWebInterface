#include "portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>

namespace {
DNSServer dnsServer;
bool dnsActive = false;
}

void portalInit() {
  const IPAddress apIp = WiFi.softAPIP();
  if (apIp == IPAddress(0, 0, 0, 0)) {
    Serial.println("[Portal] AP not ready yet; captive DNS not started.");
    return;
  }

  dnsServer.stop();
  if (dnsServer.start(53, "*", apIp)) {
    dnsActive = true;
    Serial.print("[Portal] Captive DNS started at ");
    Serial.println(apIp);
  } else {
    dnsActive = false;
    Serial.println("[Portal] Failed to start captive DNS.");
  }
}

void portalLoop() {
  if (dnsActive) {
    dnsServer.processNextRequest();
  }
}
