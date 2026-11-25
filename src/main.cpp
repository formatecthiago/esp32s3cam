#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
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

// Redes Wi-Fi pre-definidas (tenta da primeira para a ultima)
static const WifiCred WIFI_LIST[] = {
  {"FORMATEC 2G", "1234cinco@#"},
  {"NewHome",     "1234cinco@#"},
  {"REALMESPEED", "123quatro@#"}
};
static const size_t WIFI_COUNT = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);

static const char* FIREBASE_BASE_URL = "https://portaria-cam-default-rtdb.firebaseio.com/camera/stream";

// 24 linhas por bloco em 320x240 RGB565: 24 * 320 * 2 = 15360 bytes crus (~20480 chars Base64)
static const int ROWS_PER_CHUNK = 24;

WebServer server(80);

bool camOk = false;
framesize_t currentFrameSize = FRAMESIZE_QVGA; // Padrao 320x240
int targetFps = 0; // 0 = Manual, 1..4 = FPS
unsigned long lastAutoSendMs = 0;
unsigned long frameSeq = 0;
String activeSsid = "";
String lastStatusMsg = "Pronto em 320x240.";

int getFrameWidth(framesize_t fs) {
  switch (fs) {
    case FRAMESIZE_96X96: return 96;
    case FRAMESIZE_QQVGA: return 160;
    case FRAMESIZE_QVGA:
    default:              return 320;
  }
}

int getFrameHeight(framesize_t fs) {
  switch (fs) {
    case FRAMESIZE_96X96: return 96;
    case FRAMESIZE_QQVGA: return 120;
    case FRAMESIZE_QVGA:
    default:              return 240;
  }
}

framesize_t parseFrameSize(const String& val) {
  if (val == "96X96") return FRAMESIZE_96X96;
  if (val == "QQVGA") return FRAMESIZE_QQVGA;
  return FRAMESIZE_QVGA;
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
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err == ESP_OK) {
    currentFrameSize = fs;
    return true;
  }
  return false;
}

bool enviarBlocoFirebase(WiFiClientSecure& client, int chunkIdx, int rowStart, int rowCount, int w, const uint8_t* buf) {
  size_t rowBytes = (size_t)w * 2;
  size_t chunkBytes = (size_t)rowCount * rowBytes;
  const uint8_t* ptr = buf + ((size_t)rowStart * rowBytes);

  String b64 = base64::encode(ptr, chunkBytes);
  String json;
  json.reserve(b64.length() + 96);
  json += "{\"c\":";
  json += String(chunkIdx);
  json += ",\"r0\":";
  json += String(rowStart);
  json += ",\"rc\":";
  json += String(rowCount);
  json += ",\"d\":\"";
  json += b64;
  json += "\"}";
  b64 = "";

  String url = String(FIREBASE_BASE_URL) + "/blocos/" + String(chunkIdx) + ".json";
  HTTPClient http;
  http.setTimeout(7000);
  if (!http.begin(client, url)) {
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(json);
  json = "";
  http.end();

  return (code > 0 && code < 300);
}

bool enviarFrameEmBlocosFirebase(unsigned long* elapsedMsOut) {
  if (!camOk || WiFi.status() != WL_CONNECTED) {
    lastStatusMsg = "Sem camera ou sem Wi-Fi.";
    return false;
  }

  unsigned long t0 = millis();
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    lastStatusMsg = "Falha ao capturar frame (sem heap livre).";
    return false;
  }

  int w = getFrameWidth(currentFrameSize);
  int h = getFrameHeight(currentFrameSize);
  size_t expectedBytes = (size_t)w * (size_t)h * 2;

  if (fb->len < expectedBytes) {
    esp_camera_fb_return(fb);
    lastStatusMsg = "Frame incompleto: " + String(fb->len) + "/" + String(expectedBytes) + " B";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  int totalChunks = (h + ROWS_PER_CHUNK - 1) / ROWS_PER_CHUNK;
  bool ok = true;

  for (int c = 0; c < totalChunks; c++) {
    int r0 = c * ROWS_PER_CHUNK;
    int rc = min(ROWS_PER_CHUNK, h - r0);
    if (!enviarBlocoFirebase(client, c, r0, rc, w, fb->buf)) {
      ok = false;
      lastStatusMsg = "Falha no bloco " + String(c + 1) + "/" + String(totalChunks);
      break;
    }
    delay(2);
  }

  esp_camera_fb_return(fb);

  if (ok) {
    frameSeq++;
    String metaUrl = String(FIREBASE_BASE_URL) + "/meta.json";
    String metaJson = "{\"formato\":\"rgb565\",\"w\":" + String(w) +
                      ",\"h\":" + String(h) +
                      ",\"blocos\":" + String(totalChunks) +
                      ",\"frame\":" + String(frameSeq) +
                      ",\"ts\":" + String(millis()) + "}";

    HTTPClient http;
    http.setTimeout(6000);
    if (http.begin(client, metaUrl)) {
      http.addHeader("Content-Type", "application/json");
      int code = http.PUT(metaJson);
      http.end();
      ok = (code > 0 && code < 300);
    } else {
      ok = false;
    }
  }

  unsigned long dt = millis() - t0;
  if (elapsedMsOut) *elapsedMsOut = dt;

  if (ok) {
    lastStatusMsg = "Frame #" + String(frameSeq) + " (" + String(w) + "x" + String(h) + ") em " + String(dt) + " ms";
  } else if (!lastStatusMsg.startsWith("Falha no bloco")) {
    lastStatusMsg = "Falha ao atualizar meta em " + String(dt) + " ms";
  }
  return ok;
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32-S3 -> Firebase</title></head>";
  html += "<body style='font-family:sans-serif;text-align:center;background:#111;color:#fff;padding:16px;max-width:440px;margin:auto;'>";
  html += "<h2>ESP32-S3 -> Firebase</h2>";
  html += "<p style='color:#9e9e9e;'>Wi-Fi: " + activeSsid + " | IP: " + WiFi.localIP().toString() + "</p>";

  if (!camOk) {
    html += "<p style='color:#f66;'>Camera nao iniciou em 320x240 sem PSRAM. Veja o monitor serial.</p>";
  } else {
    html += "<p><label for='resSelect'>Resolucao:</label> ";
    html += "<select id='resSelect' onchange='aplicarParametros()' style='padding:8px 10px;font-size:15px;border-radius:6px;'>";
    html += String("<option value='QVGA'") + (currentFrameSize == FRAMESIZE_QVGA ? " selected" : "") + ">320x240 QVGA (Padrao)</option>";
    html += String("<option value='QQVGA'") + (currentFrameSize == FRAMESIZE_QQVGA ? " selected" : "") + ">160x120 QQVGA</option>";
    html += String("<option value='96X96'") + (currentFrameSize == FRAMESIZE_96X96 ? " selected" : "") + ">96x96</option>";
    html += "</select></p>";

    html += "<p><label for='fpsSelect'>Frequencia:</label> ";
    html += "<select id='fpsSelect' onchange='aplicarParametros()' style='padding:8px 10px;font-size:15px;border-radius:6px;'>";
    html += String("<option value='0'") + (targetFps == 0 ? " selected" : "") + ">Manual (1 clique)</option>";
    html += String("<option value='1'") + (targetFps == 1 ? " selected" : "") + ">1 FPS</option>";
    html += String("<option value='2'") + (targetFps == 2 ? " selected" : "") + ">2 FPS</option>";
    html += String("<option value='3'") + (targetFps == 3 ? " selected" : "") + ">3 FPS</option>";
    html += String("<option value='4'") + (targetFps == 4 ? " selected" : "") + ">4 FPS</option>";
    html += "</select></p>";

    html += "<p><button onclick='enviarAgora()' style='padding:12px 22px;font-size:16px;font-weight:bold;background:#2e7d32;color:#fff;border:none;border-radius:6px;margin:4px;'>ENVIAR AGORA</button> ";
    html += "<button onclick='pararEnvio()' style='padding:12px 16px;font-size:16px;background:#7f1d1d;color:#fff;border:none;border-radius:6px;margin:4px;'>PARAR</button></p>";

    html += "<p id='status' style='color:#ccc;margin-top:12px;'>" + lastStatusMsg + "</p>";

    html += "<script>";
    html += "let pollTimer=null;";
    html += "async function aplicarParametros(){";
    html += "  const tam=document.getElementById('resSelect').value;";
    html += "  const fps=document.getElementById('fpsSelect').value;";
    html += "  const s=document.getElementById('status');";
    html += "  s.textContent='Aplicando...';";
    html += "  try{";
    html += "    const r=await fetch('/parametros?tam='+encodeURIComponent(tam)+'&fps='+encodeURIComponent(fps),{cache:'no-store'});";
    html += "    const d=await r.json();";
    html += "    s.textContent=d.msg||'OK';";
    html += "    gerenciarPoll(parseInt(fps,10));";
    html += "  }catch(e){s.textContent='Erro ao aplicar';}";
    html += "}";
    html += "async function enviarAgora(){";
    html += "  const s=document.getElementById('status');";
    html += "  s.textContent='Enviando em blocos...';";
    html += "  try{";
    html += "    const r=await fetch('/enviar?t='+Date.now(),{cache:'no-store'});";
    html += "    const d=await r.json();";
    html += "    s.textContent=d.msg||'Concluido';";
    html += "  }catch(e){s.textContent='Erro ao enviar';}";
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

  lastStatusMsg = camReady
    ? (String(getFrameWidth(currentFrameSize)) + "x" + String(getFrameHeight(currentFrameSize)) + " | " + (targetFps == 0 ? String("Manual") : (String(targetFps) + " FPS")))
    : "Falha ao alocar resolucao na camera sem PSRAM.";

  server.send(200, "application/json", "{\"ok\":" + String(camReady ? "true" : "false") + ",\"msg\":\"" + lastStatusMsg + "\"}");
}

void handleEnviar() {
  unsigned long dt = 0;
  bool ok = enviarFrameEmBlocosFirebase(&dt);
  server.send(ok ? 200 : 500, "application/json", "{\"ok\":" + String(ok ? "true" : "false") + ",\"msg\":\"" + lastStatusMsg + "\"}");
}

void handleStatus() {
  server.send(200, "application/json", "{\"msg\":\"" + lastStatusMsg + "\"}");
}

bool conectarWifiLista() {
  WiFi.mode(WIFI_STA);
  for (size_t i = 0; i < WIFI_COUNT; i++) {
    if (WIFI_LIST[i].ssid == nullptr || strlen(WIFI_LIST[i].ssid) == 0) continue;
    WiFi.disconnect(true);
    delay(150);
    WiFi.begin(WIFI_LIST[i].ssid, WIFI_LIST[i].pass);
    int tentativas = 0;
    while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
      delay(400);
      tentativas++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      activeSsid = String(WIFI_LIST[i].ssid);
      return true;
    }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  camOk = initCameraWithSize(currentFrameSize);
  bool wifiOk = conectarWifiLista();

  if (wifiOk) {
    Serial.print("WIFI_OK (");
    Serial.print(activeSsid);
    Serial.print(") IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WIFI_FAIL em todas as redes da lista");
  }

  server.on("/", handleRoot);
  server.on("/parametros", handleParametros);
  server.on("/enviar", handleEnviar);
  server.on("/status", handleStatus);
  server.begin();
}

void loop() {
  server.handleClient();

  if (camOk && targetFps > 0 && WiFi.status() == WL_CONNECTED) {
    unsigned long intervaloMs = 1000UL / (unsigned long)targetFps;
    unsigned long agora = millis();
    if (lastAutoSendMs == 0 || (agora - lastAutoSendMs) >= intervaloMs) {
      lastAutoSendMs = agora;
      enviarFrameEmBlocosFirebase(nullptr);
    }
  }
}
