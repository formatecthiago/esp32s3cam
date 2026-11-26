/*
 * ============================================================================
 * ARQUIVO: src/main.cpp
 * PROJETO: Portaria-Cam (ESP32-S3-WROOM-1 MCN16R8 - 16MB Flash / 8MB PSRAM)
 * INTERVENÇÃO: Resolução exclusiva em 320x240 (FRAMESIZE_QVGA), envio ao
 *              Firebase em camera/stream/latest.json e log acumulativo logo
 *              abaixo da imagem na página da câmera.
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include "esp_camera.h"

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

static unsigned long frameSeq = 0;
static unsigned long lastFirebaseMs = 0;
static unsigned long lastClientReqMs = 0;
static const unsigned long FIREBASE_INTERVAL_MS = 500;
static const unsigned long WEB_ACTIVE_PAUSE_MS = 2500;

static String lastFbLog = "Aguardando primeiro envio ao Firebase...";
static int lastFbCode = 0;
static unsigned long lastFbDurationMs = 0;

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
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
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
  fbHttp.setTimeout(3000);
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

static void enviarFrameFirebase(camera_fb_t* fb) {
  if (!fb || !fb->buf || fb->len == 0 || WiFi.status() != WL_CONNECTED) {
    lastFbCode = -1;
    lastFbDurationMs = 0;
    lastFbLog = "Sem Wi-Fi ou buffer vazio";
    return;
  }

  unsigned long tInicio = millis();
  String b64 = base64::encode(fb->buf, fb->len);
  unsigned long nextFrame = frameSeq + 1;

  String payload;
  payload.reserve(b64.length() + 96);
  payload += "{\"formato\":\"rgb565\",\"w\":320,\"h\":240,\"frame\":";
  payload += String(nextFrame);
  payload += ",\"dados\":\"";
  payload += b64;
  payload += "\"}";
  b64 = "";

  if (!garantirClienteFirebase()) {
    lastFbCode = -2;
    lastFbDurationMs = millis() - tInicio;
    lastFbLog = "FB #" + String(nextFrame) + " FALHA begin (" + String(lastFbDurationMs) + " ms)";
    Serial.println(lastFbLog);
    return;
  }

  int code = fbHttp.PUT(payload);
  payload = "";

  lastFbCode = code;
  lastFbDurationMs = millis() - tInicio;

  if (code > 0 && code < 300) {
    frameSeq = nextFrame;
    lastFbLog = "FB #" + String(frameSeq) + " OK (HTTP " + String(code) + ") em " + String(lastFbDurationMs) + " ms";
  } else {
    fbHttp.end();
    fbHttpIniciado = false;
    lastFbLog = "FB #" + String(nextFrame) + " ERRO (HTTP " + String(code) + ") em " + String(lastFbDurationMs) + " ms";
  }
  Serial.println(lastFbLog);
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
  lastClientReqMs = millis();
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Resolucao 320x240 + Log Firebase</title></head>";
  html += "<body style='background:#111;color:#eee;font-family:monospace;padding:12px;text-align:center;'>";
  html += "<h3 style='margin-bottom:8px;'>Resolucao Exclusiva 320x240</h3>";
  html += "<canvas id='c' width='320' height='240' style='border:1px solid #00ffcc;max-width:100%;height:auto;background:#000;display:block;margin:0 auto 12px;'></canvas>";
  html += "<div id='logBox' style='background:#1a1a1a;border:1px solid #444;border-radius:6px;padding:10px;font-size:12px;text-align:left;max-width:480px;margin:0 auto;line-height:1.5;max-height:220px;overflow-y:auto;'>Aguardando envios ao Firebase...</div>";
  html += "<script>";
  html += "const c=document.getElementById('c'),ctx=c.getContext('2d'),logBox=document.getElementById('logBox');";
  html += "const w=320,h=240; let busy=false,ultimoFrameRegistrado=0,linhas=[];";
  html += "function registrarLog(linha){ linhas.unshift(linha); if(linhas.length>15) linhas.pop(); logBox.innerHTML=linhas.join('<br>'); }";
  html += "async function refreshInfo(){";
  html += "  try{";
  html += "    const r=await fetch('/info',{cache:'no-store'});";
  html += "    const j=await r.json();";
  html += "    if(j.fbFrame > ultimoFrameRegistrado){";
  html += "      ultimoFrameRegistrado = j.fbFrame;";
  html += "      registrarLog('<b>' + j.lastLog + '</b> | Heap: ' + j.heapFree + ' B');";
  html += "    } else if(linhas.length === 0){";
  html += "      logBox.innerHTML = j.lastLog + ' | Heap: ' + j.heapFree + ' B';";
  html += "    }";
  html += "  }catch(e){}";
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
  html += "  setTimeout(tick,200);";
  html += "}";
  html += "refreshInfo(); setInterval(refreshInfo,800); tick();";
  html += "</script></body></html>";
  server.send(200, "text/html", html);
}

static void handleInfo() {
  lastClientReqMs = millis();
  String json = "{";
  json += "\"w\":320,\"h\":240,";
  json += "\"fbFrame\":" + String(frameSeq) + ",";
  json += "\"lastCode\":" + String(lastFbCode) + ",";
  json += "\"lastMs\":" + String(lastFbDurationMs) + ",";
  json += "\"heapFree\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"psramFree\":" + String(ESP.getFreePsram()) + ",";
  json += "\"lastLog\":\"" + escapeJsonStr(lastFbLog) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

static void handleFrame() {
  lastClientReqMs = millis();
  if (!camOk) {
    server.send(500, "text/plain", "CAM_FAIL");
    return;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "FB_FAIL");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "application/octet-stream", (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void setup() {
  Serial.begin(115200);
  delay(400);

  camOk = initCamera();
  Serial.println(camOk ? "CAM_OK (320x240)" : "CAM_FAIL");

  conectarWifi();

  server.on("/", handleRoot);
  server.on("/info", handleInfo);
  server.on("/frame", handleFrame);
  server.begin();
}

void loop() {
  server.handleClient();

  if (camOk && WiFi.status() == WL_CONNECTED) {
    unsigned long agora = millis();
    bool paginaAtiva = (lastClientReqMs > 0 && (agora - lastClientReqMs) < WEB_ACTIVE_PAUSE_MS);
    if (!paginaAtiva && (lastFirebaseMs == 0 || (agora - lastFirebaseMs) >= FIREBASE_INTERVAL_MS)) {
      lastFirebaseMs = agora;
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) {
        enviarFrameFirebase(fb);
        esp_camera_fb_return(fb);
      }
    }
  }

  delay(2);
}
