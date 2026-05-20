#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <time.h>

// --- DINA UPPGIFTER ---
#include "secrets.h"

// SL Transport API (gratis, ingen nyckel behövs)
// Stora Lappkärrsberget = SiteId 1182
const int slSiteId = 1182;

// --- INSTÄLLNINGAR ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

const char *ntpServer = "pool.ntp.org";
const char *tzInfo = "CET-1CEST,M3.5.0,M10.5.0/3";

// --- VARIABLER ---
unsigned long lastTempUpdate = 0;
float currentTemp = -99.0;

void setup() {
  Serial.begin(115200);

  // Starta skärm
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED fail"));
    for (;;)
      ;
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 10);
  display.println("Ansluter WiFi...");
  display.display();

  // Starta WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("Synkar tid...");
  display.display();

  // Konfigurera tid
  configTzTime(tzInfo, ntpServer);

  struct tm timeinfo;
  while (!getLocalTime(&timeinfo, 5000)) {
    Serial.println("Väntar på tidssynk...");
  }
}

void loop() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Kunde inte läsa lokal tid");
    delay(5000);
    return;
  }

  // --- NATTLÄGE (22:00 till 05:59) ---
  if (timeinfo.tm_hour >= 22 || timeinfo.tm_hour < 6) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    Serial.println("Nattläge aktivt. Skärm avstängd.");
    delay(10 * 60000UL);
    return;
  }

  // Se till att skärmen är på under dagen
  display.ssd1306_command(SSD1306_DISPLAYON);

  // Kolla WiFi-anslutning
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(5000);
    return;
  }

  // Uppdatera temperaturen var 30:e minut
  if (millis() - lastTempUpdate > 30 * 60000UL || lastTempUpdate == 0) {
    currentTemp = getTemperature();
    lastTempUpdate = millis();
  }

  // Hämta avgångar och få tillbaka hur många minuter det är kvar
  int minutesLeft = getDepartureData();

  // --- SMART DELAY-LOGIK ---
  unsigned long delayMs;

  if (minutesLeft == -1) {
    // Ingen buss hittades -> Vila 30 minuter
    delayMs = 30 * 60000UL;
    Serial.println("Ingen buss. Väntar 30 minuter.");
  } else if (minutesLeft <= 5) {
    // Bussen går inom 5 minuter -> Live-uppdatering var 30:e sekund
    delayMs = 30 * 1000UL;
    Serial.println("Live-läge! Uppdaterar om 30 sekunder.");
  } else {
    // Sov tills det är exakt 5 minuter kvar. Max vila 30 min.
    int sleepMins = minutesLeft - 5;
    if (sleepMins > 30)
      sleepMins = 30;

    delayMs = sleepMins * 60000UL;
    Serial.print("Bussen går om ");
    Serial.print(minutesLeft);
    Serial.print(" min. Sover i ");
    Serial.print(sleepMins);
    Serial.println(" min.");
  }

  delay(delayMs);
}

// Returnerar antal minuter kvar. Returnerar -1 om ingen buss hittades.
int getDepartureData() {
  HTTPClient http;

  // SL Transport API - inga nycklar behövs!
  String url = "https://transport.integration.sl.se/v1/sites/";
  url += slSiteId;
  url += "/departures";

  http.useHTTP10(true);
  http.begin(url); // Vanlig HTTP, inget SSL behövs
  int httpCode = http.GET();

  int minutesLeftToReturn = -1;

  if (httpCode == 200) {
    // JSON-filter för att spara RAM
    StaticJsonDocument<256> filter;
    filter["departures"][0]["line"]["designation"] = true;
    filter["departures"][0]["destination"] = true;
    filter["departures"][0]["scheduled"] = true;
    filter["departures"][0]["expected"] = true;
    filter["departures"][0]["display"] = true;

    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(
        doc, http.getStream(), DeserializationOption::Filter(filter));

    if (error) {
      Serial.print("JSON parse fail: ");
      Serial.println(error.c_str());
      http.end();
      return -1;
    }

    if (!doc["departures"].is<JsonArray>()) {
      displayNoBus();
      http.end();
      return -1;
    }

    JsonArray departures = doc["departures"];
    bool found = false;
    int busDelayMins = 0;

    struct tm timeinfo;
    getLocalTime(&timeinfo);
    int currentTotalMins = (timeinfo.tm_hour * 60) + timeinfo.tm_min;

    for (JsonObject dep : departures) {
      String line = dep["line"]["designation"].as<String>();
      String dest = dep["destination"].as<String>();

      // Buss 50 mot Odenplan eller Universitetet (INTE mot Lappkärrsberget)
      if (line == "50" &&
          dest.indexOf("Lappk") == -1) {

        // "expected" = realtid, "scheduled" = tidtabell
        // "display" = text som "5 min" eller "Nu"
        String expectedStr = dep["expected"] ? dep["expected"].as<String>() : "";
        String scheduledStr = dep["scheduled"] ? dep["scheduled"].as<String>() : "";

        // Använd realtid om den finns, annars tidtabell
        // Format: "2026-05-17T08:35:00"
        String useTime = expectedStr.length() > 0 ? expectedStr : scheduledStr;
        
        if (useTime.length() < 16) continue; // Ogiltig tid

        // Plocka ut HH:MM från "YYYY-MM-DDTHH:MM:SS"
        int busH = useTime.substring(11, 13).toInt();
        int busM = useTime.substring(14, 16).toInt();
        int busTotalMins = (busH * 60) + busM;

        // Beräkna försening
        if (expectedStr.length() > 0 && scheduledStr.length() > 0) {
          int schedH = scheduledStr.substring(11, 13).toInt();
          int schedM = scheduledStr.substring(14, 16).toInt();
          int schedTotalMins = (schedH * 60) + schedM;
          busDelayMins = busTotalMins - schedTotalMins;
          if (busDelayMins < 0) busDelayMins = 0;
        }

        // Hantera midnatt
        if (busTotalMins < currentTotalMins &&
            (currentTotalMins - busTotalMins) > 1000) {
          busTotalMins += 24 * 60;
        }

        int diff = busTotalMins - currentTotalMins;

        if (diff >= 0) {
          minutesLeftToReturn = diff;
          found = true;
          break;
        }
      }
    }

    // --- RITA PÅ SKÄRMEN ---
    display.clearDisplay();
    display.setTextColor(WHITE);

    if (found) {
      // Stora siffror för minuter kvar
      display.setTextSize(7);

      if (minutesLeftToReturn < 10) {
        display.setCursor(20, 0);
      } else {
        display.setCursor(0, 0);
      }

      display.print(minutesLeftToReturn);

      // Visa försening om bussen är sen och nära
      if (busDelayMins > 0 && minutesLeftToReturn <= 10) {
        display.setTextSize(1);
        display.setCursor(86, 20);
        display.print("+");
        display.print(busDelayMins);
        display.print(" sen");
      }
    } else {
      display.setTextSize(2);
      display.setCursor(0, 24);
      display.print("Ingen buss");
    }

    // Rita temperatur i övre högra hörnet (visas alltid)
    if (currentTemp != -99.0) {
      display.setTextSize(2);
      display.setCursor(86, 0);
      display.print((int)round(currentTemp));
      display.print("C");
    }

    display.display();

  } else {
    Serial.print("HTTP Error: ");
    Serial.println(httpCode);

    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setCursor(0, 10);
    display.setTextSize(1);
    display.println("API-fel:");
    display.setCursor(0, 25);
    display.setTextSize(2);
    display.print("HTTP ");
    display.println(httpCode);
    display.display();
  }

  http.end();
  return minutesLeftToReturn;
}

// Hjälpfunktion för att rita "Ingen buss"
void displayNoBus() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(3);
  display.setCursor(18, 20);
  display.print("Ingen");
  display.display();
}

float getTemperature() {
  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=59.3294&longitude=18.0687&current=temperature_2m";
  
  WiFiClientSecure clientSecure;
  clientSecure.setInsecure();
  
  http.useHTTP10(true);
  http.begin(clientSecure, url);
  int httpCode = http.GET();
  float temp = -99.0;
  
  if (httpCode == 200) {
    StaticJsonDocument<128> filter;
    filter["current"]["temperature_2m"] = true;
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    if (!error) {
      temp = doc["current"]["temperature_2m"].as<float>();
    }
  }
  http.end();
  return temp;
}