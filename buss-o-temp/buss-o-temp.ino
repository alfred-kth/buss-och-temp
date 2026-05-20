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
// Se till att din secrets.h ser ut såhär inuti:
// const char *ssid = "DITT_WIFI_NAMN";
// const char *password = "DITT_LÖSENORD";

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

  // Starta WiFi (använder variablerna från secrets.h)
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
    delay(10 * 60000UL); // Vila 10 minuter, ingen API-trafik
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

  // Uppdatera temperaturen var 30:e minut (sparar data/trafik)
  if (millis() - lastTempUpdate > 30 * 60000UL || lastTempUpdate == 0) {
    currentTemp = getTemperature();
    lastTempUpdate = millis();
  }

  // Hämta avgångar. Returnerar -1 om ingen buss hittades.
  int minutesLeft = getDepartureData();

  // --- UPPDATERA ALLTID VARJE MINUT PÅ DAGTID ---
  if (minutesLeft != -1) {
    Serial.print("Bussen går om ");
    Serial.print(minutesLeft);
    Serial.println(" min.");
  } else {
    // Visas bara i seriell monitor, ej skärmen.
    Serial.println("Kunde inte hämta bussdata (försöker igen om 1 min).");
  }

  Serial.println("Uppdaterar om 1 minut...");
  delay(60000UL); // Vänta exakt 1 minut
}

// Returnerar antal minuter kvar. Returnerar -1 om ingen buss hittades.
int getDepartureData() {
  HTTPClient http;

  // SL Transport API
  String url = "https://transport.integration.sl.se/v1/sites/";
  url += slSiteId;
  url += "/departures";

  http.useHTTP10(true);
  http.begin(url);
  int httpCode = http.GET();

  int minutesLeftToReturn = -1;

  if (httpCode == 200) {
    // JSON-filter för att spara RAM
    StaticJsonDocument<256> filter;
    filter["departures"][0]["line"]["designation"] = true;
    filter["departures"][0]["destination"] = true;
    filter["departures"][0]["scheduled"] = true;
    filter["departures"][0]["expected"] = true;

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
      http.end();
      return -1;
    }

    JsonArray departures = doc["departures"];
    bool found = false;

    struct tm timeinfo;
    getLocalTime(&timeinfo);
    int currentTotalMins = (timeinfo.tm_hour * 60) + timeinfo.tm_min;

    for (JsonObject dep : departures) {
      String line = dep["line"]["designation"].as<String>();
      String dest = dep["destination"].as<String>();

      // Buss 50 mot Odenplan/Universitetet
      if (line == "50" && dest.indexOf("Lappk") == -1) {
        String useTime = dep["expected"] ? dep["expected"].as<String>()
                                         : dep["scheduled"].as<String>();

        if (useTime.length() < 16)
          continue;

        int busH = useTime.substring(11, 13).toInt();
        int busM = useTime.substring(14, 16).toInt();
        int busTotalMins = (busH * 60) + busM;

        if (busTotalMins < currentTotalMins &&
            (currentTotalMins - busTotalMins) > 1000) {
          busTotalMins += 24 * 60;
        }

        int diff = busTotalMins - currentTotalMins;

        if (diff >= 0) {
          minutesLeftToReturn = diff;
          found = true;
          break; // Hittat nästa buss, sluta leta
        }
      }
    }

    // --- RITA PÅ SKÄRMEN ---
    display.clearDisplay();
    display.setTextColor(WHITE);

    // 1. Rita temperatur i övre högra hörnet först (Större + °)
    if (currentTemp != -99.0) {
      int tempInt = (int)round(currentTemp);
      display.setTextSize(3);

      if (tempInt > -10 && tempInt < 10) {
        display.setCursor(85, 0);
      } else {
        display.setCursor(67, 0);
      }

      display.print(tempInt);
      display.print("\xF7");
    }

    // 2. Rita busstid (Alltid störst)
    display.setTextSize(7);
    if (found) {
      if (minutesLeftToReturn < 10) {
        display.setCursor(20, 8);
      } else {
        display.setCursor(0, 8);
      }
      display.print(minutesLeftToReturn);
    } else {
      display.setTextSize(2);
      display.setCursor(0, 30);
      display.print("Ingen buss");
    }

    display.display();

  } else {
    Serial.print("HTTP Error SL: ");
    Serial.println(httpCode);
  }

  http.end();
  return minutesLeftToReturn;
}

// Temperatur-API
float getTemperature() {
  HTTPClient http;
  // Stockholm (Du kan ändra lat/long till Lappkärrsberget om du
  // vill: 59.367, 18.064)
  String url =
      "https://api.open-meteo.com/v1/"
      "forecast?latitude=59.3294&longitude=18.0687&current=temperature_2m";

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
    DeserializationError error = deserializeJson(
        doc, http.getStream(), DeserializationOption::Filter(filter));
    if (!error) {
      temp = doc["current"]["temperature_2m"].as<float>();
    }
  } else {
    Serial.print("HTTP Error Väder: ");
    Serial.println(httpCode);
  }
  http.end();
  return temp;
}