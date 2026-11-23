#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
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

WebServer server(80);
Preferences prefs;

bool camOk = false;
bool apMode = false;
String savedSsid = "";
String savedPass = "";

String htmlConfigPage(String msg = "") {
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

void handleRoot() {
  if (apMode) {
    server.send(200, "text/html", htmlConfigPage("Conectado ao modo de configuracao. Informe o Wi-Fi abaixo."));
    return;
  }

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Teste ESP32-S3 CAM</title></head>";
  html += "<body style='font-family:sans-serif;text-align:center;background:#111;color:#fff;padding:16px;'>";
  html += "<h2>Teste ESP32-S3 CAM</h2>";
  html += "<p style='color:#9e9e9e;'>Rede: " + savedSsid + " | IP: " + WiFi.localIP().toString() + "</p>";
  if (!camOk) {
    html += "<p style='color:#f66;'>Camera nao iniciou. Confira o encaixe do cabo flat.</p>";
  } else {
    html += "<p><button onclick='location.reload()' style='padding:10px 18px;font-size:16px;margin:4px;'>Atualizar Foto</button> ";
    html += "<a href='/config' style='display:inline-block;padding:10px 18px;font-size:16px;background:#333;color:#fff;text-decoration:none;border-radius:4px;'>Trocar Wi-Fi</a></p>";
    html += "<img src='/foto' style='max-width:100%;border:2px solid #444;border-radius:8px;'/>";
  }
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

  server.send(200, "text/html", "<html><body style='font-family:sans-serif;background:#111;color:#fff;padding:20px;'><h3>Salvo! Reiniciando a placa para conectar em: " + s + "</h3></body></html>");
  delay(1500);
  ESP.restart();
}

void handleFoto() {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Falha ao capturar foto");
    return;
  }
  server.sendHeader("Content-Type", "image/jpeg");
  server.sendHeader("Content-Length", String(fb->len));
  server.send_P(200, "image/jpeg", (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
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
  config.frame_size   = FRAMESIZE_QVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  return (err == ESP_OK);
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
  server.on("/config", handleConfig);
  server.on("/salvar", HTTP_POST, handleSalvar);
  server.on("/foto", handleFoto);
  server.begin();
}

void loop() {
  server.handleClient();
}
