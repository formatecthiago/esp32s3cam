#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
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

static const char* FIREBASE_STREAM_URL = "https://portaria-cam-default-rtdb.firebaseio.com/camera/stream.json";

WebServer server(80);
Preferences prefs;

bool camOk = false;
bool apMode = false;
String savedSsid = "";
String savedPass = "";
String lastResultMsg = "Nenhuma captura enviada ainda.";
bool hasPreview = false;

String encodeBase64DataUrl(const uint8_t* data, size_t len) {
  size_t outLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outLen, data, len);
  if (outLen == 0) return "";

  static const char* PREFIX = "data:image/jpeg;base64,";
  const size_t prefixLen = strlen(PREFIX);

  char* b64Buf = static_cast<char*>(malloc(outLen + 1));
  if (!b64Buf) return "";

  size_t written = 0;
  int rc = mbedtls_base64_encode(
    reinterpret_cast<unsigned char*>(b64Buf),
    outLen,
    &written,
    data,
    len
  );
  if (rc != 0) {
    free(b64Buf);
    return "";
  }
  b64Buf[written] = '\0';

  String out;
  out.reserve(prefixLen + written + 1);
  out = PREFIX;
  out += b64Buf;

  free(b64Buf);
  return out;
}

String htmlConfigPage(const String& msg = "") {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Configurar Wi-Fi ESP32-S3</title></head>";
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

String htmlMainPage() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Teste 1 Foto - Firebase</title></head>";
  html += "<body style='font-family:sans-serif;text-align:center;background:#111;color:#fff;padding:18px;max-width:440px;margin:auto;'>";
  html += "<h2>Teste 1 Foto - Firebase</h2>";
  html += "<p style='color:#aaa;font-size:14px;'>Rede: " + savedSsid + " | IP: " + WiFi.localIP().toString() + "</p>";
  html += "<p style='background:#1c1c1c;padding:10px;border-radius:6px;font-size:14px;'>" + lastResultMsg + "</p>";

  if (!camOk) {
    html += "<p style='color:#f66;'>Camera nao iniciou. Confira o cabo flat.</p>";
  } else {
    html += "<form action='/start' method='POST' style='margin:14px 0;'>";
    html += "<button type='submit' style='width:100%;padding:14px;font-size:18px;font-weight:bold;background:#2e7d32;color:#fff;border:none;border-radius:8px;cursor:pointer;'>START (Capturar e Enviar 1 Foto)</button>";
    html += "</form>";

    if (hasPreview) {
      html += "<div style='margin-top:12px;'>";
      html += "<img src='/foto?t=" + String(millis()) + "' style='max-width:100%;border:2px solid #444;border-radius:8px;'/>";
      html += "</div>";
    }
  }

  html += "<p style='margin-top:16px;'><a href='/config' style='display:inline-block;padding:8px 14px;background:#2a2a2a;color:#fff;text-decoration:none;border-radius:6px;'>Trocar Wi-Fi</a></p>";
  html += "</body></html>";
  return html;
}

void handleRoot() {
  if (apMode) {
    server.send(200, "text/html", htmlConfigPage("Conectado ao modo de configuracao. Informe o Wi-Fi abaixo."));
    return;
  }
  server.send(200, "text/html", htmlMainPage());
}

void handleFoto() {
  if (!camOk) {
    server.send(500, "text/plain", "Camera indisponivel");
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Falha ao capturar foto");
    return;
  }

  uint8_t* jpg_buf = nullptr;
  size_t jpg_len = 0;
  bool converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
  esp_camera_fb_return(fb);

  if (!converted || !jpg_buf) {
    free(jpg_buf);
    server.send(500, "text/plain", "Falha ao converter JPEG");
    return;
  }

  server.sendHeader("Content-Type", "image/jpeg");
  server.sendHeader("Content-Length", String(jpg_len));
  server.send_P(200, "image/jpeg", (const char*)jpg_buf, jpg_len);
  free(jpg_buf);
}

void handleStart() {
  if (apMode || !camOk) {
    server.send(200, "text/html", htmlMainPage());
    return;
  }

  Serial.println("=== INICIANDO TESTE DE 1 FOTO ===");
  Serial.printf("Heap livre antes da captura: %u bytes\n", ESP.getFreeHeap());

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    lastResultMsg = "Erro: falha ao capturar frame da camera.";
    Serial.println("ERR_FB_GET");
    server.send(200, "text/html", htmlMainPage());
    return;
  }

  uint8_t* jpg_buf = nullptr;
  size_t jpg_len = 0;
  bool converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
  esp_camera_fb_return(fb);
  fb = nullptr;

  if (!converted || !jpg_buf || jpg_len == 0) {
    free(jpg_buf);
    lastResultMsg = "Erro: falha ao converter frame para JPEG (frame2jpg).";
    Serial.println("ERR_FRAME2JPG");
    server.send(200, "text/html", htmlMainPage());
    return;
  }

  hasPreview = true;
  Serial.printf("JPEG OK: %u bytes | Heap livre: %u bytes\n", (unsigned)jpg_len, ESP.getFreeHeap());

  String base64Img = encodeBase64DataUrl(jpg_buf, jpg_len);
  free(jpg_buf);
  jpg_buf = nullptr;

  if (base64Img.length() == 0) {
    lastResultMsg = "Erro: falha ao gerar Base64 (sem RAM suficiente).";
    Serial.println("ERR_B64_MALLOC");
    server.send(200, "text/html", htmlMainPage());
    return;
  }

  String payload;
  payload.reserve(base64Img.length() + 48);
  payload = "{\"imagem\":\"";
  payload += base64Img;
  payload += "\",\"timestamp\":";
  payload += String(static_cast<uint64_t>(millis()));
  payload += "}";
  base64Img = "";

  Serial.printf("Payload JSON: %u bytes | Heap livre antes HTTPS: %u bytes\n", payload.length(), ESP.getFreeHeap());

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setTimeout(15000);

  if (!http.begin(client, FIREBASE_STREAM_URL)) {
    lastResultMsg = "Erro: http.begin falhou.";
    Serial.println("ERR_HTTP_BEGIN");
    server.send(200, "text/html", htmlMainPage());
    return;
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PUT(payload);
  String errStr = http.errorToString(httpCode);
  http.end();

  if (httpCode >= 200 && httpCode < 300) {
    lastResultMsg = "SUCESSO! 1 foto enviada ao Firebase (HTTP " + String(httpCode) + ").";
    Serial.printf("FIREBASE_OK HTTP %d\n", httpCode);
  } else {
    lastResultMsg = "Falha no Firebase: codigo " + String(httpCode) + " (" + errStr + ")";
    Serial.printf("FIREBASE_FAIL code=%d (%s)\n", httpCode, errStr.c_str());
  }

  server.send(200, "text/html", htmlMainPage());
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
  config.xclk_freq_hz = 10000000;
  config.frame_size   = FRAMESIZE_QVGA;
  config.pixel_format = PIXFORMAT_RGB565;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_DRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_hmirror(s, 1);
    s->set_vflip(s, 1);
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
  server.on("/foto", handleFoto);
  server.on("/start", HTTP_POST, handleStart);
  server.on("/config", handleConfig);
  server.on("/salvar", HTTP_POST, handleSalvar);
  server.begin();
}

void loop() {
  server.handleClient();
}
