/* ====================================================================
 * PROJEKT: MSAG v1.16 - Sterownik Autokonsumpcji PV (Wersja PRO)
 * ==================================================================== 
 * ZMIANY: 
 * - Martwa strefa PWM (utrzymywanie 50-100W eksportu)
 * - Harmonogram 24h z resetem o 00:00 i wymuszonymi zerami o 00:05
 * - Wykres LIVE w RAM (Asynchroniczna wysyłka)
 * - Miganie morskim kolorem przy starcie (FreeRTOS Task)
 * - Płynne pulsowanie niebieskiego dla portalu AP
 * - Cichy powrót do sieci w tle
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

// --- DANE GOOGLE I DNS ---
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbzAcy2naRZOLIXcWL7EJqcBJneJ2xo41oJqKFPly6qPAq5zUFtkmoKOWeUMx3GuR9Fz/exec";
const String FW_VERSION = "v1.16";
const char* HOSTNAME = "msag";

// --- PINY ---
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

// --- OBIEKTY ---
ATM90E32 licznik_atm{}; 
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Adafruit_NeoPixel rgb_led(NUMPIXELS, PIN_RGB, NEO_GRB + NEO_KHZ800);
Preferences nvm; 
WiFiManager wm;

// --- ZMIENNE GLOBALNE ---
float p_max_heater = 0.0;
unsigned long last_control_loop = 0;
unsigned long last_ws_update = 0;
unsigned long last_wifi_check = 0; 
unsigned long last_google_try = 0;
unsigned long hw_reset_start = 0;
unsigned long last_led_update = 0;

volatile int aktualne_pwm = 0;      
volatile bool tryb_auto = true;
bool tryb_awaryjny = false;

float p_total = 0.0;       
float ema_p_total = 0.0;   
const float EMA_ALPHA = 0.4;
float ssr_v = 0.0;
String aktualny_kolor = "blue"; 

// --- ZMIENNE ZEGARA I WIFI ---
double total_export_kwh = 0.0;
double total_import_kwh = 0.0;
double today_export_kwh = 0.0; 
double today_import_kwh = 0.0; 
uint32_t start_timestamp = 0;      
uint32_t last_timestamp = 0;
bool trigger_google_sync = false; 
bool force_zero_sync = false;

// --- BUFOR LIVE (Wykres 5 minut = 60 punktów) ---
float live_history[60];

// =========================================================
// FUNKCJE POMOCNICZE
// =========================================================
String formatTimestamp(uint32_t timestamp) {
  if (timestamp < 100000) return "Brak danych";
  time_t t = timestamp;
  struct tm *tm_info = localtime(&t);
  char buf[20]; 
  strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", tm_info); 
  return String(buf);
}

String getUptime() {
  unsigned long secs = millis() / 1000;
  unsigned long h = secs / 3600;
  int m = (secs % 3600) / 60; 
  int s = secs % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu:%02d:%02d", h, m, s); 
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

// Funkcja wywoływana co 50ms w pętli loop, gwarantuje płynne pulsowanie
void aktualizujStanIKolory() {
  if (tryb_awaryjny) aktualny_kolor = "red";
  else if (ema_p_total > 100) aktualny_kolor = "red";       
  else if (ema_p_total < -100) aktualny_kolor = "green";
  else if (aktualne_pwm > 102 && tryb_auto) aktualny_kolor = "orange";    
  else aktualny_kolor = "blue";

  if (aktualny_kolor == "red") {
    if (tryb_awaryjny && (millis() % 500 < 250)) rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
    else rgb_led.setPixelColor(0, rgb_led.Color(255, 0, 0));
  } 
  else if (aktualny_kolor == "green") rgb_led.setPixelColor(0, rgb_led.Color(0, 255, 0));
  else if (aktualny_kolor == "orange") rgb_led.setPixelColor(0, rgb_led.Color(255, 100, 0));
  else if (aktualny_kolor == "blue") {
    if (WiFi.status() != WL_CONNECTED) {
        // Płynne, matematyczne pulsowanie (od 10 do 255 jasności), gdy jesteśmy w trybie Portalu AP
        int brightness = (abs((int)(millis() % 2000) - 1000) * 255) / 1000;
        brightness = map(brightness, 0, 255, 10, 255); 
        rgb_led.setPixelColor(0, rgb_led.Color(0, 0, brightness));
    } else {
        // Płytka podłączona do sieci -> Stały niebieski
        rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 255));
    }
  }
  rgb_led.show();
}

// =========================================================
// WYSYŁKA DO GOOGLE
// =========================================================
void syncWithGoogle() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  WiFiClientSecure client; 
  client.setInsecure(); 
  HTTPClient http;
  
  String url = GOOGLE_SCRIPT_URL + "?export=" + (force_zero_sync ? "0" : String(today_export_kwh, 3)) + 
               "&import=" + (force_zero_sync ? "0" : String(today_import_kwh, 3));
               
  http.begin(client, url); 
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  if (http.GET() == 200) {
    trigger_google_sync = false;
    force_zero_sync = false;
  } else {
    nvm.putDouble("exp_kwh", total_export_kwh);
    nvm.putDouble("imp_kwh", total_import_kwh);
  }
  http.end();
}

// =========================================================
// OBSŁUGA WEBSOCKET 
// =========================================================
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) return;
    
    if (doc.containsKey("mode")) tryb_auto = (doc["mode"] == "auto");
    if (doc.containsKey("pwm") && !tryb_auto) aktualne_pwm = ((int)doc["pwm"] * 1023) / 100;
    
    if (doc.containsKey("cmd")) {
      String cmd = doc["cmd"];
      if (cmd == "reboot") { delay(500); ESP.restart(); }
      else if (cmd == "reset_wifi") { wm.resetSettings(); delay(1000); ESP.restart(); }
      else if (cmd == "reset_kwh") {
        total_export_kwh = 0.0; total_import_kwh = 0.0; today_export_kwh = 0.0; today_import_kwh = 0.0;
        time_t now; time(&now); start_timestamp = (uint32_t)now; last_timestamp = (uint32_t)now;
        nvm.putDouble("exp_kwh", total_export_kwh); nvm.putDouble("imp_kwh", total_import_kwh); 
        nvm.putUInt("time_start", start_timestamp); nvm.putUInt("time_last", last_timestamp);
      }
    }
  }
}

void wyslijDaneWebsocket(bool sendLive) {
  JsonDocument doc;
  doc["grid_p"] = (int)p_total; 
  doc["heater_pwm"] = (aktualne_pwm * 100) / 1023;
  doc["heater_active"] = (aktualne_pwm > 0); 
  doc["ssr_v"] = ssr_v;
  doc["mode"] = tryb_auto ? "auto" : "manual"; 
  doc["uptime"] = getUptime();
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["sys_color"] = aktualny_kolor; 
  doc["fw_version"] = FW_VERSION; 
  doc["cloud_url"] = GOOGLE_SCRIPT_URL; 
  doc["cpu_temp"] = temperatureRead(); 
  doc["grid_hz"] = licznik_atm.GetFrequency(); 
  doc["export_kwh"] = total_export_kwh; 
  doc["import_kwh"] = total_import_kwh;
  doc["date_start"] = formatTimestamp(start_timestamp); 
  doc["date_last"]  = formatTimestamp(last_timestamp);               
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char time_str[10]; strftime(time_str, sizeof(time_str), "%H:%M:%S", &timeinfo); doc["clock"] = String(time_str);
  } else doc["clock"] = "Brak sync";

  JsonArray phases = doc.createNestedArray("phases");
  JsonObject l1 = phases.createNestedObject(); l1["v"] = licznik_atm.GetLineVoltageA(); l1["a"] = licznik_atm.GetLineCurrentA(); l1["p"] = licznik_atm.GetActivePowerA();
  JsonObject l2 = phases.createNestedObject(); l2["v"] = licznik_atm.GetLineVoltageB(); l2["a"] = licznik_atm.GetLineCurrentB(); l2["p"] = licznik_atm.GetActivePowerB();
  JsonObject l3 = phases.createNestedObject(); l3["v"] = licznik_atm.GetLineVoltageC(); l3["a"] = licznik_atm.GetLineCurrentC(); l3["p"] = licznik_atm.GetActivePowerC();
  
  JsonObject dips = doc.createNestedObject("dips");
  dips["1"] = (digitalRead(PIN_DIP1) == LOW); dips["2"] = (digitalRead(PIN_DIP2) == LOW);
  dips["3"] = (digitalRead(PIN_DIP3) == LOW); dips["4"] = (digitalRead(PIN_DIP4) == LOW);

  if (sendLive) {
    JsonArray live = doc.createNestedArray("live_data");
    for(int i=0; i<60; i++) live.add((int)live_history[i]);
  }

  String jsonString; serializeJson(doc, jsonString); ws.textAll(jsonString);
}

// =========================================================
// FREERTOS: Zadanie migania diodą (Teal / Morski) podczas szukania WiFi
// =========================================================
TaskHandle_t blinkTask;
void blinkTealTask(void * pvParameters) {
    bool state = false;
    for(;;) {
        state = !state;
        if(state) rgb_led.setPixelColor(0, rgb_led.Color(0, 128, 128)); // Morski (Teal)
        else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
        rgb_led.show();
        vTaskDelay(250 / portTICK_PERIOD_MS); // Miga szybko (4 razy na sekundę)
    }
}

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200); delay(3000); 
  Serial.println("\n\n===============================================");
  Serial.println("        START SYSTEMU MSAG v1.16 PRO");
  Serial.println("===============================================");
  
  for(int i=0; i<60; i++) live_history[i] = 0;

  pinMode(PIN_RST_OUT, OUTPUT); digitalWrite(PIN_RST_OUT, LOW); 
  pinMode(PIN_RST_IN, INPUT_PULLUP); 
  
  nvm.begin("msag", false);
  total_export_kwh = nvm.getDouble("exp_kwh", 0.0); total_import_kwh = nvm.getDouble("imp_kwh", 0.0);
  start_timestamp  = nvm.getUInt("time_start", 0); last_timestamp   = nvm.getUInt("time_last", 0);
  
  pinMode(PIN_LED1, OUTPUT); pinMode(PIN_LED2, OUTPUT);
  pinMode(PIN_SSR_SENSE, INPUT); 
  pinMode(PIN_DIP1, INPUT_PULLUP); pinMode(PIN_DIP2, INPUT_PULLUP);
  pinMode(PIN_DIP3, INPUT_PULLUP); pinMode(PIN_DIP4, INPUT_PULLUP);
  
  ledcAttach(PIN_PWM_OUT, pwmFreq, pwmResolution); ledcWrite(PIN_PWM_OUT, 0); 
  obliczMocGrzalki();

  rgb_led.begin(); rgb_led.setBrightness(50);
  rgb_led.setPixelColor(0, rgb_led.Color(128, 0, 128)); rgb_led.show();

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS);
  pinMode(PIN_SPI_CS, OUTPUT); digitalWrite(PIN_SPI_CS, LOW);
  SPI.transfer16(0x0070); SPI.transfer16(0x789A); digitalWrite(PIN_SPI_CS, HIGH);
  delay(100);
  licznik_atm.begin(PIN_SPI_CS, 50, 0, 8000, 8000, 8000, 8000);
  
  if(!LittleFS.begin(true)) Serial.println("[BŁĄD] LittleFS");

  WiFi.mode(WIFI_STA); wm.setHostname(HOSTNAME);
  wm.setConfigPortalBlocking(false); 
  
  Serial.println("\n[WIFI] Inicjalizacja... Szukam zapisanej sieci.");
  
  // Uruchomienie bezpiecznego, sprzętowego migania diodą na Morski kolor w tle
  xTaskCreate(blinkTealTask, "BlinkTask", 2048, NULL, 1, &blinkTask);

  wm.setConnectTimeout(20); // Zmniejszony czas szukania routera z 60s do 20s
  wm.setAPCallback([](WiFiManager *myWiFiManager) {
      Serial.println("\n[WIFI] Brak zasięgu routera. Uruchomiono Portal AP: " + String(myWiFiManager->getConfigPortalSSID()));
  });

  // Próba połączenia (blokuje na 20s)
  wm.autoConnect("MSAG-Konfiguracja");

  // Usuwamy zadanie migania po wyjściu z sekwencji startowej
  if(blinkTask != NULL) {
      vTaskDelete(blinkTask);
      blinkTask = NULL;
  }

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  ws.onEvent(onWsEvent); server.addHandler(&ws);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  ElegantOTA.begin(&server); server.begin(); 
}

// =========================================================
// PĘTLA GŁÓWNA
// =========================================================
void loop() {
  wm.process(); 
  ws.cleanupClients();
  ElegantOTA.loop();

  // Aktualizacja kolorów diody płynnie co 50ms
  if (millis() - last_led_update >= 50) {
      last_led_update = millis();
      aktualizujStanIKolory();
  }

  // --- ZWORKA (RESET WIFI) ---
  if (digitalRead(PIN_RST_IN) == LOW) {
      if (hw_reset_start == 0) hw_reset_start = millis();
      if (millis() - hw_reset_start > 5000) { wm.resetSettings(); delay(1000); ESP.restart(); }
  } else { hw_reset_start = 0; }

  // --- CICHY RECONNECT ---
  // Działa idealnie w tle. Jak router wróci po 10 min, połączy się z nim w kilka sekund.
  if (WiFi.status() != WL_CONNECTED && millis() - last_wifi_check >= 30000) {
      last_wifi_check = millis(); 
      if (WiFi.SSID() != "") { 
          Serial.println("[WIFI] Oczekuję na domowy router (szukam w tle)...");
          WiFi.begin(); 
      } else {
          Serial.println("[WIFI] Brak konfiguracji. Oczekuję na połączenie z portalem MSAG-Konfiguracja...");
      }
  }

  ssr_v = (analogRead(PIN_SSR_SENSE) * 3.3 / 4095.0) * 3.0;

  // 1. REGULATOR PWM - MARTWA STREFA (Co 333ms)
  if (millis() - last_control_loop >= 333) {
    last_control_loop = millis();
    p_total = licznik_atm.GetTotalActivePower();
    ema_p_total = (EMA_ALPHA * p_total) + ((1.0 - EMA_ALPHA) * ema_p_total);

    if (tryb_auto && !tryb_awaryjny) {
      if (ema_p_total > -50) aktualne_pwm -= 40; 
      else if (ema_p_total < -100) aktualne_pwm += 20;
      if (aktualne_pwm < 0) aktualne_pwm = 0;
      if (aktualne_pwm > 1023) aktualne_pwm = 1023;
    }
    if (tryb_awaryjny) aktualne_pwm = 0;
    ledcWrite(PIN_PWM_OUT, aktualne_pwm);
  }

  // 2. LICZNIKI, ZEGAR 1Hz ORAZ BUFOR LIVE
  static int live_tick_counter = 0;
  if (millis() - last_ws_update >= 1000) {
    last_ws_update = millis();
    obliczMocGrzalki();

    double recent_export = licznik_atm.GetExportEnergy(); double recent_import = licznik_atm.GetImportEnergy();
    if (recent_export > 0) { total_export_kwh += recent_export; today_export_kwh += recent_export; }
    if (recent_import > 0) { total_import_kwh += recent_import; today_import_kwh += recent_import; }

    if (aktualne_pwm > 512 && ssr_v < 1.0) tryb_awaryjny = true;

    struct tm ti;
    if (getLocalTime(&ti) && ti.tm_year > 123) {
      static int last_min = -1;
      if (ti.tm_min != last_min) {
        last_min = ti.tm_min;
        if (ti.tm_hour == 0 && ti.tm_min == 0) { today_export_kwh = 0; today_import_kwh = 0; }
        else if (ti.tm_hour == 0 && ti.tm_min == 5) { trigger_google_sync = true; force_zero_sync = true; }
        else if (ti.tm_hour >= 1 && ti.tm_min == 0) { trigger_google_sync = true; force_zero_sync = false; }
        else if (ti.tm_hour == 23 && ti.tm_min == 59) { trigger_google_sync = true; force_zero_sync = false; }
      }
    }

    // Dodanie do bufora co 5 sekund
    live_tick_counter++;
    bool send_live_now = false;
    if (live_tick_counter >= 5) {
        live_tick_counter = 0;
        for(int i = 0; i < 59; i++) live_history[i] = live_history[i+1];
        live_history[59] = ema_p_total;
        send_live_now = true;
    }

    // Wysyłka na bieżąco 1Hz (z live arrays co 5s)
    if(WiFi.status() == WL_CONNECTED) wyslijDaneWebsocket(send_live_now);
    digitalWrite(PIN_LED1, !digitalRead(PIN_LED1)); 
  }

  // 3. WYSYŁKA DO GOOGLE 
  if (trigger_google_sync && millis() - last_google_try >= 15000) {
    last_google_try = millis();
    syncWithGoogle();
  }
}