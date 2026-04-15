// ====================================================================
// PROJEKT: MSAG v1.0 - Skrypt Google Apps Script (Backend)
// ====================================================================

// --- KONFIGURACJA ---
const HP_USER = "wiki14024@gmail.com";
const HP_PASS = "Wilku666"; // Twoje hasło do portalu Hypon

/**
 * Główna funkcja obsługująca zapytania GET z ESP32 i ze strony WWW
 */
function doGet(e) {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const sheet = ss.getActiveSheet();

  // ------------------------------------------------------------------
  // 1. OBSŁUGA STRONY WWW (Rysowanie wykresów)
  // ------------------------------------------------------------------
  // Jeśli zapytanie ma parametr ?action=get_data, zwracamy całą tabelę
  if (e && e.parameter && e.parameter.action === "get_data") {
    const data = sheet.getDataRange().getValues();
    return ContentService.createTextOutput(JSON.stringify(data))
      .setMimeType(ContentService.MimeType.JSON);
  }

  // ------------------------------------------------------------------
  // 2. OBSŁUGA ESP32 (Zapis nowych pomiarów co godzinę)
  // ------------------------------------------------------------------
  // Pobieranie parametrów z linku ESP32 (zabezpieczenie przed brakiem danych)
  const expRaw = (e && e.parameter && e.parameter.export) || "0";
  const impRaw = (e && e.parameter && e.parameter.import) || "0";
  
  // Pobieranie danych o produkcji bezpośrednio z serwera Hypon
  const pvEnergy = pobierzProdukcje();
  
  // Czas serwera (Google) - używamy go, bo ESP wywołuje skrypt o pełnej godzinie
  const now = new Date();
  const formattedDate = Utilities.formatDate(now, "Europe/Warsaw", "dd.MM.yyyy HH:mm");

  // Funkcja pomocnicza do zamiany tekstu na liczbę (obsługuje kropki i przecinki)
  const toNum = (val) => {
    if (val === null || val === undefined) return 0;
    let n = parseFloat(val.toString().replace(",", "."));
    return isNaN(n) ? 0 : n;
  };

  // Zapis wiersza w arkuszu: [Data i Czas, Eksport ESP, Import ESP, Produkcja PV z falownika]
  sheet.appendRow([
    formattedDate, 
    toNum(expRaw), 
    toNum(impRaw), 
    toNum(pvEnergy)
  ]);

  // Odpowiedź dla ESP (Zwracamy tekst, który ESP loguje w Monitorze Szeregowym)
  return ContentService.createTextOutput("OK. Zapisano PV: " + pvEnergy)
    .setMimeType(ContentService.MimeType.TEXT);
}

/**
 * Loguje się do Hypona i wyciąga aktualną wartość eToday
 */
function pobierzProdukcje() {
  const domain = "https://www.hyponportal.com";
  
  try {
    // KROK 1: Logowanie
    const loginUrl = domain + "/signin";
    const loginPayload = {
      "account": HP_USER,
      "password": HP_PASS,
      "rememberMe": false
    };
    
    const commonHeaders = {
      "Accept": "application/json, text/plain, */*",
      "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36",
      "Origin": domain,
      "Content-Type": "application/json;charset=UTF-8"
    };

    const loginResp = UrlFetchApp.fetch(loginUrl, {
      "method": "post",
      "headers": { ...commonHeaders, "Referer": domain + "/signin" },
      "payload": JSON.stringify(loginPayload),
      "muteHttpExceptions": true
    });
    
    if (loginResp.getResponseCode() !== 200) return "Błąd logowania (HTTP " + loginResp.getResponseCode() + ")";

    // KROK 2: Wyciąganie Ciasteczka (Session ID)
    const allHeaders = loginResp.getAllHeaders();
    let cookieHeader = "";
    if (allHeaders['Set-Cookie']) {
      const cookies = Array.isArray(allHeaders['Set-Cookie']) ? allHeaders['Set-Cookie'] : [allHeaders['Set-Cookie']];
      cookieHeader = cookies.map(c => c.split(';')[0]).join('; ');
    }

    // KROK 3: Pobieranie danych stacji
    const stationsUrl = domain + "/stations?status=-1&pageSize=10&pageNumber=1&sortCol=status&order=1";
    
    const resp = UrlFetchApp.fetch(stationsUrl, {
      "method": "get",
      "headers": { 
        ...commonHeaders, 
        "Cookie": cookieHeader, 
        "Referer": domain + "/pilotview" 
      },
      "muteHttpExceptions": true
    });
    
    const json = JSON.parse(resp.getContentText());
    
    // KROK 4: Wyciąganie eToday z głębi struktury JSON
    if (json.data && json.data.infos && json.data.infos.length > 0) {
      const stacja = json.data.infos[0];
      if (stacja.stationRealtimeVO) {
        return stacja.stationRealtimeVO.eToday; // Zwraca np. 1.20
      }
    }
    
    return 0;

  } catch (err) {
    return "Błąd: " + err.message;
  }
}

// Funkcja do szybkiego testu wewnątrz edytora (kliknij "Uruchom")
function testujSkrypt() {
  const wynik = pobierzProdukcje();
  Logger.log("Dzisiejsza produkcja z falownika to: " + wynik);
}