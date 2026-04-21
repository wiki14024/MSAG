/* ====================================================================
 * PROJEKT: MSAG v1.18 PRO - Sterownik Autokonsumpcji PV
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

#define PIN_DIP1 20 
#define PIN_DIP2 21 
#define PIN_DIP3 22 
#define PIN_DIP4 23 
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

// --- KOLEJKA OFFLINE DLA GOOGLE ---
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
bool wifi_stable = false;
unsigned long wifi_stable_since = 0;

// --- OBIEKTY I ZMIENNE GLOBALNE ---
ATM90E32 licznik_atm{};
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Adafruit_NeoPixel rgb_led(NUMPIXELS, PIN_RGB, NEO_GRB + NEO_KHZ800);
Preferences nvm;
WiFiManager wm;
SemaphoreHandle_t spiMutex;
SemaphoreHandle_t dataMutex;

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

float phase_voltage[3] = {0, 0, 0};
float phase_current[3] = {0, 0, 0};
float phase_power[3] = {0, 0, 0};
float phase_angle[3] = {0, 0, 0};

// --- BEZPIECZNE FUNKCJE DO NADPISANIA REJESTRÓW (Używane tylko w setup) ---
void nadpiszRejestrATM(uint16_t adres, uint16_t wartosc) {
    SPI.beginTransaction(SPISettings(200000, MSBFIRST, SPI_MODE3));
    digitalWrite(PIN_SPI_CS, LOW);
    delayMicroseconds(10);
    SPI.transfer16(adres & 0x7FFF); 
    SPI.transfer16(wartosc);
    digitalWrite(PIN_SPI_CS, HIGH);
    SPI.endTransaction();
}

// --- TASK: KONTROLA PWM I ODCZYT MOCY CHWILOWEJ ---
void ControlTask(void *pvParameters) {
    for(;;) {
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            float p_meter = licznik_atm.GetTotalActivePower();
            float v1 = licznik_atm.GetLineVoltageA();
            float v2 = licznik_atm.GetLineVoltageB();
            float v3 = licznik_atm.GetLineVoltageC();
            float a1 = licznik_atm.GetLineCurrentA();
            float a2 = licznik_atm.GetLineCurrentB();
            float a3 = licznik_atm.GetLineCurrentC();
            float p1 = licznik_atm.GetActivePowerA();
            float p2 = licznik_atm.GetActivePowerB();
            float p3 = licznik_atm.GetActivePowerC();
            float ang1 = licznik_atm.GetSignedPhaseA();
            float ang2 = licznik_atm.GetSignedPhaseB();
            float ang3 = licznik_atm.GetSignedPhaseC();
            xSemaphoreGive(spiMutex);
            
            float p_sum = p1 + p2 + p3;
            p_total = (fabs(p_meter) > fabs(p_sum)) ? p_meter : p_sum;
            
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                phase_voltage[0] = v1; phase_voltage[1] = v2; phase_voltage[2] = v3;
                phase_current[0] = a1; phase_current[1] = a2; phase_current[2] = a3;
                phase_power[0] = p1;   phase_power[1] = p2;   phase_power[2] = p3;
                phase_angle[0] = ang1; phase_angle[1] = ang2; phase_angle[2] = ang3;
                xSemaphoreGive(dataMutex);
            }
        }

        ema_p_total = (EMA_ALPHA * p_total) + ((1.0 - EMA_ALPHA) * ema_p_total);
        
        if (aktualne_pwm > 512 && ssr_v < 1.0) {
            if (!tryb_awaryjny) {
                tryb_awaryjny = true;
                awaryjny_timer = millis();
                Serial.println("[ALARM] Wykryto brak napięcia SSR! Tryb awaryjny.");
                wifi_stable = false;
                wifi_stable_since = 0;
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

String formatTimestamp(uint32_t timestamp) {
  if (timestamp < 100000) return "Brak danych";
  time_t t = timestamp; struct tm *tm_info = localtime(&t);
  char buf[32]; strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", tm_info);
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

void aktualizujStanIKolory() {
    if (tryb_awaryjny) {
        if (millis() % 500 < 250) rgb_led.setPixelColor(0, rgb_led.Color(255, 0, 0));
        else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
        rgb_led.show();
        return;
    }
    if (!wifi_stable) {
        if (wifi_state == WIFI_CONNECTING) {
            if ((millis() % 500) < 250) rgb_led.setPixelColor(0, rgb_led.Color(0, 128, 128)); 
            else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
        }
        else if (wifi_state == WIFI_AP_MODE) {
            if ((millis() % 500) < 250) rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 255));
            else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
        }
        else { 
            rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 255));
        }
        rgb_led.show();
        return;
    }
    
    const float THRESHOLD = 50.0; 
    bool heater_on = (aktualne_pwm > 0);
    int color_r = 0, color_g = 0, color_b = 0;
    
    if (p_total > THRESHOLD) { color_r = 255; color_g = 0; color_b = 0; } 
    else if (p_total < -THRESHOLD) { color_r = 0; color_g = 255; color_b = 0; }
    else if (heater_on && fabs(p_total) <= THRESHOLD) { color_r = 255; color_g = 100; color_b = 0; }
    else { rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 255)); rgb_led.show(); return; }
    
    unsigned long ms = millis() % 5000;
    bool led_on = (ms < 500) || (ms >= 2500 && ms < 3000);
    if (led_on) rgb_led.setPixelColor(0, rgb_led.Color(color_r, color_g, color_b));
    else rgb_led.setPixelColor(0, rgb_led.Color(0, 0, 0));
    rgb_led.show();
}

void obslugaWiFi() {
    wm.process(); 
    switch (wifi_state) {
        case WIFI_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("\n[WIFI] Połączono z routerem!");
                if (ap_is_running) { wm.stopConfigPortal(); ap_is_running = false; }
                wifi_state = WIFI_CONNECTED;
                wifi_stable = false; wifi_stable_since = millis();
            } else if (millis() - wifi_state_timer > 15000) {
                if (!ap_is_running) { wm.startConfigPortal("MSAG-Konfiguracja"); ap_is_running = true; }
                wifi_state = WIFI_AP_MODE; last_reconnect_attempt = millis();
                wifi_stable = false; wifi_stable_since = 0;
            }
            break;
        case WIFI_CONNECTED:
            if (WiFi.status() != WL_CONNECTED) {
                wifi_state = WIFI_CONNECTING; wifi_state_timer = millis();
                wifi_stable = false; wifi_stable_since = 0;
            } else {
                if (!wifi_stable && wifi_stable_since > 0 && (millis() - wifi_stable_since >= 30000)) {
                    wifi_stable = true;
                }
            }
            break;
        case WIFI_AP_MODE:
            if (wm.getWiFiSSID() != "" && (millis() - last_reconnect_attempt > 30000)) {
                last_reconnect_attempt = millis(); WiFi.begin(); 
            }
            if (WiFi.status() == WL_CONNECTED) {
                wm.stopConfigPortal(); ap_is_running = false; wifi_state = WIFI_CONNECTED;
                wifi_stable = false; wifi_stable_since = millis();
            } else {
                wifi_stable = false; wifi_stable_since = 0;
            }
            break;
        default: break;
    }
}

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
}

void rozladujKolejkeDoGoogle() {
    if (WiFi.status() != WL_CONNECTED || queue_size == 0) return;
    
    PendingRecord rec = syncQueue[0];
    char timeStr[32];
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
                 
    http.begin(client, url); http.setTimeout(5000); http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    int httpCode = http.GET();
    if (httpCode == 200) { 
        for(int i=0; i<queue_size-1; i++) syncQueue[i] = syncQueue[i+1];
        queue_size--; zapiszKolejkeDoNVM();
    }
    http.end();
}

void wyslijDaneWebsocket(bool sendLive) {
  StaticJsonDocument<4096> doc;
  
  doc["grid_p"] = (int)p_total;
  doc["heater_pwm"] = (aktualne_pwm * 100) / 1023;
  doc["heater_active"] = (aktualne_pwm > 0); 
  doc["ssr_v"] = ssr_v;
  doc["mode"] = tryb_auto ? "auto" : "manual"; 
  doc["uptime"] = getUptime();
  doc["fw_version"] = FW_VERSION; 
  doc["cpu_temp"] = temperatureRead();
  
  double exp_copy, imp_copy;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      exp_copy = total_export_kwh;
      imp_copy = total_import_kwh;
      xSemaphoreGive(dataMutex);
  } else {
      exp_copy = total_export_kwh;
      imp_copy = total_import_kwh;
  }
  doc["export_kwh"] = exp_copy;
  doc["import_kwh"] = imp_copy;
  doc["cloud_url"] = GOOGLE_SCRIPT_URL;
  
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
  
  JsonObject dips = doc.createNestedObject("dips");
  dips["1"] = (digitalRead(PIN_DIP1) == LOW);
  dips["2"] = (digitalRead(PIN_DIP2) == LOW);
  dips["3"] = (digitalRead(PIN_DIP3) == LOW);
  dips["4"] = (digitalRead(PIN_DIP4) == LOW);
  
  if (tryb_awaryjny) doc["sys_color"] = "red";
  else if (ema_p_total > 100) doc["sys_color"] = "red";
  else if (ema_p_total < -100) doc["sys_color"] = "green";
  else if (aktualne_pwm > 102) doc["sys_color"] = "orange";
  else doc["sys_color"] = "blue";
  
  struct tm ti; 
  if (getLocalTime(&ti)) { 
    char ts[10]; strftime(ts, sizeof(ts), "%H:%M:%S", &ti); doc["clock"] = String(ts); 
  }
  
  if (sendLive) { 
    JsonArray live = doc.createNestedArray("live_data"); 
    for(int i=0; i<60; i++) live.add((int)live_history[i]); 
  }
  
  String js; serializeJson(doc, js); ws.textAll(js);
}

void onWsEvent(AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType t, void *a, uint8_t *d, size_t l) {
  if (t == WS_EVT_DATA) { 
    StaticJsonDocument<256> doc; 
    if (deserializeJson(doc, d, l)) return;
    
    if (doc.containsKey("mode")) tryb_auto = (doc["mode"] == "auto");
    if (doc.containsKey("pwm") && !tryb_auto) {
        int pwm_val = doc["pwm"]; pwm_val = constrain(pwm_val, 0, 100); aktualne_pwm = (pwm_val * 1023) / 100;
    }
    if (doc.containsKey("cmd")) { 
      String cmd = doc["cmd"]; 
      if (cmd == "reboot") { delay(100); ESP.restart(); }
      else if (cmd == "reset_wifi") { wm.resetSettings(); delay(100); ESP.restart(); }
      else if (cmd == "reset_kwh") {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            total_export_kwh = 0.0; total_import_kwh = 0.0; today_export_kwh = 0.0; today_import_kwh = 0.0;
            xSemaphoreGive(dataMutex);
        }
        nvm.putDouble("exp_kwh", 0.0); nvm.putDouble("imp_kwh", 0.0);
      }
    }
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n\nSTART SYSTEMU MSAG v1.18 PRO");
  
  spiMutex = xSemaphoreCreateMutex();
  dataMutex = xSemaphoreCreateMutex();
  
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
  if (queue_size > 0 && queue_size <= 24) { nvm.getBytes("queue", syncQueue, sizeof(PendingRecord) * queue_size); } 
  else { queue_size = 0; }
  
  // ====================================================================
  // INICJALIZACJA ORAZ FIX "AMERYKAŃSKIEJ BIBLIOTEKI"
  // ====================================================================
  pinMode(PIN_SPI_CS, OUTPUT);
  digitalWrite(PIN_SPI_CS, HIGH);
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS);
  delay(100);

  Serial.println("[INIT] Uruchamiam bibliotekę ATM90E32...");
  licznik_atm.begin(PIN_SPI_CS, 50, 0, 33308, 17128, 17016, 17062);
  delay(100);

  // UWAGA: Funkcja nadpiszRejestrATM zadeklarowana wyżej po chamsku wymusi zmianę w sprzęcie.
  // Ustawia rejestr MMode0 na 0x0087 (włącza wszystkie 3 fazy do sumatora energii!)
  nadpiszRejestrATM(0x7F, 0x55AA); // CfgRegAccEn - Otwarcie konfiguracji krzemu
  nadpiszRejestrATM(0x33, 0x0087); // MMode0: Bity EnPA, EnPB i EnPC na 1 (Sumuj wszystko!)
  nadpiszRejestrATM(0x00, 0x0001); // MeterEn: Start zliczania
  Serial.println("[INIT] Zakończono wymuszanie układu 3-fazowego (Patch MMode0)");
  // ====================================================================

  LittleFS.begin(true);
  for(int i = 0; i < 60; i++) live_history[i] = 0.0;

  WiFi.mode(WIFI_STA); wm.setHostname(HOSTNAME); wm.setConfigPortalBlocking(false);
  if (wm.getWiFiSSID() != "") { WiFi.begin(); wifi_state = WIFI_CONNECTING; wifi_state_timer = millis(); } 
  else { wm.startConfigPortal("MSAG-Konfiguracja"); ap_is_running = true; wifi_state = WIFI_AP_MODE; }

  if (MDNS.begin(HOSTNAME)) { MDNS.addService("http", "tcp", 80); }
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");
  ws.onEvent(onWsEvent); server.addHandler(&ws); 
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html"); 
  ElegantOTA.begin(&server); server.begin(); 
  
  // Zostawiamy task czytający moce na drugim rdzeniu
  xTaskCreate(ControlTask, "Control_Task", 4096, NULL, 2, NULL);
}

// --- LOOP ---
void loop() {
  obslugaWiFi(); 
  ws.cleanupClients(); ElegantOTA.loop();

  if (millis() - last_led_update >= 50) { last_led_update = millis(); aktualizujStanIKolory(); }

  static unsigned long rst_start = 0;
  if (digitalRead(PIN_RST_IN) == LOW) {
      if (rst_start == 0) rst_start = millis();
      if (millis() - rst_start > 5000) { wm.resetSettings(); ESP.restart(); }
  } else { rst_start = 0; }

  ssr_v = (analogRead(PIN_SSR_SENSE) * 3.3 / 4095.0) * 3.0;

  // ====================================================================
  // CZYSTY LOG SERYJNY (1 HZ) Z POKAZYWANIEM ZLICZONEJ ENERGII
  // ====================================================================
  static unsigned long last_serial_log = 0;
  if (millis() - last_serial_log >= 1000) {
      last_serial_log = millis();
      
      float v1, v2, v3, a1, a2, a3, phi1, phi2, phi3;
      double copy_today_imp = 0.0, copy_today_exp = 0.0;
      
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          v1 = phase_voltage[0]; a1 = phase_current[0]; phi1 = phase_angle[0];
          v2 = phase_voltage[1]; a2 = phase_current[1]; phi2 = phase_angle[1];
          v3 = phase_voltage[2]; a3 = phase_current[2]; phi3 = phase_angle[2];
          copy_today_imp = today_import_kwh;
          copy_today_exp = today_export_kwh;
          xSemaphoreGive(dataMutex);
          
          Serial.printf("LOG [%s] L1:%.1fV %.2fA %.1f° | L2:%.1fV %.2fA %.1f° | L3:%.1fV %.2fA %.1f° | P_total:%.1fW | PWM:%d%% | Imp: %.4f kWh | Exp: %.4f kWh\n", 
              getUptime().c_str(), v1, a1, phi1, v2, a2, phi2, v3, a3, phi3, p_total, (aktualne_pwm*100)/1023, copy_today_imp, copy_today_exp);
      }
  }

  // ====================================================================
  // OBSŁUGA CZASU, LICZNIKÓW, ODCZYTU ENERGII (CO 10 S) I KOLEJKOWANIA
  // ====================================================================
  if (millis() - last_ws_update >= 1000) { 
      last_ws_update = millis();
      obliczMocGrzalki();
      
      // ODCZYT ENERGII CO 10 SEKUND
      static unsigned long last_energy_read = 0;
      if (millis() - last_energy_read >= 10000) {
          last_energy_read = millis();
          
          double re = 0.0, ri = 0.0;
          if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
              // Układ ATM90E32 został poprawnie zainicjowany w Setup, więc teraz funkcje biblioteki
              // zczytają realne impulsy energii z rejestrów i zwrócą gotowe ułamki kWh
              re = licznik_atm.GetExportEnergy(); 
              ri = licznik_atm.GetImportEnergy();
              xSemaphoreGive(spiMutex);
          }
          
          if (re > 0.0 || ri > 0.0) {
              if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                  total_export_kwh += re; today_export_kwh += re; 
                  total_import_kwh += ri; today_import_kwh += ri; 
                  xSemaphoreGive(dataMutex);
              }
          }
      }
      
      struct tm ti; 
      if (getLocalTime(&ti) && ti.tm_year > 123) {
          static int last_sync_min = -1;
          if (ti.tm_min != last_sync_min) {
              last_sync_min = ti.tm_min;
              
              if (ti.tm_hour == 0 && ti.tm_min == 0) { 
                  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                      today_export_kwh = 0; today_import_kwh = 0; xSemaphoreGive(dataMutex);
                  }
              }
              
              bool add_to_q = false; bool force_z = false;
              if (ti.tm_hour == 0 && ti.tm_min == 5) { add_to_q = true; force_z = true; }
              else if (ti.tm_min == 0 || ti.tm_min == 30) { add_to_q = true; force_z = false; }
              else if (ti.tm_hour == 23 && ti.tm_min == 59) { add_to_q = true; force_z = false; }
              
              if (add_to_q) {
                  time_t now; time(&now);
                  double exp_copy, imp_copy;
                  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                      exp_copy = today_export_kwh; imp_copy = today_import_kwh; xSemaphoreGive(dataMutex);
                      dodajDoKolejki((uint32_t)now, exp_copy, imp_copy, force_z);
                  }
              }
              
              static int last_saved_hour = -1;
              if (ti.tm_hour != last_saved_hour) {
                  last_saved_hour = ti.tm_hour;
                  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                      nvm.putDouble("exp_kwh", total_export_kwh); nvm.putDouble("imp_kwh", total_import_kwh);
                      xSemaphoreGive(dataMutex);
                  }
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

  if (queue_size > 0 && millis() - last_google_try >= 15000) {
    last_google_try = millis(); rozladujKolejkeDoGoogle();
  }
}