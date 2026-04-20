/* ====================================================================
 * PROJEKT: MSAG v1.18 PRO - Sterownik Autokonsumpcji PV
 * ZMIANY v1.18: 
 *   - Fix: reset rst_start po puszczeniu przycisku
 *   - Fix: zwiększony timeout mutexu (50→200ms)
 *   - Fix: atomowy odczyt double dla WebSocket
 *   - Fix: sprawdzenie overflow JSON
 *   - Fix: timeout HTTP skrócony do 5s
 *   - Optymalizacja: loop() używa danych z ControlTask zamiast ponownych odczytów
 *   - Fix: rozmiar bufora w formatTimestamp (20→32)
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
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbwhYfMLe6PS3PxiIYSjvXA-fwvaT6TM6Ta97vp9TzwSErPxCwhNvsFWI8d1dCimNf-w/exec";
const String FW_VERSION = "v1.18 PRO";
const char* HOSTNAME = "msag"; 

#define PIN_DIP1 20   // Twoja nóżka nr 1 (lewa) -> 4kW
#define PIN_DIP2 21   // Twoja nóżka nr 2 -> 2kW
#define PIN_DIP3 22   // Twoja nóżka nr 3 -> 1kW
#define PIN_DIP4 23   // Twoja nóżka nr 4 (prawa) -> 0.5kW
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
SemaphoreHandle_t dataMutex; // NOWY: MUTEX DO OCHRONY DANYCH WSPÓŁDZIELONYCH

float p_max_heater = 0.0;
unsigned long last_ws_update = 0;
unsigned long last_google_try = 0;
unsigned long last_led_update = 0;
volatile int aktualne_pwm = 0;
volatile bool tryb_auto = true;
volatile bool tryb_awaryjny = false;
unsigned long awaryjny_timer = 0;
float p_total = 0.0;
float ema_p_total = 0.0;
const float EMA_ALPHA = 0.4;
float ssr_v = 0.0;

double total_export_kwh = 0.0, total_import_kwh = 0.0;
double today_export_kwh = 0.0, today_import_kwh = 0.0;
float live_history[60];

// Dane fazowe do WebSocket (teraz chronione mutexem)
float phase_voltage[3] = {0, 0, 0};
float phase_current[3] = {0, 0, 0};
float phase_power[3] = {0, 0, 0};
float phase_angle[3] = {0, 0, 0};

// --- TASK: KONTROLA PWM I ODCZYT MOCY (WYSOKI PRIORYTET) ---
void ControlTask(void *pvParameters) {
    for(;;) {
        // Bezpieczny odczyt z licznika
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            p_total = licznik_atm.GetTotalActivePower();
            
            // Odczytujemy też dane fazowe
            float v1 = licznik_atm.GetLineVoltageA();
            float v2 = licznik_atm.GetLineVoltageB();
            float v3 = licznik_atm.GetLineVoltageC();
            float a1 = licznik_atm.GetLineCurrentA();
            float a2 = licznik_atm.GetLineCurrentB();
            float a3 = licznik_atm.GetLineCurrentC();
            float p1 = licznik_atm.GetActivePowerA();
            float p2 = licznik_atm.GetActivePowerB();
            float p3 = licznik_atm.GetActivePowerC();
            float ang1 = licznik_atm.GetPhaseA();
            float ang2 = licznik_atm.GetPhaseB();
            float ang3 = licznik_atm.GetPhaseC();
            
            xSemaphoreGive(spiMutex);
            
            // Aktualizacja globalnych zmiennych z ochroną mutexem
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                phase_voltage[0] = v1; phase_voltage[1] = v2; phase_voltage[2] = v3;
                phase_current[0] = a1; phase_current[1] = a2; phase_current[2] = a3;
                phase_power[0] = p1;   phase_power[1] = p2;   phase_power[2] = p3;
                phase_angle[0] = ang1; phase_angle[1] = ang2; phase_angle[2] = ang3;
                xSemaphoreGive(dataMutex);
            }
        }

        ema_p_total = (EMA_ALPHA * p_total) + ((1.0 - EMA_ALPHA) * ema_p_total);
        
        // Sprawdzanie warunku awaryjnego
        if (aktualne_pwm > 512 && ssr_v < 1.0) {
            if (!tryb_awaryjny) {
                tryb_awaryjny = true;
                awaryjny_timer = millis();
                Serial.println("[ALARM] Wykryto brak napięcia SSR! Tryb awaryjny.");
            }
        }
        else if (tryb_awaryjny) {
            if (ssr_v > 1.5 || (aktualne_pwm == 0 && millis() - awaryjny_timer > 10000)) {
                tryb_awaryjny = false;
                Serial.println("[ALARM] Wyjście z trybu awaryjnego.");
            }
        }
        
        if (tryb_auto && !tryb_awaryjny) {
            if (ema_p_total > -50) aktualne_pwm -= 40; 
            else if (ema_p_total < -100) aktualne_pwm += 20;
            aktualne_pwm = constrain(aktualne_pwm, 0, 1023);
        }
        
        ledcWrite(PIN_PWM_OUT, tryb_awaryjny ? 0 : aktualne_pwm);
        
        vTaskDelay(pdMS_TO_TICKS(333));
    }
}

// --- FUNKCJE POMOCNICZE ---
String formatTimestamp(uint32_t timestamp) {
  if (timestamp < 100000) return "Brak danych";
  time_t t = timestamp; struct tm *tm_info = localtime(&t);
  char buf[32]; strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", tm_info);  // FIX: 20→32
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
    
    char timeStr[32];  // FIX: 20→32
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
    http.setTimeout(5000);  // FIX: 20000→5000 - nie blokujemy loop() na 20s
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
  // Zwiększony rozmiar JSON - 1536 → 2048 dla bezpieczeństwa
  StaticJsonDocument<2048> doc;
  
  doc["grid_p"] = (int)p_total;
  doc["heater_pwm"] = (aktualne_pwm * 100) / 1023;
  doc["heater_active"] = (aktualne_pwm > 0); 
  doc["ssr_v"] = ssr_v;
  doc["mode"] = tryb_auto ? "auto" : "manual"; 
  doc["uptime"] = getUptime();
  doc["fw_version"] = FW_VERSION; 
  doc["cpu_temp"] = temperatureRead();
  
  // FIX: Atomowy odczyt double przez mutex
  double exp_copy, imp_copy;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      exp_copy = total_export_kwh;
      imp_copy = total_import_kwh;
      xSemaphoreGive(dataMutex);
  } else {
      exp_copy = total_export_kwh;  // fallback - może być nieatomowe, ale lepsze niż nic
      imp_copy = total_import_kwh;
  }
  doc["export_kwh"] = exp_copy;
  doc["import_kwh"] = imp_copy;
  doc["cloud_url"] = GOOGLE_SCRIPT_URL;
  
  // Dodajemy dane fazowe (zabezpieczone mutexem)
  JsonArray phases = doc.createNestedArray("phases");
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      for(int i = 0; i < 3; i++) {
        JsonObject phase = phases.createNestedObject();
        phase["v"] = phase_voltage[i];
        phase["a"] = phase_current[i];
        phase["p"] = (int)phase_power[i];
      }
      xSemaphoreGive(dataMutex);
  }
  
  // Dodajemy stan DIP switchy
  JsonObject dips = doc.createNestedObject("dips");
  dips["1"] = (digitalRead(PIN_DIP1) == LOW);
  dips["2"] = (digitalRead(PIN_DIP2) == LOW);
  dips["3"] = (digitalRead(PIN_DIP3) == LOW);
  dips["4"] = (digitalRead(PIN_DIP4) == LOW);
  
  // Określamy kolor systemowy dla LED na stronie
  if (tryb_awaryjny) doc["sys_color"] = "red";
  else if (ema_p_total > 100) doc["sys_color"] = "red";
  else if (ema_p_total < -100) doc["sys_color"] = "green";
  else if (aktualne_pwm > 102) doc["sys_color"] = "orange";
  else doc["sys_color"] = "blue";
  
  struct tm ti; 
  if (getLocalTime(&ti)) { 
    char ts[10]; 
    strftime(ts, sizeof(ts), "%H:%M:%S", &ti); 
    doc["clock"] = String(ts); 
  }
  
  if (sendLive) { 
    JsonArray live = doc.createNestedArray("live_data"); 
    for(int i=0; i<60; i++) live.add((int)live_history[i]); 
  }
  
  // FIX: Sprawdzenie overflow przed wysłaniem
  if (doc.overflowed()) {
      Serial.println("[WS] OSTRZEŻENIE: JSON overflow - dane niekompletne!");
  }
  
  String js; 
  serializeJson(doc, js); 
  ws.textAll(js);
}

void onWsEvent(AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType t, void *a, uint8_t *d, size_t l) {
  if (t == WS_EVT_DATA) { 
    StaticJsonDocument<256> doc; 
    if (deserializeJson(doc, d, l)) return;
    
    if (doc.containsKey("mode")) tryb_auto = (doc["mode"] == "auto");
    if (doc.containsKey("pwm") && !tryb_auto) {
        int pwm_val = doc["pwm"];
        pwm_val = constrain(pwm_val, 0, 100);  // FIX: Walidacja zakresu
        aktualne_pwm = (pwm_val * 1023) / 100;
    }
    if (doc.containsKey("cmd")) { 
      String cmd = doc["cmd"]; 
      if (cmd == "reboot") {
        ws.textAll("{\"info\":\"Restartowanie...\"}");
        delay(100);
        ESP.restart();
      }
      else if (cmd == "reset_wifi") { 
        ws.textAll("{\"info\":\"Reset WiFi...\"}");
        wm.resetSettings(); 
        delay(100);
        ESP.restart(); 
      }
      else if (cmd == "reset_kwh") {
        // FIX: Atomowa aktualizacja double
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            total_export_kwh = 0.0;
            total_import_kwh = 0.0;
            today_export_kwh = 0.0;
            today_import_kwh = 0.0;
            xSemaphoreGive(dataMutex);
        }
        nvm.putDouble("exp_kwh", 0.0);
        nvm.putDouble("imp_kwh", 0.0);
        ws.textAll("{\"info\":\"Liczniki energii zresetowane\"}");
        Serial.println("[NVM] Liczniki energii wyzerowane przez użytkownika");
      }
    }
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n\nSTART SYSTEMU MSAG v1.18 PRO");
  
  spiMutex = xSemaphoreCreateMutex();
  dataMutex = xSemaphoreCreateMutex();  // NOWY MUTEX
  
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
  
  xTaskCreate(ControlTask, "Control_Task", 4096, NULL, 2, NULL);
}

// --- LOOP ---
void loop() {
  obslugaWiFi(); 
  ws.cleanupClients(); ElegantOTA.loop();

  if (millis() - last_led_update >= 50) { last_led_update = millis(); aktualizujStanIKolory(); }

  // FIX: Reset rst_start po puszczeniu przycisku
  static unsigned long rst_start = 0;
  if (digitalRead(PIN_RST_IN) == LOW) {
      if (rst_start == 0) rst_start = millis();
      if (millis() - rst_start > 5000) { 
          wm.resetSettings(); 
          ESP.restart(); 
      }
  } else {
      rst_start = 0;  // FIX: Reset timera po puszczeniu
  }

  ssr_v = (analogRead(PIN_SSR_SENSE) * 3.3 / 4095.0) * 3.0;

  // --- LOGI SERYJNE 1 HZ (używamy danych z ControlTask) ---
  static unsigned long last_serial_log = 0;
  if (millis() - last_serial_log >= 1000) {
      last_serial_log = millis();
      
      // FIX: Używamy danych z ControlTask zamiast ponownego odczytu
      float v1, v2, v3, a1, a2, a3, phi1, phi2, phi3;
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          v1 = phase_voltage[0]; a1 = phase_current[0]; phi1 = phase_angle[0];
          v2 = phase_voltage[1]; a2 = phase_current[1]; phi2 = phase_angle[1];
          v3 = phase_voltage[2]; a3 = phase_current[2]; phi3 = phase_angle[2];
          xSemaphoreGive(dataMutex);
          
          Serial.printf("LOG [%s] L1:%.1fV %.2fA %.1f° | L2:%.1fV %.2fA %.1f° | L3:%.1fV %.2fA %.1f° | PWM:%d%% | Tryb:%s\n", 
              getUptime().c_str(), 
              v1, a1, phi1, 
              v2, a2, phi2, 
              v3, a3, phi3, 
              (aktualne_pwm*100)/1023,
              tryb_awaryjny ? "AWARYJNY" : (tryb_auto ? "AUTO" : "MANUAL"));
      }
  }

  // OBSŁUGA CZASU, LICZNIKÓW I KOLEJKOWANIA
  if (millis() - last_ws_update >= 1000) { last_ws_update = millis();
    obliczMocGrzalki();
    // FIX: Zwiększony timeout mutexu (50→200ms)
    double re = 0.0, ri = 0.0;
    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        re = licznik_atm.GetExportEnergy(); 
        ri = licznik_atm.GetImportEnergy();
        xSemaphoreGive(spiMutex);
    } else {
        Serial.println("[OSTRZEŻENIE] Nie udało się odczytać energii z licznika (mutex timeout)");
    }
    
    // FIX: Atomowa aktualizacja double
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (re > 0) { total_export_kwh += re; today_export_kwh += re; }
        if (ri > 0) { total_import_kwh += ri; today_import_kwh += ri; }
        xSemaphoreGive(dataMutex);
    }
    
    struct tm ti; 
    if (getLocalTime(&ti) && ti.tm_year > 123) {
        
        static int last_sync_min = -1;
        if (ti.tm_min != last_sync_min) {
            last_sync_min = ti.tm_min;
            
            if (ti.tm_hour == 0 && ti.tm_min == 0) { 
                if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    today_export_kwh = 0; 
                    today_import_kwh = 0;
                    xSemaphoreGive(dataMutex);
                }
            }
            
            bool add_to_q = false;
            bool force_z = false;
            
            if (ti.tm_hour == 0 && ti.tm_min == 5) { add_to_q = true; force_z = true; }
            else if (ti.tm_min == 0 || ti.tm_min == 30) { add_to_q = true; force_z = false; }
            else if (ti.tm_hour == 23 && ti.tm_min == 59) { add_to_q = true; force_z = false; }
            
            if (add_to_q) {
                time_t now; time(&now);
                double exp_copy, imp_copy;
                if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    exp_copy = today_export_kwh;
                    imp_copy = today_import_kwh;
                    xSemaphoreGive(dataMutex);
                    dodajDoKolejki((uint32_t)now, exp_copy, imp_copy, force_z);
                }
            }
            
            static int last_saved_hour = -1;
            if (ti.tm_hour != last_saved_hour) {
                last_saved_hour = ti.tm_hour;
                if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    nvm.putDouble("exp_kwh", total_export_kwh);
                    nvm.putDouble("imp_kwh", total_import_kwh);
                    xSemaphoreGive(dataMutex);
                }
                Serial.println("[NVM] Zapisano stan liczników (1/h)");
            }
        }
    }

    static int lt = 0; if (++lt >= 5) { lt = 0;
      for(int i=0; i<59; i++) live_history[i] = live_history[i+1];
      live_history[59] = ema_p_total; 
      wyslijDaneWebsocket(true);
    } else {
      wyslijDaneWebsocket(false);
    }
  }

  // MECHANIZM ROZŁADOWANIA KOLEJKI DO GOOGLE
  if (queue_size > 0 && millis() - last_google_try >= 15000) {
    last_google_try = millis(); 
    rozladujKolejkeDoGoogle();
  }
}