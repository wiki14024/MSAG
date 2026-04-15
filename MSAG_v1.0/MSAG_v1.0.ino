/* ====================================================================
 * PROJEKT: MSAG v1.16 PRO - Sterownik Autokonsumpcji PV
 * ZMIANY: Miganie Blue, logowanie IP, obsługa http://msag.local/
 * ==================================================================== */

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h> 
#include <LittleFS.h>
#include <SPI.h>
#include <ATM90E32.h>
#include <Adafruit_NeoPixel.h>
#include <ESPmDNS.h>      
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#include <ElegantOTA.h>
#include <Preferences.h>
#include <time.h> 
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>

// --- KONFIGURACJA I PINY ---
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbzAcy2naRZOLIXcWL7EJqcBJneJ2xo41oJqKFPly6qPAq5zUFtkmoKOWeUMx3GuR9Fz/exec";
const String FW_VERSION = "v1.16 PRO";
const char* HOSTNAME = "msag"; 

#define PIN_DIP1 23
#define PIN_DIP2 22
#define PIN_DIP3 21
#define PIN_DIP4 20
#define PIN_LED1 19
#define PIN_LED2 18
#define PIN_PWM_OUT 14
#define PIN_SSR_SENSE 0
#define PIN_SPI_MOSI 2
#define PIN_SPI_MISO 3
#define PIN_SPI_SCK  4
#define PIN_SPI_CS   5
#define PIN_RGB 8
#define PIN_RST_OUT 6
#define PIN_RST_IN 7

const int pwmFreq = 5000;
const int pwmResolution = 10;
#define NUMPIXELS 1

// --- STAN WIFI (State Machine) ---
enum WiFiState { WIFI_INIT, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_AP_MODE };
WiFiState wifi_state = WIFI_INIT;
unsigned long wifi_state_timer = 0;
unsigned long last_reconnect_attempt = 0;
bool ap_is_running = false;

// --- OBIEKTY I ZMIENNE GLOBALNE ---
ATM90E32 licznik_atm{};
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Adafruit_NeoPixel rgb_led(NUMPIXELS, PIN_RGB, NEO_GRB + NEO_KHZ800);
Preferences nvm;
WiFiManager wm;

float p_max_heater = 0.0;
unsigned long last_control_loop = 0;
unsigned long last_ws_update = 0;
unsigned long last_wifi_check = 0;
unsigned long last_google_try = 0;
unsigned long last_led_update = 0;
volatile int aktualne_pwm = 0;
volatile bool tryb_auto = true;
bool tryb_awaryjny = false;
float p_total = 0.0;
float ema_p_total = 0.0;
const float EMA_ALPHA = 0.4;
float ssr_v = 0.0;

double total_export_kwh = 0.0, total_import_kwh = 0.0;
double today_export_kwh = 0.0, today_import_kwh = 0.0;
uint32_t start_timestamp = 0, last_timestamp = 0;
bool trigger_google_sync = false, force_zero_sync = false;
float live_history[60];

// --- FUNKCJE POMOCNICZE ---
String formatTimestamp(uint32_t timestamp) {
  if (timestamp < 100000) return "Brak danych";
  time_t t = timestamp; struct tm *tm_info = localtime(&t);
  char buf[20]; strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", tm_info); 
  return String(buf);
}

String getUptime() {
  unsigned long secs = millis() / 1000;
  unsigned long h = secs / 3600; int m = (secs % 3600) / 60; int s = secs % 60;
  char buf[16]; snprintf(buf, sizeof(buf), "%lu:%02d:%02d", h, m, s);
  return String(buf);
}

void obliczMocGrzalki() {
  p_max_heater = 0.0;
  if (digitalRead(PIN_DIP1) == LOW) p_max_heater += 4000.0;
  if (digitalRead(PIN_DIP2) == LOW) p_max_heater += 2000.0;
  if (digitalRead(PIN_DIP3) == LOW) p_max_heater += 1000.0;
  if (digitalRead(PIN_DIP4) == LOW) p_max_heater += 500.0;
  if (p_max_heater == 0) p_max_heater = 1000.0;
}

// --- SYSTEM SYGNALIZACJI LED (NON-BLOCKING) ---
void aktualizujStanIKolory() {
    if (wifi_state == WIFI_CONNECTING) {
        // Miganie turkusowe (Teal) - Próba nawiązania połączenia
        if ((millis() % 500) < 250) rgb_led.setPixelColor(0, rgb_led.Color(0, 128, 128)); 
        else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
    }
    else if (wifi_state == WIFI_AP_MODE) {
        // Miganie Niebieskie (Blue) - Tryb Konfiguracji AP
        if ((millis() % 500) < 250) rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 255));
        else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
    }
    else { // WIFI_CONNECTED - Logika energii
        if (tryb_awaryjny) {
            if (millis() % 500 < 250) rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
            else rgb_led.setPixelColor(0, rgb_led.Color(255, 0, 0)); // Miganie Czerwone
        } else {
            if (ema_p_total > 100) rgb_led.setPixelColor(0, rgb_led.Color(255, 0, 0)); // Pobór (Red)
            else if (ema_p_total < -100) rgb_led.setPixelColor(0, rgb_led.Color(0, 255, 0)); // Eksport (Green)
            else if (aktualne_pwm > 102 && tryb_auto) rgb_led.setPixelColor(0, rgb_led.Color(255, 100, 0)); // Grzałka (Orange)
            else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 255)); // Balans (Blue)
        }
    }
    rgb_led.show();
}

// --- MASZYNA STANÓW WIFI ---
void obslugaWiFi() {
    wm.process(); 
    switch (wifi_state) {
        case WIFI_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("\n[WIFI] Połączono z routerem pomyślnie!");
                Serial.print("[WIFI] Przydzielony adres IP: ");
                Serial.println(WiFi.localIP());
                Serial.println("[WIFI] Dostęp przez przeglądarkę pod adresem: http://msag.local/");
                
                if (ap_is_running) { wm.stopConfigPortal(); ap_is_running = false; }
                wifi_state = WIFI_CONNECTED;
            } else if (millis() - wifi_state_timer > 15000) {
                Serial.println("[WIFI] Nie udało się połączyć. Start trybu AP...");
                if (!ap_is_running) { wm.startConfigPortal("MSAG-Konfiguracja"); ap_is_running = true; }
                wifi_state = WIFI_AP_MODE;
                last_reconnect_attempt = millis();
            }
            break;
        case WIFI_CONNECTED:
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("\n[WIFI] Utrata połączenia! Próba reconnectu...");
                wifi_state = WIFI_CONNECTING;
                wifi_state_timer = millis();
            }
            break;
        case WIFI_AP_MODE:
            if (wm.getWiFiSSID() != "" && (millis() - last_reconnect_attempt > 30000)) {
                last_reconnect_attempt = millis();
                Serial.println("[WIFI] Sprawdzam po cichu czy router powrócił...");
                WiFi.begin(); 
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("\n[WIFI] Router powrócił! Połączenie odzyskane.");
                Serial.print("[WIFI] Przydzielony adres IP: ");
                Serial.println(WiFi.localIP());
                Serial.println("[WIFI] Dostęp przez przeglądarkę pod adresem: http://msag.local/");
                
                wm.stopConfigPortal(); ap_is_running = false;
                wifi_state = WIFI_CONNECTED;
            }
            break;
    }
}

// --- KOMUNIKACJA GOOGLE I WS ---
void syncWithGoogle() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client; client.setInsecure(); HTTPClient http;
  String url = GOOGLE_SCRIPT_URL + "?export=" + (force_zero_sync ? "0" : String(today_export_kwh, 3)) + 
               "&import=" + (force_zero_sync ? "0" : String(today_import_kwh, 3));
  http.begin(client, url); http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (http.GET() == 200) { trigger_google_sync = false; force_zero_sync = false; }
  else { nvm.putDouble("exp_kwh", total_export_kwh); nvm.putDouble("imp_kwh", total_import_kwh); }
  http.end();
}

void wyslijDaneWebsocket(bool sendLive) {
  JsonDocument doc; doc["grid_p"] = (int)p_total;
  doc["heater_pwm"] = (aktualne_pwm * 100) / 1023;
  doc["heater_active"] = (aktualne_pwm > 0); doc["ssr_v"] = ssr_v;
  doc["mode"] = tryb_auto ? "auto" : "manual"; doc["uptime"] = getUptime();
  doc["fw_version"] = FW_VERSION; doc["cpu_temp"] = temperatureRead();
  doc["export_kwh"] = total_export_kwh; doc["import_kwh"] = total_import_kwh;
  struct tm ti; if (getLocalTime(&ti)) { char ts[10]; strftime(ts, sizeof(ts), "%H:%M:%S", &ti); doc["clock"] = String(ts); }
  if (sendLive) { JsonArray live = doc.createNestedArray("live_data"); for(int i=0; i<60; i++) live.add((int)live_history[i]); }
  String js; serializeJson(doc, js); ws.textAll(js);
}

void onWsEvent(AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType t, void *a, uint8_t *d, size_t l) {
  if (t == WS_EVT_DATA) { JsonDocument doc; if (deserializeJson(doc, d, l)) return;
    if (doc.containsKey("mode")) tryb_auto = (doc["mode"] == "auto");
    if (doc.containsKey("pwm") && !tryb_auto) aktualne_pwm = ((int)doc["pwm"] * 1023) / 100;
    if (doc.containsKey("cmd")) { String cmd = doc["cmd"]; 
      if (cmd == "reboot") ESP.restart();
      else if (cmd == "reset_wifi") { wm.resetSettings(); ESP.restart(); }
    }
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n\nSTART SYSTEMU MSAG v1.16 PRO");
  rgb_led.begin(); rgb_led.setBrightness(50);
  
  pinMode(PIN_RST_OUT, OUTPUT); digitalWrite(PIN_RST_OUT, LOW);
  pinMode(PIN_RST_IN, INPUT_PULLUP);
  pinMode(PIN_SSR_SENSE, INPUT);
  pinMode(PIN_DIP1, INPUT_PULLUP); pinMode(PIN_DIP2, INPUT_PULLUP);
  pinMode(PIN_DIP3, INPUT_PULLUP); pinMode(PIN_DIP4, INPUT_PULLUP);
  
  ledcAttach(PIN_PWM_OUT, pwmFreq, pwmResolution); ledcWrite(PIN_PWM_OUT, 0);
  obliczMocGrzalki();
  
  nvm.begin("msag", false);
  total_export_kwh = nvm.getDouble("exp_kwh", 0.0);
  total_import_kwh = nvm.getDouble("imp_kwh", 0.0);
  
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS);
  licznik_atm.begin(PIN_SPI_CS, 50, 0, 8000, 8000, 8000, 8000);
  LittleFS.begin(true);

  WiFi.mode(WIFI_STA); wm.setHostname(HOSTNAME); wm.setConfigPortalBlocking(false);
  if (wm.getWiFiSSID() != "") { 
      Serial.println("[WIFI] Próba połączenia z zapamiętaną siecią...");
      WiFi.begin(); 
      wifi_state = WIFI_CONNECTING; 
      wifi_state_timer = millis(); 
  }
  else { 
      Serial.println("[WIFI] Brak sieci. Start AP MSAG-Konfiguracja.");
      wm.startConfigPortal("MSAG-Konfiguracja"); 
      ap_is_running = true; 
      wifi_state = WIFI_AP_MODE; 
  }

// --- REJESTRACJA mDNS ---
  if (MDNS.begin(HOSTNAME)) {
      Serial.println("[mDNS] Usługa mDNS uruchomiona poprawnie.");
      MDNS.addService("http", "tcp", 80);
  }

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");
  ws.onEvent(onWsEvent); 
  server.addHandler(&ws); 
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html"); 
  
  ElegantOTA.begin(&server); 
  server.begin(); 
}

// --- LOOP ---
void loop() {
  obslugaWiFi(); 
  ws.cleanupClients(); ElegantOTA.loop();

  if (millis() - last_led_update >= 50) { last_led_update = millis(); aktualizujStanIKolory(); }

  // Reset WiFi przez zworkę
  if (digitalRead(PIN_RST_IN) == LOW) {
      static unsigned long rst_start = 0; if (rst_start == 0) rst_start = millis();
      if (millis() - rst_start > 5000) { wm.resetSettings(); ESP.restart(); }
  }

  ssr_v = (analogRead(PIN_SSR_SENSE) * 3.3 / 4095.0) * 3.0;
  if (millis() - last_control_loop >= 333) { last_control_loop = millis();
    p_total = licznik_atm.GetTotalActivePower();
    ema_p_total = (EMA_ALPHA * p_total) + ((1.0 - EMA_ALPHA) * ema_p_total);
    if (tryb_auto && !tryb_awaryjny) {
      if (ema_p_total > -50) aktualne_pwm -= 40; else if (ema_p_total < -100) aktualne_pwm += 20;
      aktualne_pwm = constrain(aktualne_pwm, 0, 1023);
    }
    ledcWrite(PIN_PWM_OUT, tryb_awaryjny ? 0 : aktualne_pwm);
  }

  if (millis() - last_ws_update >= 1000) { last_ws_update = millis();
    obliczMocGrzalki();
    double re = licznik_atm.GetExportEnergy(); double ri = licznik_atm.GetImportEnergy();
    if (re > 0) { total_export_kwh += re; today_export_kwh += re; }
    if (ri > 0) { total_import_kwh += ri; today_import_kwh += ri; }
    if (aktualne_pwm > 512 && ssr_v < 1.0) tryb_awaryjny = true;
    
    struct tm ti; if (getLocalTime(&ti) && ti.tm_year > 123) {
        if (ti.tm_hour == 0 && ti.tm_min == 0) { today_export_kwh = 0; today_import_kwh = 0; }
        if (ti.tm_min == 0 || ti.tm_min == 30) trigger_google_sync = true;
    }

    static int lt = 0; if (++lt >= 5) { lt = 0;
      for(int i=0; i<59; i++) live_history[i] = live_history[i+1];
      live_history[59] = ema_p_total; wyslijDaneWebsocket(true);
    } else wyslijDaneWebsocket(false);
  }

  if (trigger_google_sync && millis() - last_google_try >= 15000) {
    last_google_try = millis(); syncWithGoogle();
  }
}