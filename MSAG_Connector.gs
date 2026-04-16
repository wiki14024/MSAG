// ====================================================================
// PROJEKT: MSAG v1.0 - Skrypt Google Apps Script (Backend)
// WERSJA: Zoptymalizowana dla buforowania offline przez ESP32
// ====================================================================

function doGet(e) {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const sheet = ss.getActiveSheet();

  // 1. OBSŁUGA STRONY WWW
  if (e && e.parameter && e.parameter.action === "get_data") {
    const data = sheet.getDataRange().getValues();
    return ContentService.createTextOutput(JSON.stringify(data)).setMimeType(ContentService.MimeType.JSON);
  }

  // 2. OBSŁUGA ESP32
  const expRaw = (e && e.parameter && e.parameter.export) || "0";
  const impRaw = (e && e.parameter && e.parameter.import) || "0";
  const reqTime = (e && e.parameter && e.parameter.time) || null;
  const pvActive = (e && e.parameter && e.parameter.pv_active) || "1";
  
  let pvEnergy = 0;
  if (pvActive === "1") {
    pvEnergy = pobierzProdukcje();
  }
  
  let formattedDate = reqTime;
  if (!formattedDate) {
    const now = new Date();
    formattedDate = Utilities.formatDate(now, "Europe/Warsaw", "dd.MM.yyyy HH:mm");
  }

  const toNum = (val) => {
    if (val === null || val === undefined) return 0;
    let n = parseFloat(val.toString().replace(",", "."));
    return isNaN(n) ? 0 : n;
  };

  sheet.appendRow([
    formattedDate, 
    toNum(expRaw), 
    toNum(impRaw), 
    toNum(pvEnergy)
  ]);

  return ContentService.createTextOutput("OK. Zapisano dla: " + formattedDate)
    .setMimeType(ContentService.MimeType.TEXT);
}

function pobierzProdukcje() {
  // Pobieramy dane logowania z bezpiecznego magazynu
  const scriptProperties = PropertiesService.getScriptProperties();
  const HP_USER = scriptProperties.getProperty('HP_USER');
  const HP_PASS = scriptProperties.getProperty('HP_PASS');
  
  if (!HP_USER || !HP_PASS) {
    console.error("Brak skonfigurowanych właściwości HP_USER lub HP_PASS");
    return 0;
  }

  const domain = "https://www.hyponportal.com";
  try {
    const loginUrl = domain + "/signin";
    const loginPayload = { "account": HP_USER, "password": HP_PASS, "rememberMe": false };
    const commonHeaders = {
      "Accept": "application/json, text/plain, */*",
      "User-Agent": "Mozilla/5.0",
      "Origin": domain,
      "Content-Type": "application/json;charset=UTF-8"
    };

    const loginResp = UrlFetchApp.fetch(loginUrl, {
      "method": "post", 
      "headers": { ...commonHeaders, "Referer": domain + "/signin" },
      "payload": JSON.stringify(loginPayload), 
      "muteHttpExceptions": true
    });
    
    if (loginResp.getResponseCode() !== 200) {
      console.error(`Błąd logowania: ${loginResp.getResponseCode()}`);
      return 0;
    }

    const allHeaders = loginResp.getAllHeaders();
    let cookieHeader = "";
    if (allHeaders['Set-Cookie']) {
      const cookies = Array.isArray(allHeaders['Set-Cookie']) ? allHeaders['Set-Cookie'] : [allHeaders['Set-Cookie']];
      cookieHeader = cookies.map(c => c.split(';')[0]).join('; ');
    }

    const stationsUrl = domain + "/stations?status=-1&pageSize=10&pageNumber=1&sortCol=status&order=1";
    const resp = UrlFetchApp.fetch(stationsUrl, {
      "method": "get",
      "headers": { ...commonHeaders, "Cookie": cookieHeader, "Referer": domain + "/pilotview" },
      "muteHttpExceptions": true
    });
    
    const json = JSON.parse(resp.getContentText());
    if (json.data && json.data.infos && json.data.infos.length > 0) {
      const stacja = json.data.infos[0];
      if (stacja.stationRealtimeVO) {
        return stacja.stationRealtimeVO.eToday; 
      }
    }
    return 0;
  } catch (err) {
    console.error(`Wyjątek w pobierzProdukcje: ${err.toString()}`);
    return 0;
  }
}

function testujSkrypt() {
  const wynik = pobierzProdukcje();
  Logger.log("Dzisiejsza produkcja z falownika to: " + wynik);
}