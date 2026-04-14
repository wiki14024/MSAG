/* ====================================================================
 * PROJEKT: MSAG v1.15 - Sterownik Autokonsumpcji PV (Wersja PRO)
 * ==================================================================== 
 * ZMIANY: Przemysłowa stabilność WiFi. Usunięto agresywnego WDT.
 * Izolacja serwerów: Portal konfiguracyjny działa TYLKO przy braku 
 * zapisanej sieci. W przypadku awarii routera -> Cichy autokonnect.
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
const String FW_VERSION = "v1.15";
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

// PINY DO FIZYCZNEGO RESETU WIFI
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

// --- ZMIENNE GLOBALNE ---
float p_max_heater = 0.0; 
unsigned long last_control_loop = 0;
unsigned long last_ws_update = 0;
unsigned long last_wifi_check = 0; 
unsigned long last_google_try = 0;
unsigned long hw_reset_start = 0;

int last_saved_hour = -1; 
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

// =========================================================
// FUNKCJE POMOCNICZE
// =========================================================
String formatTimestamp(uint32_t timestamp) {
  if (timestamp < 100000) return "Brak danych"; 
  time_t t = timestamp;
  struct tm *tm_info = localtime(&t);
  char buf[20]; strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", tm_info); return String(buf);
}

String getUptime() {
  unsigned long secs = millis() / 1000;
  unsigned long h = secs / 3600; int m = (secs % 3600) / 60; int s = secs % 60;
  char buf[16]; snprintf(buf, sizeof(buf), "%lu:%02d:%02d", h, m, s); return String(buf);
}

void obliczMocGrzalki() {
  p_max_heater = 0.0; 
  if (digitalRead(PIN_DIP1) == LOW) p_max_heater += 4000.0;
  if (digitalRead(PIN_DIP2) == LOW) p_max_heater += 2000.0;
  if (digitalRead(PIN_DIP3) == LOW) p_max_heater += 1000.0;
  if (digitalRead(PIN_DIP4) == LOW) p_max_heater += 500.0;
  if (p_max_heater == 0) p_max_heater = 1000.0; 
}

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
    if (WiFi.status() != WL_CONNECTED && (millis() % 2000 < 1000)) rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 50)); // Miga na ciemno niebiesko gdy szuka sieci
    else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 255));
  }

  rgb_led.show();
}

// =========================================================
// WYSYŁKA DO GOOGLE
// =========================================================
void syncWithGoogle() {
  WiFiClientSecure client; client.setInsecure(); HTTPClient http;
  String url = GOOGLE_SCRIPT_URL + "?export=" + String(today_export_kwh, 3) + "&import=" + String(today_import_kwh, 3);
  http.begin(client, url); http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); 
  if (http.GET() == 200) trigger_google_sync = false;
  http.end();
}

// =========================================================
// OBSŁUGA WEBSOCKET 
// =========================================================
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA) {
    JsonDocument doc; if (deserializeJson(doc, data, len)) return;
    if (doc.containsKey("mode")) tryb_auto = (doc["mode"] == "auto");
    if (doc.containsKey("pwm") && !tryb_auto) aktualne_pwm = ((int)doc["pwm"] * 1023) / 100;

    if (doc.containsKey("cmd")) {
      String cmd = doc["cmd"];
      if (cmd == "reboot") { delay(500); ESP.restart(); }
      else if (cmd == "reset_wifi") {
        WiFiManager wm; wm.resetSettings();
        delay(1000); ESP.restart();
      }
      else if (cmd == "reset_kwh") {
        total_export_kwh = 0.0; total_import_kwh = 0.0; today_export_kwh = 0.0; today_import_kwh = 0.0;
        time_t now; time(&now); start_timestamp = (uint32_t)now; last_timestamp = (uint32_t)now;
        nvm.putDouble("exp_kwh", total_export_kwh); nvm.putDouble("imp_kwh", total_import_kwh); 
        nvm.putUInt("time_start", start_timestamp); nvm.putUInt("time_last", last_timestamp);
      }
    }
  }
}

void wyslijDaneWebsocket() {
  JsonDocument doc; 
  doc["grid_p"] = (int)p_total; doc["heater_pwm"] = (aktualne_pwm * 100) / 1023;
  doc["heater_active"] = (aktualne_pwm > 0); doc["ssr_v"] = ssr_v;
  doc["mode"] = tryb_auto ? "auto" : "manual"; doc["uptime"] = getUptime();
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["sys_color"] = aktualny_kolor; doc["fw_version"] = FW_VERSION; 
  doc["cpu_temp"] = temperatureRead(); doc["grid_hz"] = licznik_atm.GetFrequency(); 
  doc["export_kwh"] = total_export_kwh; doc["import_kwh"] = total_import_kwh;
  doc["date_start"] = formatTimestamp(start_timestamp); doc["date_last"]  = formatTimestamp(last_timestamp);               
  
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

  String jsonString; serializeJson(doc, jsonString); ws.textAll(jsonString);
}

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200);
  delay(3000); 
  
  Serial.println("\n\n===============================================");
  Serial.println("        START SYSTEMU MSAG v1.15 PRO");
  Serial.println("===============================================");

  // --- PINY RESETU ---
  pinMode(PIN_RST_OUT, OUTPUT); digitalWrite(PIN_RST_OUT, LOW); 
  pinMode(PIN_RST_IN, INPUT_PULLUP); 
  
  nvm.begin("msag", false);
  total_export_kwh = nvm.getDouble("exp_kwh", 0.0); total_import_kwh = nvm.getDouble("imp_kwh", 0.0);
  start_timestamp  = nvm.getUInt("time_start", 0); last_timestamp   = nvm.getUInt("time_last", 0);
  
  pinMode(PIN_LED1, OUTPUT); pinMode(PIN_LED2, OUTPUT); pinMode(PIN_SSR_SENSE, INPUT); 
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

  // =========================================================
  // KULOODPORNA LOGIKA WIFI - POPRAWIONA
  // =========================================================
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  
  // Wywołanie begin() bez parametrów zmusza ESP do pobrania SSID i hasła z pamięci NVS
  WiFi.begin(); 
  
  Serial.println("[WIFI] Próba połączenia (szukam zapisanej sieci)...");

  // Czekamy maksymalnie 10 sekund na połączenie przed puszczeniem pętli dalej
  int retries = 20;
  while (WiFi.status() != WL_CONNECTED && retries > 0) {
      delay(500);
      Serial.print(".");
      retries--;
  }

  if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n=== POŁĄCZONO Z WI-FI ===");
      Serial.print("Adres IP urządzenia: "); Serial.println(WiFi.localIP());
      if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
      configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  } else {
      // Nie połączono w 10 sekund. Teraz sprawdzamy dlaczego.
      String savedSSID = WiFi.SSID();

      if (savedSSID == "") {
          Serial.println("\n[WIFI] Brak hasła w pamięci! Uruchamiam Portal Konfiguracyjny (BLOCKING).");
          rgb_led.setPixelColor(0, rgb_led.Color(255, 255, 255)); rgb_led.show(); // BIAŁY
          
          WiFiManager wm;
          wm.setHostname(HOSTNAME);
          // Portal blokuje kod. Gwarantuje 100% stabilności zapisu.
          wm.startConfigPortal("MSAG-Konfiguracja"); 
          
          Serial.println("[WIFI] Zapisano nowe ustawienia. Wymuszam restart!");
          delay(1000);
          ESP.restart(); // Czysty start po otrzymaniu hasła
      } else {
          Serial.println("\n[WIFI] Router (" + savedSSID + ") nie odpowiada.");
          Serial.println("[WIFI] Przechodzę w tryb AUTONOMICZNY (Ciche wznowienia).");
      }
  }

  // --- URUCHOMIENIE GŁÓWNEGO SERWERA MSAG (Zawsze na końcu) ---
  ws.onEvent(onWsEvent); server.addHandler(&ws);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  ElegantOTA.begin(&server); 
  server.begin(); 
}

// =========================================================
// PĘTLA GŁÓWNA
// =========================================================
void loop() {
  ws.cleanupClients();
  ElegantOTA.loop(); 
  
  // --- ZWORKA 6 I 7 (HARDWARE RESET WIFI) ---
  if (digitalRead(PIN_RST_IN) == LOW) {
      if (hw_reset_start == 0) {
          hw_reset_start = millis();
          Serial.println("\n[HARDWARE] Wykryto zwarcie pinów 6 i 7. Trzymaj 5 sekund!");
      }
      if (millis() - hw_reset_start > 5000) {
          Serial.println("[HARDWARE] KASOWANIE PAMIĘCI WIFI I RESTART!");
          WiFiManager wm; wm.resetSettings();
          delay(1000);
          ESP.restart();
      }
  } else {
      if (hw_reset_start > 0 && (millis() - hw_reset_start < 5000)) Serial.println("[HARDWARE] Przerwano zwarcie.");
      hw_reset_start = 0; 
  }

  // --- CICHY RECONNECT (Co 30 sekund jeśli padnie router) ---
  if (WiFi.status() != WL_CONNECTED) {
      if (millis() - last_wifi_check >= 30000) {
          last_wifi_check = millis();
          Serial.println("[WIFI] Próba cichego wznowienia połączenia z routerem...");
          WiFi.disconnect(); // Czyszczenie stanów przed nową próbą
          WiFi.reconnect();
      }
  }

  ssr_v = (analogRead(PIN_SSR_SENSE) * 3.3 / 4095.0) * 3.0;

  // 1. Regulator (Działa niezależnie od awarii WiFi!)
  if (millis() - last_control_loop >= 333) {
    last_control_loop = millis();
    p_total = licznik_atm.GetTotalActivePower();
    ema_p_total = (EMA_ALPHA * p_total) + ((1.0 - EMA_ALPHA) * ema_p_total);

    if (tryb_auto && !tryb_awaryjny) {
      if (ema_p_total > 0) {
        float moc_do_obciecia = ema_p_total * 1.5;
        int redukcja_pwm = (moc_do_obciecia / p_max_heater) * 1023.0;
        if (redukcja_pwm < 0) redukcja_pwm = 0;
        aktualne_pwm -= redukcja_pwm;
        if (aktualne_pwm < 0) aktualne_pwm = 0;
      } 
      else if (ema_p_total < -50) {
        if (aktualne_pwm < 1023) {
          aktualne_pwm += 68; 
          if (aktualne_pwm > 1023) aktualne_pwm = 1023; 
        }
      }
    }

    if (tryb_awaryjny) aktualne_pwm = 0; 
    ledcWrite(PIN_PWM_OUT, aktualne_pwm);
    aktualizujStanIKolory(); 
  }

  // 2. Odczyt Zegara i NVM (1 Hz)
  if (millis() - last_ws_update >= 1000) {
    last_ws_update = millis();
    obliczMocGrzalki();

    double recent_export = licznik_atm.GetExportEnergy(); double recent_import = licznik_atm.GetImportEnergy();
    if (recent_export > 0) { total_export_kwh += recent_export; today_export_kwh += recent_export; }
    if (recent_import > 0) { total_import_kwh += recent_import; today_import_kwh += recent_import; }

    if (aktualne_pwm > 512 && ssr_v < 1.0) tryb_awaryjny = true;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      int obecna_godzina = timeinfo.tm_hour; int obecna_minuta  = timeinfo.tm_min; time_t now; time(&now);

      if (start_timestamp < 100000 && timeinfo.tm_year > 123) { start_timestamp = (uint32_t)now; nvm.putUInt("time_start", start_timestamp); }

      static int last_action_minute = -1; 
      if (obecna_minuta != last_action_minute && timeinfo.tm_year > 123) {
        if (obecna_godzina >= 1 && obecna_minuta == 0) trigger_google_sync = true;
        else if (obecna_godzina == 23 && obecna_minuta == 59) trigger_google_sync = true;
        else if (obecna_godzina == 0 && obecna_minuta == 0) { today_export_kwh = 0.0; today_import_kwh = 0.0; }
        else if (obecna_godzina == 0 && obecna_minuta == 5) trigger_google_sync = true;
        last_action_minute = obecna_minuta;
      }

      if ((obecna_godzina == 8 || obecna_godzina == 12 || obecna_godzina == 16 || obecna_godzina == 20) && last_saved_hour != obecna_godzina) {
        last_saved_hour = obecna_godzina; last_timestamp = (uint32_t)now;
        nvm.putDouble("exp_kwh", total_export_kwh); nvm.putDouble("imp_kwh", total_import_kwh); nvm.putUInt("time_last", last_timestamp);
      }
    }

    if(WiFi.status() == WL_CONNECTED) wyslijDaneWebsocket();
    digitalWrite(PIN_LED1, !digitalRead(PIN_LED1)); 
  }

  // 3. Wysyłka do GOOGLE
  if (millis() - last_google_try >= 15000) {
    last_google_try = millis();
    if (trigger_google_sync && WiFi.status() == WL_CONNECTED) syncWithGoogle();
  }
}