/*
 * ============================================================================
 * ARQUIVO: src/main.cpp
 * PROJETO: Portaria-Cam (ESP32-S3-WROOM-1 MCN16R8 - 16MB Flash / 8MB PSRAM)
 * INTERVENÇÃO: Resolução 320x240, conversão RGB565 -> JPEG via fmt2jpg(),
 *              envio contínuo ao Firebase em Task no Core 0 e log acumulativo
 *              com tempos de conversão (jpg), Base64 (b64) e envio (put).
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Pinos da camera na Freenove ESP32-S3
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

static const WifiCred WIFI_LIST[] = {
  {"FORMATEC 2G", "1234cinco@#"},
  {"NewHome",     "1234cinco@#"},
  {"REALMESPEED", "123quatro@#"}
};
static const size_t WIFI_COUNT = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);

static const char* FIREBASE_LATEST_URL = "https://portaria-cam-default-rtdb.firebaseio.com/camera/stream/latest.json";

WebServer server(80);
static bool camOk = false;
static bool temPsram = false;
static const framesize_t currentFrameSize = FRAMESIZE_QVGA;
static const int currentW = 320;
static const int currentH = 240;
static const int JPEG_QUALITY = 65;

static unsigned long frameSeq = 0;
static unsigned long lastFirebaseMs = 0;
static const unsigned long FIREBASE_INTERVAL_MS = 300;

static String lastFbLog = "Aguardando primeiro envio JPEG ao Firebase...";
static int lastFbCode = 0;
static unsigned long lastFbDurationMs = 0;
static String fbPayload;

static SemaphoreHandle_t camMutex = nullptr;
static SemaphoreHandle_t stateMutex = nullptr;
static volatile bool envioEmAndamento = false;
static uint8_t* pendingJpgBuf = nullptr;
static size_t pendingJpgLen = 0;
static unsigned long pendingJpgMs = 0;

static WiFiClientSecure fbClient;
static HTTPClient fbHttp;
static bool fbHttpIniciado = false;

static bool initCamera() {
  temPsram = psramFound();

  Serial.print("PSRAM_FOUND: ");
  Serial.println(temPsram ? "SIM" : "NAO");
  Serial.print("PSRAM_FREE_BYTES: ");
  Serial.println(ESP.getFreePsram());
  Serial.print("HEAP_FREE_BYTES: ");
  Serial.println(ESP.getFreeHeap());

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
  config.frame_size   = currentFrameSize;
  config.pixel_format = PIXFORMAT_RGB565;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = temPsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.fb_count     = temPsram ? 2 : 1;

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
      Serial.print("WIFI_OK: ");
      Serial.print(WIFI_LIST[i].ssid);
      Serial.print(" IP: ");
      Serial.println(WiFi.localIP());
      return true;
    }
  }

  Serial.println("WIFI_FAIL");
  return false;
}

static bool garantirClienteFirebase() {
  if (fbHttpIniciado) return true;
  fbClient.setInsecure();
  fbClient.setTimeout(3);
  fbHttp.setTimeout(2500);
  fbHttp.setReuse(true);
  if (!fbHttp.begin(fbClient, FIREBASE_LATEST_URL)) {
    fbHttp.end();
    fbHttpIniciado = false;
    return false;
  }
  fbHttp.addHeader("Content-Type", "application/json");
  fbHttpIniciado = true;
  return true;
}

static void registrarLogSeguro(const String& msg, int code, unsigned long totalMs, unsigned long novoFrame) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (novoFrame > 0) frameSeq = novoFrame;
    lastFbCode = code;
    lastFbDurationMs = totalMs;
    lastFbLog = msg;
    xSemaphoreGive(stateMutex);
  }
  Serial.println(msg);
}

static void firebaseTask(void* pvParameters) {
  (void)pvParameters;
  for (;;) {
    uint8_t* jpgBuf = nullptr;
    size_t jpgLen = 0;
    unsigned long tJpgMs = 0;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (envioEmAndamento && pendingJpgBuf != nullptr && pendingJpgLen > 0) {
        jpgBuf = pendingJpgBuf;
        jpgLen = pendingJpgLen;
        tJpgMs = pendingJpgMs;
        pendingJpgBuf = nullptr;
        pendingJpgLen = 0;
      }
      xSemaphoreGive(stateMutex);
    }

    if (jpgBuf != nullptr && jpgLen > 0) {
      unsigned long tInicio = millis();

      if (WiFi.status() != WL_CONNECTED) {
        free(jpgBuf);
        envioEmAndamento = false;
        registrarLogSeguro("Sem Wi-Fi no envio JPEG", -1, 0, 0);
        continue;
      }

      unsigned long tB64Inicio = millis();
      String b64 = base64::encode(jpgBuf, jpgLen);
      unsigned long tB64Ms = millis() - tB64Inicio;
      free(jpgBuf);
      jpgBuf = nullptr;

      unsigned long proxFrame = 0;
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        proxFrame = frameSeq + 1;
        xSemaphoreGive(stateMutex);
      }

      fbPayload = "";
      fbPayload += "{\"formato\":\"jpeg\",\"w\":320,\"h\":240,\"frame\":";
      fbPayload += String(proxFrame);
      fbPayload += ",\"dados\":\"";
      fbPayload += b64;
      fbPayload += "\"}";
      b64 = "";

      if (!garantirClienteFirebase()) {
        unsigned long totalMs = millis() - tInicio;
        envioEmAndamento = false;
        registrarLogSeguro(
          "FB #" + String(proxFrame) + " FALHA begin (" + String(totalMs) + " ms)",
          -2, totalMs, 0
        );
        continue;
      }

      unsigned long tPutInicio = millis();
      int code = fbHttp.PUT(fbPayload);
      unsigned long tPutMs = millis() - tPutInicio;
      fbPayload = "";

      unsigned long totalMs = millis() - tInicio;

      if (code > 0 && code < 300) {
        String msg = "FB #" + String(proxFrame) + " OK (HTTP " + String(code) + ") " +
                     String(totalMs) + "ms [jpg:" + String(tJpgMs) +
                     "ms b64:" + String(tB64Ms) + "ms put:" + String(tPutMs) +
                     "ms " + String(jpgLen / 1024) + "KB]";
        registrarLogSeguro(msg, code, totalMs, proxFrame);
      } else {
        fbHttp.end();
        fbHttpIniciado = false;
        String msg = "FB #" + String(proxFrame) + " ERRO (HTTP " + String(code) + ") " +
                     String(totalMs) + "ms [jpg:" + String(tJpgMs) +
                     "ms b64:" + String(tB64Ms) + "ms put:" + String(tPutMs) + "ms]";
        registrarLogSeguro(msg, code, totalMs, 0);
      }

      envioEmAndamento = false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void agendarEnvioFirebase() {
  if (envioEmAndamento || WiFi.status() != WL_CONNECTED) return;

  camera_fb_t* fb = nullptr;
  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(25)) == pdTRUE) {
    fb = esp_camera_fb_get();
    if (fb) {
      uint8_t* jpgOut = nullptr;
      size_t jpgLen = 0;
      unsigned long t0 = millis();
      bool ok = fmt2jpg(fb->buf, fb->len, currentW, currentH, PIXFORMAT_RGB565, JPEG_QUALITY, &jpgOut, &jpgLen);
      unsigned long tJpg = millis() - t0;
      esp_camera_fb_return(fb);

      if (ok && jpgOut != nullptr && jpgLen > 0) {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          pendingJpgBuf = jpgOut;
          pendingJpgLen = jpgLen;
          pendingJpgMs = tJpg;
          envioEmAndamento = true;
          xSemaphoreGive(stateMutex);
        } else {
          free(jpgOut);
        }
      } else if (jpgOut != nullptr) {
        free(jpgOut);
      }
    }
    xSemaphoreGive(camMutex);
  }
}

static String escapeJsonStr(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

static void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>320x240 JPEG + Log Firebase</title></head>";
  html += "<body style='background:#111;color:#eee;font-family:monospace;padding:12px;text-align:center;'>";
  html += "<h3 style='margin-bottom:8px;'>320x240 JPEG + Core 0</h3>";
  html += "<canvas id='c' width='320' height='240' style='border:1px solid #00ffcc;max-width:100%;height:auto;background:#000;display:block;margin:0 auto 12px;'></canvas>";
  html += "<div id='logBox' style='background:#1a1a1a;border:1px solid #444;border-radius:6px;padding:10px;font-size:12px;text-align:left;max-width:540px;margin:0 auto;line-height:1.5;max-height:240px;overflow-y:auto;'>Aguardando envios ao Firebase...</div>";
  html += "<script>";
  html += "const c=document.getElementById('c'),ctx=c.getContext('2d'),logBox=document.getElementById('logBox');";
  html += "const w=320,h=240; let busy=false,ultimoFrameRegistrado=0,linhas=[];";
  html += "function registrarLog(linha){ linhas.unshift(linha); if(linhas.length>20) linhas.pop(); logBox.innerHTML=linhas.join('<br>'); }";
  html += "async function refreshInfo(){";
  html += "  try{";
  html += "    const r=await fetch('/info',{cache:'no-store'});";
  html += "    const j=await r.json();";
  html += "    if(j.fbFrame !== ultimoFrameRegistrado || linhas.length === 0){";
  html += "      ultimoFrameRegistrado = j.fbFrame;";
  html += "      registrarLog('<b>' + j.lastLog + '</b> | Heap: ' + j.heapFree + ' B');";
  html += "    }";
  html += "  }catch(e){}";
  html += "  setTimeout(refreshInfo,700);";
  html += "}";
  html += "async function tick(){";
  html += "  if(!busy){ busy=true;";
  html += "    try{";
  html += "      const r=await fetch('/frame?t='+Date.now(),{cache:'no-store'});";
  html += "      if(r.ok){";
  html += "        const buf=new Uint8Array(await r.arrayBuffer());";
  html += "        if(buf.length===w*h*2){";
  html += "          const img=ctx.createImageData(w,h),d=img.data;";
  html += "          for(let i=0;i<w*h;i++){";
  html += "            const p=(buf[i*2]<<8)|buf[i*2+1];";
  html += "            d[i*4]=((p>>11)&31)*255/31;";
  html += "            d[i*4+1]=((p>>5)&63)*255/63;";
  html += "            d[i*4+2]=(p&31)*255/31;";
  html += "            d[i*4+3]=255;";
  html += "          }";
  html += "          ctx.putImageData(img,0,0);";
  html += "        }";
  html += "      }";
  html += "    }catch(e){}";
  html += "    busy=false;";
  html += "  }";
  html += "  setTimeout(tick,150);";
  html += "}";
  html += "refreshInfo(); tick();";
  html += "</script></body></html>";
  server.send(200, "text/html", html);
}

static void handleInfo() {
  String logCopy;
  unsigned long frameCopy = 0;
  int codeCopy = 0;
  unsigned long msCopy = 0;

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    logCopy = lastFbLog;
    frameCopy = frameSeq;
    codeCopy = lastFbCode;
    msCopy = lastFbDurationMs;
    xSemaphoreGive(stateMutex);
  }

  String json = "{";
  json += "\"w\":320,\"h\":240,";
  json += "\"fbFrame\":" + String(frameCopy) + ",";
  json += "\"lastCode\":" + String(codeCopy) + ",";
  json += "\"lastMs\":" + String(msCopy) + ",";
  json += "\"heapFree\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"psramFree\":" + String(ESP.getFreePsram()) + ",";
  json += "\"lastLog\":\"" + escapeJsonStr(logCopy) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

static void handleFrame() {
  if (!camOk) {
    server.send(500, "text/plain", "CAM_FAIL");
    return;
  }

  camera_fb_t* fb = nullptr;
  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    fb = esp_camera_fb_get();
    if (!fb) {
      xSemaphoreGive(camMutex);
      server.send(500, "text/plain", "FB_FAIL");
      return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "application/octet-stream", (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    xSemaphoreGive(camMutex);
  } else {
    server.send(503, "text/plain", "CAM_BUSY");
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);

  camMutex = xSemaphoreCreateMutex();
  stateMutex = xSemaphoreCreateMutex();
  fbPayload.reserve(45000);

  camOk = initCamera();
  Serial.println(camOk ? "CAM_OK (320x240 RGB565->JPEG)" : "CAM_FAIL");

  conectarWifi();

  server.on("/", handleRoot);
  server.on("/info", handleInfo);
  server.on("/frame", handleFrame);
  server.begin();

  xTaskCreatePinnedToCore(
    firebaseTask,
    "FirebaseTask",
    8192,
    nullptr,
    1,
    nullptr,
    0
  );
}

void loop() {
  server.handleClient();

  if (camOk && WiFi.status() == WL_CONNECTED) {
    unsigned long agora = millis();
    if (!envioEmAndamento && (lastFirebaseMs == 0 || (agora - lastFirebaseMs) >= FIREBASE_INTERVAL_MS)) {
      lastFirebaseMs = agora;
      agendarEnvioFirebase();
    }
  }

  delay(1);
}
