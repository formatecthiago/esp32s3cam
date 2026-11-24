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

// Configuracao do Firebase Realtime Database
static const char* FIREBASE_STREAM_URL = "https://portaria-cam-default-rtdb.firebaseio.com/camera/stream.json";

// Tempos e limites da logica de espera e movimento
static const unsigned long IDLE_CHECK_INTERVAL_MS = 500;    // Checagem leve para detectar movimento
static const unsigned long IDLE_SEND_INTERVAL_MS = 60000;   // 1 imagem a cada 1 minuto em espera
static const unsigned long NO_MOTION_TIMEOUT_MS = 60000;    // Volta para espera apos 1 minuto sem movimento
static const unsigned long MIN_BURST_INTERVAL_MS = 1000;    // Minimo inicial: 1 imagem por segundo (1000 ms)
static const unsigned long MAX_BURST_INTERVAL_MS = 5000;    // Teto seguro de intervalo configuravel
static const float MOTION_DIFF_THRESHOLD = 0.08f;           // 8% de diferenca media entre quadros
static const uint8_t JPEG_ENCODE_QUALITY = 12;              // Qualidade JPEG equilibrada sem PSRAM

WebServer server(80);
Preferences prefs;

bool camOk = false;
bool apMode = false;
String savedSsid = "";
String savedPass = "";

unsigned long burstIntervalMs = MIN_BURST_INTERVAL_MS;
unsigned long lastCamCheckMs = 0;
unsigned long lastFirebaseSendMs = 0;
unsigned long motionUntilMs = 0;
unsigned long totalSentCount = 0;
String lastSendMode = "Aguardando";

uint8_t* prevSampleBuf = nullptr;
size_t prevSampleLen = 0;

struct JpegChunkCollector {
  uint8_t* buf = nullptr;
  size_t len = 0;
  size_t cap = 0;
  bool failed = false;
};

static size_t onJpegChunk(void* arg, size_t index, const void* data, size_t len) {
  auto* ctx = static_cast<JpegChunkCollector*>(arg);
  if (!ctx || ctx->failed || !data || len == 0) return 0;

  size_t needed = index + len;
  if (needed > ctx->cap) {
    size_t newCap = ctx->cap ? ctx->cap : 4096;
    while (newCap < needed) {
      newCap = (newCap * 3) / 2;
    }
    uint8_t* newBuf = static_cast<uint8_t*>(realloc(ctx->buf, newCap));
    if (!newBuf) {
      ctx->failed = true;
      return 0;
    }
    ctx->buf = newBuf;
    ctx->cap = newCap;
  }

  memcpy(ctx->buf + index, data, len);
  if (needed > ctx->len) {
    ctx->len = needed;
  }
  return len;
}

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

bool detectMotion(camera_fb_t* fb) {
  if (!fb || !fb->buf || fb->len < 64) return false;

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

bool sendFrameToFirebase(camera_fb_t* fb, const char* modeLabel) {
  if (WiFi.status() != WL_CONNECTED || !fb || !fb->buf || fb->len == 0) {
    return false;
  }

  JpegChunkCollector jpg;
  bool ok = fmt2jpg_cb(
    fb->buf,
    fb->len,
    fb->width,
    fb->height,
    fb->format,
    JPEG_ENCODE_QUALITY,
    onJpegChunk,
    &jpg
  );

  if (!ok || jpg.failed || !jpg.buf || jpg.len == 0) {
    free(jpg.buf);
    Serial.println("ERR_JPG_ENCODE");
    return false;
  }

  String base64Img = encodeBase64DataUrl(jpg.buf, jpg.len);
  free(jpg.buf);
  jpg.buf = nullptr;

  if (base64Img.length() == 0) {
    Serial.println("ERR_B64");
    return false;
  }

  String payload;
  payload.reserve(base64Img.length() + 48);
  payload = "{\"imagem\":\"";
  payload += base64Img;
  payload += "\",\"timestamp\":";
  payload += String(static_cast<uint64_t>(millis()));
  payload += "}";

  base64Img = "";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, FIREBASE_STREAM_URL)) {
    Serial.println("ERR_HTTP_BEGIN");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PUT(payload);
  http.end();

  if (httpCode >= 200 && httpCode < 300) {
    totalSentCount++;
    lastSendMode = String(modeLabel);
    Serial.printf("SEND_OK [%s] #%lu (HTTP %d)\n", modeLabel, totalSentCount, httpCode);
    return true;
  }
  Serial.printf("ERR_HTTP_%d\n", httpCode);
  return false;
}

void loadSettings() {
  prefs.begin("cam_cfg", true);
  unsigned long loaded = prefs.getULong("burst_ms", MIN_BURST_INTERVAL_MS);
  prefs.end();

  if (loaded < MIN_BURST_INTERVAL_MS) loaded = MIN_BURST_INTERVAL_MS;
  if (loaded > MAX_BURST_INTERVAL_MS) loaded = MAX_BURST_INTERVAL_MS;
  burstIntervalMs = loaded;
}

void saveBurstInterval(unsigned long newIntervalMs) {
  if (newIntervalMs < MIN_BURST_INTERVAL_MS) newIntervalMs = MIN_BURST_INTERVAL_MS;
  if (newIntervalMs > MAX_BURST_INTERVAL_MS) newIntervalMs = MAX_BURST_INTERVAL_MS;

  burstIntervalMs = newIntervalMs;
  prefs.begin("cam_cfg", false);
  prefs.putULong("burst_ms", burstIntervalMs);
  prefs.end();
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

String htmlMainPage(const String& notice = "") {
  unsigned long now = millis();
  bool activeBurst = (now <= motionUntilMs);
  unsigned long secSinceSend = (lastFirebaseSendMs > 0 && now >= lastFirebaseSendMs) ? ((now - lastFirebaseSendMs) / 1000) : 0;
  unsigned long remBurstSec = activeBurst ? ((motionUntilMs - now + 999) / 1000) : 0;

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Portaria CAM - Ajustes</title></head>";
  html += "<body style='font-family:sans-serif;background:#111;color:#fff;padding:18px;max-width:440px;margin:auto;'>";
  html += "<h2>Portaria CAM</h2>";
  html += "<p style='color:#aaa;font-size:14px;'>Rede: " + savedSsid + " | IP: " + WiFi.localIP().toString() + "</p>";

  if (notice.length() > 0) {
    html += "<p style='background:#1f3322;padding:10px;border-left:4px solid #4caf50;font-size:14px;'>" + notice + "</p>";
  }

  html += "<div style='background:#1c1c1c;padding:12px;border-radius:8px;margin-bottom:16px;font-size:14px;line-height:1.6;'>";
  html += "<b>Estado atual:</b> " + String(activeBurst ? "Sequencia rapida" : "Espera (1 foto/min)") + "<br>";
  html += "<b>Intervalo rapido gravado:</b> " + String(burstIntervalMs) + " ms<br>";
  html += "<b>Retorno a espera:</b> 60 s sem movimento" + String(activeBurst ? (" (restam ~" + String(remBurstSec) + " s)") : "") + "<br>";
  html += "<b>Ultimo envio:</b> " + lastSendMode + (lastFirebaseSendMs > 0 ? (" (" + String(secSinceSend) + " s atras)") : " (nenhum ainda)") + "<br>";
  html += "<b>Total enviados:</b> " + String(totalSentCount);
  html += "</div>";

  html += "<form action='/gravar' method='POST' style='background:#1c1c1c;padding:14px;border-radius:8px;margin-bottom:16px;'>";
  html += "<label for='interval_ms' style='display:block;margin-bottom:8px;'><b>Intervalo na sequencia rapida (ms):</b></label>";
  html += "<input id='interval_ms' name='interval_ms' type='number' min='" + String(MIN_BURST_INTERVAL_MS) + "' max='" + String(MAX_BURST_INTERVAL_MS) + "' step='50' value='" + String(burstIntervalMs) + "' style='width:100%;padding:10px;font-size:16px;margin-bottom:8px;'>";
  html += "<small style='color:#aaa;display:block;margin-bottom:12px;'>1000 ms = 1 foto/seg (minimo inicial). Valores menores aumentam o ritmo.</small>";
  html += "<button type='submit' style='width:100%;padding:12px;font-size:16px;background:#2e7d32;color:#fff;border:none;border-radius:6px;'>Gravar</button>";
  html += "</form>";

  html += "<p><a href='/' style='display:inline-block;padding:8px 14px;background:#2a2a2a;color:#fff;text-decoration:none;border-radius:6px;margin-right:8px;'>Atualizar status</a>";
  html += "<a href='/config' style='display:inline-block;padding:8px 14px;background:#2a2a2a;color:#fff;text-decoration:none;border-radius:6px;'>Trocar Wi-Fi</a></p>";
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

void handleGravar() {
  if (apMode) {
    server.send(200, "text/html", htmlConfigPage("Configure o Wi-Fi primeiro."));
    return;
  }
  unsigned long val = server.arg("interval_ms").toInt();
  saveBurstInterval(val);
  server.send(200, "text/html", htmlMainPage("Intervalo gravado com sucesso: " + String(burstIntervalMs) + " ms."));
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
  config.jpeg_quality = JPEG_ENCODE_QUALITY;
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

  loadSettings();

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
  server.on("/gravar", HTTP_POST, handleGravar);
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
  if (now - lastCamCheckMs < IDLE_CHECK_INTERVAL_MS) {
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
    motionUntilMs = now + NO_MOTION_TIMEOUT_MS;
  }

  bool inMotionWindow = (now <= motionUntilMs);
  bool shouldSend = false;
  const char* modeLabel = "Espera 1m";

  if (inMotionWindow) {
    modeLabel = "Movimento";
    if (lastFirebaseSendMs == 0 || (now - lastFirebaseSendMs >= burstIntervalMs)) {
      shouldSend = true;
    }
  } else {
    modeLabel = "Espera 1m";
    if (lastFirebaseSendMs == 0 || (now - lastFirebaseSendMs >= IDLE_SEND_INTERVAL_MS)) {
      shouldSend = true;
    }
  }

  if (shouldSend) {
    if (sendFrameToFirebase(fb, modeLabel)) {
      lastFirebaseSendMs = now;
    }
  }

  esp_camera_fb_return(fb);
}
