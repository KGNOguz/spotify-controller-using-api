#define IRQ 10

#include "WiFi.h"
#include "secrets.h"
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

#include "Free_Fonts.h"
#include "icons.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
WiFiClientSecure client;
HTTPClient https;

#include <ArduinoJson.h>

#include "Preferences.h"
Preferences prefs;

#include "SPI.h"
#include "TFT_eSPI.h"

TFT_eSPI tft = TFT_eSPI();

bool checkSecret;
String deviceID;
String deviceSecret;
int pairingCode;

//Colors-----------------------------------------------
const uint16_t clrBackground = tft.color565(10, 10, 15);
const uint16_t clrPrimary = tft.color565(227, 227, 227);
const uint16_t clrPairingPrimary = tft.color565(30, 215, 96);
const uint16_t clrGrey = tft.color565(100, 100, 100);

//Milisecond Values For millis() Timing
unsigned long currentMillis;
unsigned long lastPairPollingMillis;
unsigned long valuePairPolling = 2000;
unsigned long lastGetCurrentTrackMillis;
unsigned long valueGetCurrentTrack = 4000;
unsigned long lastStatusBarRefreshMillis;
unsigned long valueStatusBarRefresh = 500;

//Spotify Verileri-------------------------------------

//---CurrentTrack---
bool is_playing;
String device_name;
String device_type;
int volume_percent;
unsigned long progress_ms;
String current_track_id;
String last_track_id;  //For Checking
bool isNewTrack;
int statusBarLength;

//---TrackDetails---
String track_name;
String artist_name;
String album_name;
unsigned long duration_ms;

//---TouchScreen---
uint16_t touchX;
uint16_t touchY;
bool isTouch;
uint16_t calData[5];
bool stateNext;
bool stateMiddle;
bool statePrev;

void setup() {

  pinMode(IRQ, INPUT);

  Serial.begin(115200);

  //eraseMemory();

  initTFT();

  checkTouchCalibrate();

  connectWiFi();
  client.setInsecure();  // test aşamasında güvenliksiz veri için

  checkDeviceID();
  checkDeviceSecret();

  if (checkSecret == false) {
    getPairingCode();
    displayPairingCode();
  }
}

void loop() {

  checkIsTouch();

  currentMillis = millis();

  if (checkSecret != false) {

    if (currentMillis - lastStatusBarRefreshMillis > valueStatusBarRefresh) {  //Status Bar Güncelleme
      lastStatusBarRefreshMillis = currentMillis;
      refreshStatusBarLength();
    }

    if (currentMillis - lastGetCurrentTrackMillis > valueGetCurrentTrack) {  //Geçerli İçerik Bilgisi Alma
      lastGetCurrentTrackMillis = currentMillis;
      getCurrentTrackDetails();
    }

    if (isNewTrack == 1) {
      delay(100);
      getTrackDetails();
      eraseAndRenderNewTrackDetails();
      delay(10);
      isNewTrack = 0;
    }

    checkPrevButtonState();
    checkMiddleButtonState();
    checkNextButtonState();

  } else {

    if (currentMillis - lastPairPollingMillis > valuePairPolling) {  // Device Secret Polling
      lastPairPollingMillis = currentMillis;
      getDeviceSecret();
    }
  }

  delay(50);
}

void checkDeviceSecret() {

  prefs.begin("memory", false);

  if (prefs.isKey("deviceSecret") == false || prefs.getString("deviceSecret").length() < 36) {
    checkSecret = 0;
    Serial.println("Device Secret Bulunmuyor/Yanlış");
  } else {
    checkSecret = 1;
    deviceSecret = prefs.getString("deviceSecret");
    Serial.println("Device Secret Kullanılabilir: " + deviceSecret);
  }

  prefs.end();
}

void checkDeviceID() {

  prefs.begin("memory", false);

  if (prefs.isKey("deviceID") == false) {
    prefs.putString("deviceID", randomDeviceID());
    Serial.println("Cihaz ID Bulunmuyor: Oluşturuldu");
  } else {
    Serial.println("Cihaz ID Bulundu:" + prefs.getString("deviceID"));
  }

  deviceID = prefs.getString("deviceID");

  prefs.end();
}

String randomDeviceID() {

  String temp = WiFi.macAddress();
  temp.replace(":", "");

  return temp;
}

void initTFT() {

  tft.init();
  tft.fillScreen(clrBackground);
  tft.setTextColor(clrPrimary, clrBackground);

  Serial.println("TFT Başlatıldı");
}

void connectWiFi() {
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Connecting to WiFi", 120, 160);

  // Tam sıfırlama
  WiFi.setSleep(false);
  WiFi.mode(WIFI_OFF);   // Önce tamamen kapat
  delay(500);
  WiFi.mode(WIFI_STA);   // Sonra STA moduna al
  delay(500);

  // Ağ taraması (opsiyonel)
  int n = WiFi.scanNetworks();
  Serial.println("Bulunan aglar:");
  for (int i = 0; i < n; ++i) Serial.println(WiFi.SSID(i));
  WiFi.scanDelete();
  delay(500);

  WiFi.begin(ssid, password);

  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED) {
    tft.fillRect(0, 150, 240, 20, clrBackground);
    tft.drawString("Baglaniyor... " + String(retryCount), 120, 160);
    Serial.print("Status: ");
    Serial.print(WiFi.status());
    Serial.print(" | RSSI: ");
    Serial.println(WiFi.RSSI());
    delay(750);
    retryCount++;

    if (retryCount > 20) {
      Serial.println("Yeniden deneniyor...");
      WiFi.mode(WIFI_OFF);  // ✅ disconnect() değil, tamamen kapat
      delay(1000);
      WiFi.mode(WIFI_STA);
      delay(500);
      WiFi.begin(ssid, password);
      retryCount = 0;
    }
  }

  Serial.println("Baglandi! IP: " + WiFi.localIP().toString());
  tft.fillRect(0, 150, 240, 20, clrBackground);
  tft.drawString("Baglandi!", 120, 160);
  delay(500);
  tft.fillRect(0, 150, 240, 20, clrBackground);
}


void getPairingCode() {

  if (https.begin(client, "start-pair endpoint here") == true) {

    https.setTimeout(20000);

    https.addHeader("Content-Type", "application/json");

    DynamicJsonDocument req(256);
    req["device_id"] = deviceID;
    String body;
    serializeJson(req, body);
    int httpCode = https.POST(body);
    Serial.println("HTTP Code: " + String(httpCode));

    if (httpCode == 200) {

      String payload = https.getString();
      DynamicJsonDocument res(256);

      DeserializationError error = deserializeJson(res, payload);
      if (error) {
        Serial.println("JSON parse hatası");
        return;
      }

      pairingCode = res["pairing_code"].as<int>();

      prefs.begin("memory", false);
      prefs.putInt("pairingCode", pairingCode);

      prefs.end();

      Serial.println("Pairing Code Alma Başarılı: " + String(pairingCode));
    }

    https.end();
  }
}

void displayPairingCode() {

  tft.fillScreen(clrBackground);

  tft.setTextDatum(MC_DATUM);

  tft.setTextSize(1);
  tft.drawString("Your Pairing Key:", 120, 130);
  tft.drawString("frontend link here", 120, 190);

  tft.setTextSize(3);
  tft.drawString(String(pairingCode), 120, 160);
}

void displayDevicePaired() {

  tft.fillScreen(clrBackground);

  tft.fillScreen(clrBackground);

  tft.setTextDatum(MC_DATUM);

  tft.setTextSize(1);
  tft.drawString("Your Device Is Paired Successfully", 120, 150);
  tft.drawString("You Will Be Redirected To Menu", 120, 170);

  delay(1000);

  tft.fillScreen(clrBackground);
}

void getDeviceSecret() {

  if (https.begin(client, "poll-pairing endpoint here") == true) {

    https.setTimeout(20000);

    https.addHeader("Content-Type", "application/json");

    DynamicJsonDocument req(256);
    req["device_id"] = deviceID;
    String body;
    serializeJson(req, body);

    int httpCode = https.POST(body);
    Serial.println("HTTP Code: " + String(httpCode));

    if (httpCode == 200) {

      String payload = https.getString();
      DynamicJsonDocument res(256);

      DeserializationError error = deserializeJson(res, payload);
      if (error) {
        Serial.println("JSON parse hatası");
        return;
      }

      String tempSecret = res["device_secret"];
      bool checkPaired = res["paired"];

      if (checkPaired != true) {
        Serial.println("Veritabanında Pair Yapılmadı: Yeniden Deneniyor");
      } else if (tempSecret.length() != 36) {
        Serial.println("UUID Bulunmuyor Veya Geçersiz Formatta");
      } else {
        deviceSecret = tempSecret;
        prefs.begin("memory", false);
        prefs.putString("deviceSecret", deviceSecret);
        prefs.end();
        Serial.println("Device Secret Kodu Alındı: " + deviceSecret);
        checkSecret = true;

        displayDevicePaired();
      }
    }

    https.end();
  }
}

void getCurrentTrackDetails() {

  if (https.begin(client, "get-current-track-details endpoint here") == true) {

    https.setTimeout(20000);

    addHeaders();

    int httpCode = https.GET();
    Serial.println("HTTP Code: " + String(httpCode));

    if (httpCode == 200) {

      String payload = https.getString();
      DynamicJsonDocument res(512);

      DeserializationError error = deserializeJson(res, payload);
      if (error) {
        Serial.println("JSON parse hatası");
        return;
      }

      if (res["success"].as<bool>() == true) {
        is_playing = res["is_playing"].as<bool>();
        device_name = res["device_name"].as<String>();
        device_type = res["device_type"].as<String>();
        volume_percent = res["volume_percent"].as<int>();
        progress_ms = res["progress_ms"].as<long>();
        current_track_id = res["track_id"].as<String>();
        Serial.println(String(is_playing) + device_name + device_type + String(volume_percent) + String(progress_ms) + current_track_id);

        if (current_track_id != "null" && current_track_id != last_track_id) {
          isNewTrack = 1;
          last_track_id = current_track_id;
          Serial.println("Şarkı Değişimi Algılandı: " + current_track_id);
        }
      }

    } else {
      Serial.println("Şarkı Çalınmıyor Veya Hata Oluştu");
    }

    https.end();
  }
}

void getTrackDetails() {

  if (https.begin(client, "get-track-details endpoint here") == true) {

    https.setTimeout(20000);

    addHeaders();

    int httpCode = https.GET();
    Serial.println("HTTP Code: " + String(httpCode));

    if (httpCode == 200) {

      String payload = https.getString();
      DynamicJsonDocument res(512);

      DeserializationError error = deserializeJson(res, payload);
      if (error) {
        Serial.println("JSON parse hatası");
        return;
      }

      if (res["success"].as<bool>() == 1) {
        track_name = res["track_name"].as<String>();
        artist_name = res["artist_name"].as<String>();
        album_name = res["album_name"].as<String>();
        duration_ms = res["duration_ms"].as<long>();
        Serial.println(track_name + artist_name + album_name + String(duration_ms));
      }

    } else {
      Serial.println("Şarkı Çalınmıyor Veya Hata Oluştu");
    }

    https.end();
  }
}



void eraseMemory() {

  prefs.begin("memory", false);
  prefs.clear();
  prefs.end();
  Serial.println("Hafiza SIFIRLANDI");
}

void addHeaders() {

  https.addHeader("Content-Type", "application/json");
  https.addHeader("x-device-id", deviceID);
  https.addHeader("x-device-secret", deviceSecret);
}

void refreshStatusBarLength() {
  progress_ms = progress_ms + 500;

  if (duration_ms > 0 && progress_ms <= duration_ms) {
    // Önce float'a çevir ki çarpma işleminde taşmasın!
    float barWidth = 180.0;
    float param = ((float)progress_ms / (float)duration_ms) * barWidth;

    statusBarLength = round(param);
    if (statusBarLength > 180) statusBarLength = 180;

    tft.fillRoundRect(30, 205, statusBarLength, 10, 4, clrPairingPrimary);
  }
}


void renderTrackDetails() {

  tft.setTextDatum(TC_DATUM);

  tft.setTextColor(clrPrimary);
  tft.setFreeFont(&FreeSansBoldOblique12pt7b);
  tft.drawString(track_name, 120, 215);

  tft.setTextColor(clrGrey);
  tft.setFreeFont(&FreeSansBoldOblique9pt7b);
  tft.drawString(artist_name, 120, 240);

  //Status Bar
  tft.fillRoundRect(30, 205, 180, 10, 4, clrPrimary);

  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(NULL);

  tft.drawString("Playing On: " + device_name, 6, 6);
}

void eraseAndRenderNewTrackDetails() {

  tft.fillRect(0, 218, 240, 52, clrBackground);
  tft.fillRect(0, 3, 15, 240, clrBackground);

  renderTrackDetails();
  delay(100);
  getAlbumCover();
}

void getAlbumCover() {

  if (https.begin(client, "get-album-cover endpoint here")) {
    https.setTimeout(20000);
    addHeaders();

    int httpCode = https.GET();
    Serial.println("HTTP Code: " + String(httpCode));

    if (httpCode == 200) {

      int startX = 30;
      int startY = 20;

      tft.setAddrWindow(startX, startY, 180, 180);
      WiFiClient* stream = https.getStreamPtr();

      // Backend'in gönderdiği Content-Length'i alalım (Beklenen 64800)
      int len = https.getSize();

      uint8_t buffer[512];
      int bytesRead = 0;
      int timeout = 0;  // Donmayı engelleyecek sayaç

      // len > 0 ise tam byte sayısı kadar bekle, değilse buffer bitene kadar dön
      while (https.connected() && (len > 0 ? (bytesRead < len) : true)) {
        size_t size = stream->available();

        if (size) {
          int c = stream->readBytes(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
          tft.pushColors(buffer, c);
          bytesRead += c;
          timeout = 0;  // Veri geldikçe hata sayacını sıfırla
        } else {
          delay(10);
          timeout++;
          if (timeout > 500) {  // 5 saniye boyunca tek byte gelmezse hata ver çık
            Serial.println("Kapak indirmesi zaman asimina ugradi!");
            break;
          }
        }
      }

      Serial.println("Album kapagi yuklendi! Toplam byte: " + String(bytesRead));
    } else if (httpCode == 404) {
      Serial.println("Sarki calinmiyor veya kapak yok.");
    } else {
      Serial.println("Kapak yuklenemedi. HTTP Kod: " + String(httpCode));
    }

    https.end();
  }
}

void checkTouchCalibrate() {

  prefs.begin("memory", false);

  if (prefs.isKey("touchCalData") == false) {
    touchCalibrate();
    prefs.putBytes("touchCalData", calData, sizeof(calData));
  } else {
    size_t len = prefs.getBytesLength("touchCalData");

    if (len == sizeof(calData)) {
      prefs.getBytes("touchCalData", calData, sizeof(calData));
    } else {
      touchCalibrate();
      prefs.putBytes("touchCalData", calData, sizeof(calData));
    }
  }

  tft.setTouch(calData);
  prefs.end();

  tft.fillScreen(clrBackground);
}

void touchCalibrate() {

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(clrPrimary, clrBackground);
  tft.drawString("Calibration Started", 120, 160);
  Serial.println("Kalibrasyon Başladı");

  delay(2000);

  tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 15);

  Serial.println("Kalibrasyon tamam:");
  for (int i = 0; i < 5; i++) {
    Serial.print(calData[i]);
    Serial.print(", ");
  }
}

void checkNextButtonState() {

  bool lastState = stateNext;

  if (isTouch) {
    if (touchX >= 172 && touchX <= 240 && touchY >= 255 && touchY <= 320) {
      stateNext = true;
    } else {
      stateNext = false;
    }
  } else {
    stateNext = false;
  }

  if (lastState != stateNext) {
    tft.fillRect(186, 269, 40, 42, clrBackground);
    if (stateNext == true) {
      tft.drawBitmap(187, 270, bitmapNextHover, 38, 40, clrPrimary);
      endpointNext();
      delay(500);
      renderTrackDetails();
      getAlbumCover();
    } else {
      tft.drawBitmap(187, 270, bitmapNext, 38, 40, clrPrimary);
    }
  }
}

void checkPrevButtonState() {

  bool lastState = statePrev;

  if (isTouch) {
    if (touchX >= 0 && touchX <= 68 && touchY >= 255 && touchY <= 320) {
      statePrev = true;
    } else {
      statePrev = false;
    }
  } else {
    statePrev = false;
  }

  if (lastState != statePrev) {
    tft.fillRect(14, 269, 40, 42, clrBackground);
    if (statePrev == true) {
      tft.drawBitmap(15, 270, bitmapPrevHover, 38, 40, clrPrimary);
      endpointPrev();
      delay(500);
      renderTrackDetails();
      getAlbumCover();
    } else {
      tft.drawBitmap(15, 270, bitmapPrev, 38, 40, clrPrimary);
    }
  }
}

void checkMiddleButtonState() {

  bool lastState = stateMiddle;

  if (isTouch) {
    if (touchX >= 86 && touchX <= 164 && touchY >= 255 && touchY <= 320) {
      stateMiddle = true;
    } else {
      stateMiddle = false;
    }
  } else {
    stateMiddle = false;
  }

  if (lastState != stateMiddle) {
    tft.fillRect(100, 269, 40, 42, clrBackground);

    if (stateMiddle == true) {
      // basılı — mevcut duruma göre hover
      if (is_playing == 1) {
        tft.drawBitmap(101, 270, bitmapPauseHover, 38, 40, clrPrimary);
        endpointPause();
        is_playing = 0; // state'i hemen çevir
      } else {
        tft.drawBitmap(101, 270, bitmapPlayHover, 38, 40, clrPrimary);
        endpointPlay();
        is_playing = 1; // state'i hemen çevir
      }
      delay(500);

    } else {
      // bırakıldı — güncel duruma göre bitmap
      if (is_playing == 1) {
        tft.drawBitmap(101, 270, bitmapPause, 38, 40, clrPrimary);
      } else {
        tft.drawBitmap(101, 270, bitmapPlay, 38, 40, clrPrimary);
      }
    }
  }
}

void endpointNext() {

  if (https.begin(client, "controls/next endpoint here") == true) {

    https.setTimeout(20000);

    addHeaders();

    int httpCode = https.POST("");

    Serial.println("HTTP Code: " + String(httpCode));

    if (httpCode == 200) {

      String payload = https.getString();
      DynamicJsonDocument res(128);

      DeserializationError error = deserializeJson(res, payload);
      if (error) {
        Serial.println("JSON parse hatası");
        return;
      }

      if (res["success"].as<bool>() == 1) {
        Serial.println("Şarkı Atlandı");
      }

    } else {
      Serial.println("NEXT Fonksiyonunda Hata Oluştu");
    }

    https.end();
  }
}

void endpointPrev() {

  if (https.begin(client, "controls/previous endpoint here") == true) {

    https.setTimeout(20000);

    addHeaders();

    int httpCode = https.POST("");

    Serial.println("HTTP Code: " + String(httpCode));

    if (httpCode == 200) {

      String payload = https.getString();
      DynamicJsonDocument res(128);

      DeserializationError error = deserializeJson(res, payload);
      if (error) {
        Serial.println("JSON parse hatası");
        return;
      }

      if (res["success"].as<bool>() == 1) {
        Serial.println("Şarkı Geri Atlandı");
      }

    } else {
      Serial.println("PREV Fonksiyonunda Hata Oluştu");
    }

    https.end();
  }
}

void endpointPlay() {

  if (https.begin(client, "controls/play endpoint here") == true) {

    https.setTimeout(20000);

    addHeaders();

    int httpCode = https.PUT("");

    Serial.println("HTTP Code: " + String(httpCode));

    if (httpCode == 200) {

      String payload = https.getString();
      DynamicJsonDocument res(128);

      DeserializationError error = deserializeJson(res, payload);
      if (error) {
        Serial.println("JSON parse hatası");
        return;
      }

      if (res["success"].as<bool>() == 1) {
        Serial.println("Şarkı Başlatıldı");
      }

    } else {
      Serial.println("PLAY Fonksiyonunda Hata Oluştu");
    }

    https.end();
  }
}

void endpointPause() {

  if (https.begin(client, "controls/pause endpoint here") == true) {

    https.setTimeout(20000);

    addHeaders();

    int httpCode = https.PUT("");

    Serial.println("HTTP Code: " + String(httpCode));

    if (httpCode == 200) {

      String payload = https.getString();
      DynamicJsonDocument res(128);

      DeserializationError error = deserializeJson(res, payload);
      if (error) {
        Serial.println("JSON parse hatası");
        return;
      }

      if (res["success"].as<bool>() == 1) {
        Serial.println("Şarkı Durduruldu");
      }

    } else {
      Serial.println("PAUSE Fonksiyonunda Hata Oluştu");
    }

    https.end();
  }
}

void checkIsTouch() {

  touchX = -1;
  touchY = -1;
  isTouch = 0;

  if ( tft.getTouch(&touchX, &touchY) ) {
    isTouch = 1;
    Serial.println("X: " + String(touchX) + "Y: " + String(touchY));
  }

}