#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "mbedtls/base64.h"

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

// Configuracao do Firebase Realtime Database
static const char* FIREBASE_STREAM_URL = "https://portaria-cam-default-rtdb.firebaseio.com/camera/stream.json";

// Parametros de espera leve e deteccao de movimento
static const unsigned long IDLE_CHECK_INTERVAL_MS = 450;   // Espera leve sem envio quando parado
static const unsigned long ACTIVE_SEND_INTERVAL_MS = 400; // Margem para envio em VGA + Base64
static const unsigned long MOTION_HOLD_MS = 4000;         // Continua enviando por 4s apos ultimo movimento
static const float MOTION_DIFF_THRESHOLD = 0.08f;         // 8% de diferenca media entre quadros

WebServer server(80);
Preferences prefs;

bool camOk = false;
bool apMode = false;
String savedSsid = "";
String savedPass = "";

uint8_t* prevSampleBuf = nullptr;
size_t prevSampleLen = 0;
unsigned long lastCamCheckMs = 0;
unsigned long lastFirebaseSendMs = 0;
unsigned long motionUntilMs = 0;

String encodeBase64Jpeg(const uint8_t* data, size_t len) {
  size_t outLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outLen, data, len);
  if (outLen == 0) return "";

  String out;
  out.reserve(outLen + 32);
  out = "data:image/jpeg;base64,";

  size_t prefixLen = out.length();
  out.resize(prefixLen + outLen);

  size_t written = 0;
  int rc = mbedtls_base64_encode(
    reinterpret_cast<unsigned char*>(&out[prefixLen]),
    outLen,
    &written,
    data,
    len
  );
  if (rc != 0) return "";

  out.resize(prefixLen + written);
  return out;
}

bool detectMotion(camera_fb_t* fb) {
  if (!fb || !fb->buf || fb->len < 64) return false;

  // Amostragem espacada para manter baixo custo de CPU e memoria
  const size_t step = (fb->len > 4096) ? (fb->len / 1024) : 4;
  const size_t sampleCount = (fb->len + step - 1) / step;

  if (!prevSampleBuf || prevSampleLen != sampleCount) {
    free(prevSampleBuf);
    prevSampleBuf = static_cast<uint8_t*>(malloc(sampleCount));
    prevSampleLen = prevSampleBuf ? sampleCount : 0;
    if (!prevSampleBuf) return true;

    for (size_t i = 0, idx = 0; i < fb->len && idx < sampleCount; i += step, idx++) {
      prevSampleBuf[idx] = fb->buf[i];
    }
    return false;
  }

  uint64_t diffSum = 0;
  size_t compared = 0;
  for (size_t i = 0, idx = 0; i < fb->len && idx < sampleCount; i += step, idx++) {
    uint8_t cur = fb->buf[i];
    diffSum += (cur > prevSampleBuf[idx]) ? (cur - prevSampleBuf[idx]) : (prevSampleBuf[idx] - cur);
    prevSampleBuf[idx] = cur;
    compared++;
  }

  if (compared == 0) return false;
  float avgDiff = static_cast<float>(diffSum) / (static_cast<float>(compared) * 255.0f);
  return (avgDiff >= MOTION_DIFF_THRESHOLD);
}

bool sendFrameToFirebase(camera_fb_t* fb) {
  if (WiFi.status() != WL_CONNECTED || !fb || !fb->buf || fb->len == 0) {
    return false;
  }

  String base64Img = encodeBase64Jpeg(fb->buf, fb->len);
  if (base64Img.length() == 0) {
    return false;
  }

  JsonDocument doc;
  doc["imagem"] = base64Img;
  doc["timestamp"] = static_cast<uint64_t>(millis());

  String payload;
  serializeJson(doc, payload);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, FIREBASE_STREAM_URL)) {
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PUT(payload);
  http.end();

  return (httpCode >= 200 && httpCode < 300);
}

String htmlConfigPage(const String& msg = "") {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Configurar Wi-Fi Portaria CAM</title></head>";
  html += "<body style='font-family:sans-serif;background:#111;color:#fff;padding:20px;max-width:420px;margin:auto;'>";
  html += "<h2>Configurar Wi-Fi da Camera</h2>";
  if (msg.length() > 0) {
    html += "<p style='background:#222;padding:10px;border-left:4px solid #4caf50;'>" + msg + "</p>";
  }
  html += "<form action='/salvar' method='POST'>";
  html += "<label>Nome da Rede Wi-Fi (SSID):</label><br>";
  html += "<input name='ssid' value='" + savedSsid + "' required style='width:100%;padding:10px;margin:6px 0 14px;font-size:16px;'><br>";
  html += "<label>Senha do Wi-Fi:</label><br>";
  html += "<input name='pass' type='password' placeholder='Digite a senha' style='width:100%;padding:10px;margin:6px 0 18px;font-size:16px;'><br>";
  html += "<button type='submit' style='width:100%;padding:12px;font-size:16px;background:#2e7d32;color:#fff;border:none;border-radius:6px;'>Salvar e Reiniciar</button>";
  html += "</form></body></html>";
  return html;
}

void handleRoot() {
  if (apMode) {
    server.send(200, "text/html", htmlConfigPage("Conectado ao modo de configuracao. Informe o Wi-Fi abaixo."));
    return;
  }

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Portaria CAM ESP32-S3</title></head>";
  html += "<body style='font-family:sans-serif;text-align:center;background:#111;color:#fff;padding:16px;'>";
  html += "<h2>Portaria CAM ESP32-S3</h2>";
  html += "<p style='color:#9e9e9e;'>Rede: " + savedSsid + " | IP: " + WiFi.localIP().toString() + "</p>";
  html += "<p><a href='/config' style='display:inline-block;padding:10px 18px;font-size:16px;background:#333;color:#fff;text-decoration:none;border-radius:4px;'>Trocar Wi-Fi</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleConfig() {
  server.send(200, "text/html", htmlConfigPage());
}

void handleSalvar() {
  String s = server.arg("ssid");
  String p = server.arg("pass");
  s.trim();
  if (s.length() == 0) {
    server.send(400, "text/html", htmlConfigPage("O nome da rede (SSID) nao pode ficar vazio."));
    return;
  }
  prefs.begin("wifi_cfg", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();

  server.send(200, "text/html", "<html><body style='font-family:sans-serif;background:#111;color:#fff;padding:20px;'><h3>Salvo! Reiniciando para conectar em: " + s + "</h3></body></html>");
  delay(1500);
  ESP.restart();
}

bool initCamera() {
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
  config.xclk_freq_hz = 20000000;
  config.frame_size   = FRAMESIZE_VGA; // 640x480 para maior nitidez
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.jpeg_quality = 12; // Qualidade JPEG mais refinada
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1); // Corrige imagem invertida verticalmente
  }
  return true;
}

void startAPConfig() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-CAM-CONFIG");
  Serial.println("MODO_CONFIG_AP: Conecte no WiFi ESP32-CAM-CONFIG e abra http://192.168.4.1");
}

void setup() {
  Serial.begin(115200);
  delay(800);

  camOk = initCamera();
  Serial.println(camOk ? "CAM_OK" : "CAM_FAIL");

  prefs.begin("wifi_cfg", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();

  if (savedSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    Serial.print("Conectando em " + savedSsid);
    int tentativas = 0;
    while (WiFi.status() != WL_CONNECTED && tentativas < 30) {
      delay(500);
      Serial.print(".");
      tentativas++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WIFI_OK IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("WIFI_FAIL -> Abrindo AP de configuracao");
      startAPConfig();
    }
  } else {
    startAPConfig();
  }

  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/salvar", HTTP_POST, handleSalvar);
  server.begin();
}

void loop() {
  server.handleClient();

  if (!camOk || apMode || WiFi.status() != WL_CONNECTED) {
    delay(20);
    return;
  }

  unsigned long now = millis();
  bool inMotionWindow = (now <= motionUntilMs);
  unsigned long checkInterval = inMotionWindow ? ACTIVE_SEND_INTERVAL_MS : IDLE_CHECK_INTERVAL_MS;

  if (now - lastCamCheckMs < checkInterval) {
    delay(15);
    return;
  }
  lastCamCheckMs = now;

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    return;
  }

  bool motionNow = detectMotion(fb);
  if (motionNow) {
    motionUntilMs = now + MOTION_HOLD_MS;
    inMotionWindow = true;
  }

  if (inMotionWindow && (now - lastFirebaseSendMs >= ACTIVE_SEND_INTERVAL_MS)) {
    if (sendFrameToFirebase(fb)) {
      lastFirebaseSendMs = now;
    }
  }

  esp_camera_fb_return(fb);
}
