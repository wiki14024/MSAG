/* ====================================================================
 * PROJEKT: MSAG v1.16 PRO - Sterownik Autokonsumpcji PV
 * ZMIANY: Buforowanie offline (Kolejka NVM), Timestamp ESP-Side, FreeRTOS Task, Logi Fazy
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
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbxSyhAEa9GBRaPFcB29TTm2KTVomfVwHMjd_FWGZS_nxbdR6pqymmmnGaTk93RbLbYD/exec";
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

// --- KOLEJKA OFFLINE DLA GOOGLE (Max 24 wpisy = 1 cała doba braku prądu/wifi) ---
struct PendingRecord {
    uint32_t timestamp;
    double exp;
    double imp;
    bool force_zero;
    bool is_offline;
};
PendingRecord syncQueue[24];
int queue_size = 0;

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
SemaphoreHandle_t spiMutex; // MUTEX DO OCHRONY SPI

float p_max_heater = 0.0;
unsigned long last_ws_update = 0;
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
float live_history[60];

// --- TASK: KONTROLA PWM I ODCZYT MOCY (WYSOKI PRIORYTET) ---
void ControlTask(void *pvParameters) {
    for(;;) {
        // Bezpieczny odczyt z licznika
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            p_total = licznik_atm.GetTotalActivePower();
            xSemaphoreGive(spiMutex);
        }

        ema_p_total = (EMA_ALPHA * p_total) + ((1.0 - EMA_ALPHA) * ema_p_total);
        
        if (tryb_auto && !tryb_awaryjny) {
            if (ema_p_total > -50) aktualne_pwm -= 40; 
            else if (ema_p_total < -100) aktualne_pwm += 20;
            aktualne_pwm = constrain(aktualne_pwm, 0, 1023);
        }
        
        ledcWrite(PIN_PWM_OUT, tryb_awaryjny ? 0 : aktualne_pwm);
        
        // Zwalniamy procesor na równe 333 ms
        vTaskDelay(pdMS_TO_TICKS(333));
    }
}

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

// --- SYSTEM SYGNALIZACJI LED ---
void aktualizujStanIKolory() {
    if (wifi_state == WIFI_CONNECTING) {
        if ((millis() % 500) < 250) rgb_led.setPixelColor(0, rgb_led.Color(0, 128, 128)); 
        else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
    }
    else if (wifi_state == WIFI_AP_MODE) {
        if ((millis() % 500) < 250) rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 255));
        else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
    }
    else { 
        if (tryb_awaryjny) {
            if (millis() % 500 < 250) rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
            else rgb_led.setPixelColor(0, rgb_led.Color(255, 0, 0)); 
        } else {
            if (ema_p_total > 100) rgb_led.setPixelColor(0, rgb_led.Color(255, 0, 0)); 
            else if (ema_p_total < -100) rgb_led.setPixelColor(0, rgb_led.Color(0, 255, 0)); 
            else if (aktualne_pwm > 102 && tryb_auto) rgb_led.setPixelColor(0, rgb_led.Color(255, 100, 0)); 
            else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 255)); 
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
                Serial.print("[WIFI] Przydzielony adres IP: "); Serial.println(WiFi.localIP());
                Serial.println("[WIFI] Dostęp: http://msag.local/");
                if (ap_is_running) { wm.stopConfigPortal(); ap_is_running = false; }
                wifi_state = WIFI_CONNECTED;
            } else if (millis() - wifi_state_timer > 15000) {
                Serial.println("[WIFI] Nie udało się połączyć. Start trybu AP...");
                if (!ap_is_running) { wm.startConfigPortal("MSAG-Konfiguracja"); ap_is_running = true; }
                wifi_state = WIFI_AP_MODE; last_reconnect_attempt = millis();
            }
            break;
        case WIFI_CONNECTED:
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("\n[WIFI] Utrata połączenia! Próba reconnectu...");
                wifi_state = WIFI_CONNECTING; wifi_state_timer = millis();
            }
            break;
        case WIFI_AP_MODE:
            if (wm.getWiFiSSID() != "" && (millis() - last_reconnect_attempt > 30000)) {
                last_reconnect_attempt = millis(); Serial.println("[WIFI] Sprawdzam po cichu czy router powrócił...");
                WiFi.begin(); 
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("\n[WIFI] Router powrócił! Połączenie odzyskane.");
                Serial.print("[WIFI] Przydzielony adres IP: "); Serial.println(WiFi.localIP());
                wm.stopConfigPortal(); ap_is_running = false; wifi_state = WIFI_CONNECTED;
            }
            break;
    }
}

// --- OBSŁUGA KOLEJKI NVM DLA GOOGLE ---
void zapiszKolejkeDoNVM() {
    nvm.putInt("q_size", queue_size);
    if (queue_size > 0) nvm.putBytes("queue", syncQueue, sizeof(PendingRecord) * queue_size);
}

void dodajDoKolejki(uint32_t ts, double e, double i, bool fz) {
    if (queue_size >= 24) {
        for(int j=0; j<23; j++) syncQueue[j] = syncQueue[j+1];
        queue_size = 23;
    }
    syncQueue[queue_size].timestamp = ts;
    syncQueue[queue_size].exp = e;
    syncQueue[queue_size].imp = i;
    syncQueue[queue_size].force_zero = fz;
    syncQueue[queue_size].is_offline = (WiFi.status() != WL_CONNECTED);
    queue_size++;
    zapiszKolejkeDoNVM();
    Serial.printf("[KOLEJKA] Dodano nowy pomiar. Oczekuje wpisów: %d\n", queue_size);
}

void rozladujKolejkeDoGoogle() {
    if (WiFi.status() != WL_CONNECTED || queue_size == 0) return;
    
    PendingRecord rec = syncQueue[0];
    
    char timeStr[20];
    time_t t = rec.timestamp; struct tm *ti = localtime(&t);
    strftime(timeStr, sizeof(timeStr), "%d.%m.%Y %H:%M", ti);
    
    String timeParam = String(timeStr);
    timeParam.replace(" ", "%20"); 

    int pv_flag = (rec.force_zero || rec.is_offline) ? 0 : 1;

    WiFiClientSecure client; client.setInsecure(); HTTPClient http;
    String url = GOOGLE_SCRIPT_URL + "?export=" + (rec.force_zero ? "0" : String(rec.exp, 3)) + 
                 "&import=" + (rec.force_zero ? "0" : String(rec.imp, 3)) +
                 "&time=" + timeParam +
                 "&pv_active=" + String(pv_flag);
                 
    http.begin(client, url); 
    http.setTimeout(20000); 
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    int httpCode = http.GET();
    if (httpCode == 200) { 
        Serial.println("[GOOGLE] Wysłano pomiar z godziny: " + String(timeStr));
        for(int i=0; i<queue_size-1; i++) syncQueue[i] = syncQueue[i+1];
        queue_size--;
        zapiszKolejkeDoNVM();
    } else {
        Serial.printf("[GOOGLE] Błąd wysyłki (HTTP %d). Ponowna próba za chwilę.\n", httpCode);
    }
    http.end();
}

// --- WEBSOCKET ---
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
  
  spiMutex = xSemaphoreCreateMutex(); // INICJALIZACJA MUTEXU
  
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
  
  queue_size = nvm.getInt("q_size", 0);
  if (queue_size > 0 && queue_size <= 24) {
      nvm.getBytes("queue", syncQueue, sizeof(PendingRecord) * queue_size);
      Serial.printf("[NVM] Przywrócono kolejkę zaległych pomiarów: %d wpisów\n", queue_size);
  } else { queue_size = 0; }
  
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS);
  licznik_atm.begin(PIN_SPI_CS, 50, 0, 8000, 8000, 8000, 8000);
  LittleFS.begin(true);

  WiFi.mode(WIFI_STA); wm.setHostname(HOSTNAME); wm.setConfigPortalBlocking(false);
  if (wm.getWiFiSSID() != "") { 
      Serial.println("[WIFI] Próba połączenia z zapamiętaną siecią...");
      WiFi.begin(); wifi_state = WIFI_CONNECTING; wifi_state_timer = millis(); 
  } else { 
      Serial.println("[WIFI] Brak sieci. Start AP MSAG-Konfiguracja.");
      wm.startConfigPortal("MSAG-Konfiguracja"); ap_is_running = true; wifi_state = WIFI_AP_MODE; 
  }

  if (MDNS.begin(HOSTNAME)) { MDNS.addService("http", "tcp", 80); }

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");
  ws.onEvent(onWsEvent); server.addHandler(&ws); 
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html"); 
  ElegantOTA.begin(&server); server.begin(); 
  
  // URUCHOMIENIE ZADANIA KONTROLNEGO
  xTaskCreate(ControlTask, "Control_Task", 4096, NULL, 2, NULL);
}

// --- LOOP ---
void loop() {
  obslugaWiFi(); 
  ws.cleanupClients(); ElegantOTA.loop();

  if (millis() - last_led_update >= 50) { last_led_update = millis(); aktualizujStanIKolory(); }

  if (digitalRead(PIN_RST_IN) == LOW) {
      static unsigned long rst_start = 0; if (rst_start == 0) rst_start = millis();
      if (millis() - rst_start > 5000) { wm.resetSettings(); ESP.restart(); }
  }

  ssr_v = (analogRead(PIN_SSR_SENSE) * 3.3 / 4095.0) * 3.0;

  // --- LOGI SERYJNE 1 HZ ---
  static unsigned long last_serial_log = 0;
  if (millis() - last_serial_log >= 1000) {
      last_serial_log = millis();
      
      if (wifi_state == WIFI_CONNECTED) {
          // Pobieramy dane z licznika zabezpieczone Mutexem
          if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
              float v1 = licznik_atm.GetLineVoltageA(); float a1 = licznik_atm.GetLineCurrentA(); float phi1 = licznik_atm.GetPhaseA();
              float v2 = licznik_atm.GetLineVoltageB(); float a2 = licznik_atm.GetLineCurrentB(); float phi2 = licznik_atm.GetPhaseB();
              float v3 = licznik_atm.GetLineVoltageC(); float a3 = licznik_atm.GetLineCurrentC(); float phi3 = licznik_atm.GetPhaseC();
              xSemaphoreGive(spiMutex);

              Serial.printf("LOG [%s] L1:%.1fV %.2fA %.1f° | L2:%.1fV %.2fA %.1f° | L3:%.1fV %.2fA %.1f° | PWM:%d%%\n", 
                  getUptime().c_str(), v1, a1, phi1, v2, a2, phi2, v3, a3, phi3, (aktualne_pwm*100)/1023);
          }
      }
  }

  // OBSŁUGA CZASU, LICZNIKÓW I KOLEJKOWANIA
  if (millis() - last_ws_update >= 1000) { last_ws_update = millis();
    obliczMocGrzalki();
    
    // Zabezpieczony Mutexem odczyt liczników energii
    double re = 0.0, ri = 0.0;
    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        re = licznik_atm.GetExportEnergy(); 
        ri = licznik_atm.GetImportEnergy();
        xSemaphoreGive(spiMutex);
    }
    
    if (re > 0) { total_export_kwh += re; today_export_kwh += re; }
    if (ri > 0) { total_import_kwh += ri; today_import_kwh += ri; }
    if (aktualne_pwm > 512 && ssr_v < 1.0) tryb_awaryjny = true;
    
    struct tm ti; 
    if (getLocalTime(&ti) && ti.tm_year > 123) {
        
        static int last_sync_min = -1;
        if (ti.tm_min != last_sync_min) {
            last_sync_min = ti.tm_min;
            
            if (ti.tm_hour == 0 && ti.tm_min == 0) { today_export_kwh = 0; today_import_kwh = 0; }
            
            bool add_to_q = false;
            bool force_z = false;
            
            if (ti.tm_hour == 0 && ti.tm_min == 5) { add_to_q = true; force_z = true; }
            else if (ti.tm_min == 0 || ti.tm_min == 30) { add_to_q = true; force_z = false; }
            else if (ti.tm_hour == 23 && ti.tm_min == 59) { add_to_q = true; force_z = false; }
            
            if (add_to_q) {
                time_t now; time(&now);
                dodajDoKolejki((uint32_t)now, today_export_kwh, today_import_kwh, force_z);
            }
            
            static int last_saved_hour = -1;
            if (ti.tm_hour != last_saved_hour) {
                last_saved_hour = ti.tm_hour;
                nvm.putDouble("exp_kwh", total_export_kwh);
                nvm.putDouble("imp_kwh", total_import_kwh);
                Serial.println("[NVM] Zapisano stan liczników (1/h)");
            }
        }
    }

    static int lt = 0; if (++lt >= 5) { lt = 0;
      for(int i=0; i<59; i++) live_history[i] = live_history[i+1];
      live_history[59] = ema_p_total; wyslijDaneWebsocket(true);
    } else wyslijDaneWebsocket(false);
  }

  // MECHANIZM ROZŁADOWANIA KOLEJKI DO GOOGLE
  if (queue_size > 0 && millis() - last_google_try >= 15000) {
    last_google_try = millis(); 
    rozladujKolejkeDoGoogle();
  }
}