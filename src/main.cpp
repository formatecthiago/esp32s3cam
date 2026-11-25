#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
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

static const char* FIREBASE_STREAM_URL = "https://portaria-cam-default-rtdb.firebaseio.com/camera/stream.json";

WebServer server(80);
Preferences prefs;

bool camOk = false;
bool apMode = false;
String savedSsid = "";
String savedPass = "";
framesize_t currentFrameSize = FRAMESIZE_96X96;

// 0 = Manual (1 disparo), 1..4 = FPS alvo
int targetFps = 0;
unsigned long lastAutoSendMs = 0;
String lastStatusMsg = "Pronto.";

int getFrameWidth(framesize_t fs) {
  switch (fs) {
    case FRAMESIZE_QQVGA: return 160;
    case FRAMESIZE_QVGA:  return 320;
    case FRAMESIZE_96X96:
    default:              return 96;
  }
}

int getFrameHeight(framesize_t fs) {
  switch (fs) {
    case FRAMESIZE_QQVGA: return 120;
    case FRAMESIZE_QVGA:  return 240;
    case FRAMESIZE_96X96:
    default:              return 96;
  }
}

framesize_t parseFrameSize(const String& val) {
  if (val == "QQVGA") return FRAMESIZE_QQVGA;
  if (val == "QVGA")  return FRAMESIZE_QVGA;
  return FRAMESIZE_96X96;
}

bool initCameraWithSize(framesize_t fs) {
  esp_camera_deinit();

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
  config.frame_size   = fs;
  config.pixel_format = PIXFORMAT_RGB565;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_DRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err == ESP_OK) {
    currentFrameSize = fs;
    return true;
  }
  return false;
}

bool enviarFrameBrutoFirebase(unsigned long* elapsedMsOut, size_t* rawLenOut) {
  if (!camOk || WiFi.status() != WL_CONNECTED) {
    lastStatusMsg = "Sem camera ou sem Wi-Fi.";
    return false;
  }

  unsigned long t0 = millis();
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    lastStatusMsg = "Falha ao capturar frame da camera.";
    return false;
  }

  size_t rawLen = fb->len;
  int w = getFrameWidth(currentFrameSize);
  int h = getFrameHeight(currentFrameSize);

  String b64 = base64::encode(fb->buf, rawLen);
  esp_camera_fb_return(fb);

  String payload;
  payload.reserve(b64.length() + 128);
  payload += "{\"formato\":\"rgb565\",\"w\":";
  payload += String(w);
  payload += ",\"h\":";
  payload += String(h);
  payload += ",\"timestamp\":";
  payload += String((unsigned long)millis());
  payload += ",\"dados\":\"";
  payload += b64;
  payload += "\"}";
  b64 = "";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, FIREBASE_STREAM_URL)) {
    lastStatusMsg = "Falha ao iniciar conexao HTTPS com Firebase.";
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PUT(payload);
  payload = "";
  http.end();

  unsigned long dt = millis() - t0;
  if (elapsedMsOut) *elapsedMsOut = dt;
  if (rawLenOut) *rawLenOut = rawLen;

  if (httpCode > 0 && httpCode < 300) {
    lastStatusMsg = "Enviado " + String(rawLen) + "B (" + String(w) + "x" + String(h) + ") em " + String(dt) + " ms (HTTP " + String(httpCode) + ")";
    return true;
  } else {
    lastStatusMsg = "Erro Firebase HTTP " + String(httpCode) + " em " + String(dt) + " ms";
    return false;
  }
}

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
  html += "<title>Controle ESP32-S3 -> Firebase</title></head>";
  html += "<body style='font-family:sans-serif;text-align:center;background:#111;color:#fff;padding:16px;max-width:440px;margin:auto;'>";
  html += "<h2>ESP32-S3 -> Firebase (Bruto)</h2>";
  html += "<p style='color:#9e9e9e;'>Rede: " + savedSsid + " | IP: " + WiFi.localIP().toString() + "</p>";

  if (!camOk) {
    html += "<p style='color:#f66;'>Camera nao iniciou. Confira o cabo flat.</p>";
  } else {
    html += "<p><label for='resSelect'>Resolucao:</label> ";
    html += "<select id='resSelect' onchange='aplicarParametros()' style='padding:8px 10px;font-size:15px;border-radius:6px;'>";
    html += String("<option value='96X96'") + (currentFrameSize == FRAMESIZE_96X96 ? " selected" : "") + ">96x96 (Leve)</option>";
    html += String("<option value='QQVGA'") + (currentFrameSize == FRAMESIZE_QQVGA ? " selected" : "") + ">160x120 QQVGA (Medio)</option>";
    html += String("<option value='QVGA'") + (currentFrameSize == FRAMESIZE_QVGA ? " selected" : "") + ">320x240 QVGA (Pesado)</option>";
    html += "</select></p>";

    html += "<p><label for='fpsSelect'>Frequencia de envio:</label> ";
    html += "<select id='fpsSelect' onchange='aplicarParametros()' style='padding:8px 10px;font-size:15px;border-radius:6px;'>";
    html += String("<option value='0'") + (targetFps == 0 ? " selected" : "") + ">Manual (1 clique)</option>";
    html += String("<option value='1'") + (targetFps == 1 ? " selected" : "") + ">1 FPS (1000 ms)</option>";
    html += String("<option value='2'") + (targetFps == 2 ? " selected" : "") + ">2 FPS (500 ms)</option>";
    html += String("<option value='3'") + (targetFps == 3 ? " selected" : "") + ">3 FPS (333 ms)</option>";
    html += String("<option value='4'") + (targetFps == 4 ? " selected" : "") + ">4 FPS (250 ms)</option>";
    html += "</select></p>";

    html += "<p><button onclick='enviarAgora()' style='padding:12px 22px;font-size:16px;font-weight:bold;background:#2e7d32;color:#fff;border:none;border-radius:6px;margin:4px;'>ENVIAR AGORA</button> ";
    html += "<button onclick='pararEnvio()' style='padding:12px 16px;font-size:16px;background:#7f1d1d;color:#fff;border:none;border-radius:6px;margin:4px;'>PARAR</button> ";
    html += "<a href='/config' style='display:inline-block;padding:12px 16px;font-size:16px;background:#333;color:#fff;text-decoration:none;border-radius:6px;margin:4px;'>Wi-Fi</a></p>";

    html += "<p id='status' style='color:#ccc;margin-top:12px;'>" + lastStatusMsg + "</p>";

    html += "<script>";
    html += "let pollTimer=null;";
    html += "async function aplicarParametros(){";
    html += "  const tam=document.getElementById('resSelect').value;";
    html += "  const fps=document.getElementById('fpsSelect').value;";
    html += "  const s=document.getElementById('status');";
    html += "  s.textContent='Aplicando parametros...';";
    html += "  try{";
    html += "    const r=await fetch('/parametros?tam='+encodeURIComponent(tam)+'&fps='+encodeURIComponent(fps),{cache:'no-store'});";
    html += "    const d=await r.json();";
    html += "    s.textContent=d.msg||'Parametros atualizados';";
    html += "    gerenciarPoll(parseInt(fps,10));";
    html += "  }catch(e){";
    html += "    s.textContent='Falha ao atualizar parametros';";
    html += "  }";
    html += "}";
    html += "async function enviarAgora(){";
    html += "  const s=document.getElementById('status');";
    html += "  s.textContent='Enviando frame bruto ao Firebase...';";
    html += "  try{";
    html += "    const r=await fetch('/enviar?t='+Date.now(),{cache:'no-store'});";
    html += "    const d=await r.json();";
    html += "    s.textContent=d.msg||'Concluido';";
    html += "  }catch(e){";
    html += "    s.textContent='Falha na requisicao de envio';";
    html += "  }";
    html += "}";
    html += "async function pararEnvio(){";
    html += "  document.getElementById('fpsSelect').value='0';";
    html += "  await aplicarParametros();";
    html += "}";
    html += "function gerenciarPoll(fps){";
    html += "  if(pollTimer){clearInterval(pollTimer);pollTimer=null;}";
    html += "  if(fps>0){";
    html += "    const ms=Math.max(250, Math.round(1000/fps));";
    html += "    pollTimer=setInterval(async()=>{";
    html += "      try{const r=await fetch('/status?t='+Date.now(),{cache:'no-store'});const d=await r.json();if(d.msg)document.getElementById('status').textContent=d.msg;}catch(e){}";
    html += "    }, ms);";
    html += "  }";
    html += "}";
    html += "gerenciarPoll(" + String(targetFps) + ");";
    html += "</script>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleParametros() {
  framesize_t fs = parseFrameSize(server.arg("tam"));
  int fps = server.arg("fps").toInt();
  if (fps < 0) fps = 0;
  if (fps > 4) fps = 4;

  bool camReady = true;
  if (fs != currentFrameSize) {
    camReady = initCameraWithSize(fs);
    camOk = camReady;
  }

  targetFps = fps;
  lastAutoSendMs = 0;

  String msg = camReady
    ? ("Resolucao " + String(getFrameWidth(currentFrameSize)) + "x" + String(getFrameHeight(currentFrameSize)) + " | Frequencia: " + (targetFps == 0 ? String("Manual") : (String(targetFps) + " FPS")))
    : "Falha ao alocar resolucao na camera sem PSRAM.";

  lastStatusMsg = msg;
  server.send(200, "application/json", "{\"ok\":" + String(camReady ? "true" : "false") + ",\"msg\":\"" + msg + "\"}");
}

void handleEnviar() {
  unsigned long dt = 0;
  size_t rawLen = 0;
  bool ok = enviarFrameBrutoFirebase(&dt, &rawLen);
  server.send(ok ? 200 : 500, "application/json", "{\"ok\":" + String(ok ? "true" : "false") + ",\"msg\":\"" + lastStatusMsg + "\"}");
}

void handleStatus() {
  server.send(200, "application/json", "{\"msg\":\"" + lastStatusMsg + "\"}");
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

void startAPConfig() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-CAM-CONFIG");
  Serial.println("MODO_CONFIG_AP: Conecte no WiFi ESP32-CAM-CONFIG e abra http://192.168.4.1");
}

void setup() {
  Serial.begin(115200);
  delay(800);

  camOk = initCameraWithSize(currentFrameSize);
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
  server.on("/parametros", handleParametros);
  server.on("/enviar", handleEnviar);
  server.on("/status", handleStatus);
  server.on("/config", handleConfig);
  server.on("/salvar", HTTP_POST, handleSalvar);
  server.begin();
}

void loop() {
  server.handleClient();

  if (!apMode && camOk && targetFps > 0 && WiFi.status() == WL_CONNECTED) {
    unsigned long intervaloMs = 1000UL / (unsigned long)targetFps;
    unsigned long agora = millis();
    if (lastAutoSendMs == 0 || (agora - lastAutoSendMs) >= intervaloMs) {
      lastAutoSendMs = agora;
      enviarFrameBrutoFirebase(nullptr, nullptr);
    }
  }
}
