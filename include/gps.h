#pragma once

#include <Arduino.h>

struct GpsData {
	bool hasFix;
	int fixType;
	int satellitesUsed;
	double latitude;
	double longitude;
	double altitudeMeters;
	double speedKph;
	String utcDate;
	String utcTime;
	String rawInfo;
};

void gpsInit();
void gpsLoop();
bool gpsRefreshNow();
GpsData gpsGetData();
