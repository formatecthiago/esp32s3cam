#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include "esp_camera.h"

// Pinos da camera na Freenove ESP32-S3 WROOM
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

struct WifiCred {
  const char* ssid;
  const char* pass;
};

// Redes Wi-Fi pre-definidas (tenta em sequencia)
static const WifiCred WIFI_LIST[] = {
  {"FORMATEC 2G", "1234cinco@#"},
  {"NewHome",     "1234cinco@#"},
  {"REALMESPEED", "123quatro@#"}
};
static const size_t WIFI_COUNT = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);

static const char* FIREBASE_STREAM_URL = "https://portaria-cam-default-rtdb.firebaseio.com/camera/stream";

// Parametros fixos para o teste limpo (320x240 RGB565 em blocos de 16 linhas)
static const framesize_t FRAME_SIZE = FRAMESIZE_QVGA;
static const int FRAME_W = 320;
static const int FRAME_H = 240;
static const int ROWS_PER_CHUNK = 16;
static const int TARGET_FPS = 1; // 1 = 1 FPS (~1000 ms). Ajuste para 1..4 conforme o teste.

static bool camOk = false;
static unsigned long frameSeq = 0;
static unsigned long lastSendMs = 0;

static WiFiClientSecure tlsClient;
static HTTPClient httpClient;
static bool httpPronto = false;

static bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.frame_size   = FRAME_SIZE;
  config.pixel_format = PIXFORMAT_RGB565;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_DRAM;
  config.fb_count     = 1;

  return (esp_camera_init(&config) == ESP_OK);
}

static bool conectarWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  for (size_t i = 0; i < WIFI_COUNT; i++) {
    WiFi.disconnect(true);
    delay(150);
    WiFi.begin(WIFI_LIST[i].ssid, WIFI_LIST[i].pass);

    int tentativas = 0;
    while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
      delay(400);
      tentativas++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      tlsClient.setInsecure();
      tlsClient.setTimeout(7);
      httpClient.setReuse(true);
      httpClient.setTimeout(7000);
      httpPronto = true;
      Serial.print("WIFI_OK: ");
      Serial.println(WIFI_LIST[i].ssid);
      return true;
    }
  }

  Serial.println("WIFI_FAIL");
  return false;
}

static bool putJsonReutilizado(const String& url, const String& body) {
  if (!httpPronto) return false;
  if (!httpClient.begin(tlsClient, url)) {
    httpClient.end();
    return false;
  }
  httpClient.addHeader("Content-Type", "application/json");
  int code = httpClient.PUT(body);
  return (code > 0 && code < 300);
}

static bool enviarBloco(int idx, int r0, int rc, const uint8_t* buf) {
  size_t rowBytes = (size_t)FRAME_W * 2;
  size_t chunkBytes = (size_t)rc * rowBytes;
  const uint8_t* ptr = buf + ((size_t)r0 * rowBytes);

  String b64 = base64::encode(ptr, chunkBytes);
  String json;
  json.reserve(b64.length() + 96);
  json += "{\"c\":";
  json += String(idx);
  json += ",\"r0\":";
  json += String(r0);
  json += ",\"rc\":";
  json += String(rc);
  json += ",\"d\":\"";
  json += b64;
  json += "\"}";
  b64 = "";

  String url = String(FIREBASE_STREAM_URL) + "/blocos/" + String(idx) + ".json";
  bool ok = putJsonReutilizado(url, json);
  json = "";
  return ok;
}

static void enviarFrameLimpo() {
  if (!camOk || WiFi.status() != WL_CONNECTED) return;

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("FB_FAIL");
    return;
  }

  size_t expected = (size_t)FRAME_W * (size_t)FRAME_H * 2;
  if (fb->len < expected) {
    esp_camera_fb_return(fb);
    Serial.println("FB_SHORT");
    return;
  }

  int totalBlocos = (FRAME_H + ROWS_PER_CHUNK - 1) / ROWS_PER_CHUNK;
  bool ok = true;

  for (int c = 0; c < totalBlocos; c++) {
    int r0 = c * ROWS_PER_CHUNK;
    int rc = min(ROWS_PER_CHUNK, FRAME_H - r0);
    if (!enviarBloco(c, r0, rc, fb->buf)) {
      ok = false;
      Serial.print("BLOCO_FAIL ");
      Serial.println(c);
      break;
    }
    yield();
  }

  esp_camera_fb_return(fb);

  if (ok) {
    frameSeq++;
    String metaUrl = String(FIREBASE_STREAM_URL) + "/meta.json";
    String metaJson = "{\"formato\":\"rgb565\",\"w\":" + String(FRAME_W) +
                      ",\"h\":" + String(FRAME_H) +
                      ",\"blocos\":" + String(totalBlocos) +
                      ",\"frame\":" + String(frameSeq) +
                      ",\"ts\":" + String(millis()) + "}";
    if (!putJsonReutilizado(metaUrl, metaJson)) {
      Serial.println("META_FAIL");
    } else {
      Serial.print("FRAME_OK #");
      Serial.println(frameSeq);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);

  camOk = initCamera();
  Serial.println(camOk ? "CAM_OK" : "CAM_FAIL");

  conectarWifi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    conectarWifi();
    delay(500);
    return;
  }

  unsigned long intervalo = (TARGET_FPS > 0) ? (1000UL / (unsigned long)TARGET_FPS) : 1000UL;
  unsigned long agora = millis();
  if (camOk && (lastSendMs == 0 || (agora - lastSendMs) >= intervalo)) {
    lastSendMs = agora;
    enviarFrameLimpo();
  }

  delay(5);
}
