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
static size_t psramTotalBytes = 0;
static framesize_t currentFrameSize = FRAMESIZE_QVGA;
static int currentW = 320;
static int currentH = 240;
static unsigned long frameSeq = 0;
static unsigned long lastFirebaseMs = 0;
static unsigned long lastClientReqMs = 0;
static const unsigned long FIREBASE_INTERVAL_MS = 1500;
static const unsigned long WEB_ACTIVE_PAUSE_MS = 2500;

static WiFiClientSecure fbClient;
static bool fbClientReady = false;

static void dimsForSize(framesize_t fs, int& w, int& h) {
  if (fs == FRAMESIZE_QQVGA) {
    w = 160;
    h = 120;
  } else if (fs == FRAMESIZE_VGA) {
    w = 640;
    h = 480;
  } else {
    w = 320;
    h = 240;
  }
}

static bool applyCameraSize(framesize_t fs) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;
  if (s->set_framesize(s, fs) != 0) return false;
  currentFrameSize = fs;
  dimsForSize(fs, currentW, currentH);
  return true;
}

static bool initCamera() {
  temPsram = psramFound();
  psramTotalBytes = ESP.getPsramSize();

  Serial.print("PSRAM_FOUND: ");
  Serial.println(temPsram ? "SIM" : "NAO");
  Serial.print("PSRAM_TOTAL_BYTES: ");
  Serial.println(psramTotalBytes);
  Serial.print("PSRAM_FREE_BYTES: ");
  Serial.println(ESP.getFreePsram());
  Serial.print("HEAP_FREE_BYTES: ");
  Serial.println(ESP.getFreeHeap());

  dimsForSize(currentFrameSize, currentW, currentH);

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

static void enviarFrameFirebase(camera_fb_t* fb) {
  if (!fb || WiFi.status() != WL_CONNECTED) return;

  String b64 = base64::encode(fb->buf, fb->len);
  unsigned long nextFrame = frameSeq + 1;

  String payload;
  payload.reserve(b64.length() + 180);
  payload += "{\"formato\":\"rgb565\"";
  payload += ",\"w\":";
  payload += String(currentW);
  payload += ",\"h\":";
  payload += String(currentH);
  payload += ",\"frame\":";
  payload += String(nextFrame);
  payload += ",\"psram\":";
  payload += String(temPsram ? 1 : 0);
  payload += ",\"ts\":";
  payload += String(millis());
  payload += ",\"dados\":\"";
  payload += b64;
  payload += "\"}";
  b64 = "";

  if (!fbClientReady) {
    fbClient.setInsecure();
    fbClient.setTimeout(6);
    fbClientReady = true;
  }

  HTTPClient http;
  http.setTimeout(6000);
  http.setReuse(true);

  if (!http.begin(fbClient, FIREBASE_LATEST_URL)) {
    http.end();
    Serial.println("FB_FAIL");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(payload);
  http.end();
  payload = "";

  if (code > 0 && code < 300) {
    frameSeq = nextFrame;
    Serial.print("FB_OK #");
    Serial.println(frameSeq);
  } else {
    Serial.print("FB_FAIL ");
    Serial.println(code);
  }
}

static void handleRoot() {
  lastClientReqMs = millis();
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Monitor Resolucao + PSRAM</title></head><body style='background:#111;color:#eee;font-family:sans-serif;padding:14px;text-align:center;'>";
  html += "<h3>Monitoramento & Resolucao</h3>";
  html += "<div style='margin-bottom:10px;'>";
  html += "<button onclick=\"setRes('qqvga')\" style='padding:8px 12px;margin:2px;'>160x120</button>";
  html += "<button onclick=\"setRes('qvga')\" style='padding:8px 12px;margin:2px;'>320x240</button>";
  html += "<button onclick=\"setRes('vga')\" style='padding:8px 12px;margin:2px;'>640x480</button>";
  html += "</div>";
  html += "<div id='log' style='background:#1d1d1d;padding:10px;border-radius:6px;font-size:13px;text-align:left;margin:0 auto 12px;max-width:480px;'>Carregando log...</div>";
  html += "<canvas id='c' width='320' height='240' style='border:1px solid #555;max-width:100%;height:auto;'></canvas>";
  html += "<script>";
  html += "const c=document.getElementById('c'),ctx=c.getContext('2d'),logEl=document.getElementById('log');";
  html += "let w=320,h=240,busy=false;";
  html += "async function setRes(r){ await fetch('/res?size='+r); await refreshInfo(); }";
  html += "async function refreshInfo(){";
  html += "  try{";
  html += "    const r=await fetch('/info',{cache:'no-store'});";
  html += "    const j=await r.json();";
  html += "    if(c.width!==j.w||c.height!==j.h){ w=j.w; h=j.h; c.width=w; c.height=h; }";
  html += "    logEl.innerHTML='PSRAM: <b>'+(j.psram?'SIM':'NAO')+'</b> ('+j.psramTotal+' B) | Livre: '+j.psramFree+' B<br>Heap Livre: '+j.heapFree+' B | Res: '+w+'x'+h+' | FB #'+j.fbFrame;";
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
  html += "  setTimeout(tick,250);";
  html += "}";
  html += "refreshInfo(); setInterval(refreshInfo,1500); tick();";
  html += "</script></body></html>";
  server.send(200, "text/html", html);
}

static void handleRes() {
  lastClientReqMs = millis();
  String sz = server.arg("size");
  framesize_t target = FRAMESIZE_QVGA;
  if (sz == "qqvga") target = FRAMESIZE_QQVGA;
  else if (sz == "vga") target = FRAMESIZE_VGA;

  bool ok = applyCameraSize(target);
  server.send(ok ? 200 : 500, "text/plain", ok ? "RES_OK" : "RES_FAIL");
}

static void handleInfo() {
  lastClientReqMs = millis();
  String json = "{";
  json += "\"psram\":" + String(temPsram ? "true" : "false") + ",";
  json += "\"psramTotal\":" + String(psramTotalBytes) + ",";
  json += "\"psramFree\":" + String(ESP.getFreePsram()) + ",";
  json += "\"heapFree\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"w\":" + String(currentW) + ",";
  json += "\"h\":" + String(currentH) + ",";
  json += "\"fbFrame\":" + String(frameSeq);
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
  Serial.println(camOk ? "CAM_OK" : "CAM_FAIL");

  conectarWifi();

  server.on("/", handleRoot);
  server.on("/res", handleRes);
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
