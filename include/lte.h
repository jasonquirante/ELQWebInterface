#pragma once

#include <Arduino.h>

struct LteData {
	bool responsive;
	bool simReady;
	bool dataConnected;
	int rssi;
	int ber;
	int creg;
	int cereg;
	int cgatt;
	String apn;
	String ipAddress;
	String rawCpin;
};

void lteInit();
void lteInitBridge();
void lteLoop();
bool lteIsResponsive();
bool lteRecoverNow();
bool lteDataModeActive();
void lteStartInternetGateway();
void lteSetApn(const String& apn);
String lteGetApn();
size_t lteRawWrite(const uint8_t* data, size_t len);
int lteRawAvailable();
int lteRawRead();
bool lteSendCommand(const char* cmd, String& response, uint32_t timeoutMs = 1500);
bool lteSendSms(const String& phoneNumber, const String& message, String& modemResponse);
bool lteDialNumber(const String& phoneNumber, String& modemResponse);
bool lteHangupCall(String& modemResponse);
bool lteReadSmsInbox(String& modemResponse);
LteData lteGetData();
