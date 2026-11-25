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
framesize_t currentFrameSize = FRAMESIZE_96X96;

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

bool applyFrameSize(framesize_t fs) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;
  int res = s->set_framesize(s, fs);
  if (res == 0) {
    currentFrameSize = fs;
    return true;
  }
  return false;
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

  int w = getFrameWidth(currentFrameSize);
  int h = getFrameHeight(currentFrameSize);

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Teste ESP32-S3 CAM</title></head>";
  html += "<body style='font-family:sans-serif;text-align:center;background:#111;color:#fff;padding:16px;max-width:440px;margin:auto;'>";
  html += "<h2>Teste ESP32-S3 CAM</h2>";
  html += "<p style='color:#9e9e9e;'>Rede: " + savedSsid + " | IP: " + WiFi.localIP().toString() + "</p>";
  if (!camOk) {
    html += "<p style='color:#f66;'>Camera nao iniciou. Confira o encaixe do cabo flat.</p>";
  } else {
    html += "<p><label for='resSelect'>Resolucao:</label> ";
    html += "<select id='resSelect' onchange='trocarResolucao()' style='padding:8px 10px;font-size:15px;border-radius:6px;'>";
    html += String("<option value='96X96'") + (currentFrameSize == FRAMESIZE_96X96 ? " selected" : "") + ">96x96 (Leve)</option>";
    html += String("<option value='QQVGA'") + (currentFrameSize == FRAMESIZE_QQVGA ? " selected" : "") + ">160x120 QQVGA (Medio)</option>";
    html += String("<option value='QVGA'") + (currentFrameSize == FRAMESIZE_QVGA ? " selected" : "") + ">320x240 QVGA (Pesado)</option>";
    html += "</select></p>";

    html += "<p><button onclick='dispararCaptura()' style='padding:12px 22px;font-size:16px;font-weight:bold;background:#2e7d32;color:#fff;border:none;border-radius:6px;margin:4px;'>START</button> ";
    html += "<a href='/config' style='display:inline-block;padding:12px 18px;font-size:16px;background:#333;color:#fff;text-decoration:none;border-radius:6px;'>Trocar Wi-Fi</a></p>";

    html += "<p id='status' style='color:#ccc;'>Pronto. Escolha a resolucao e clique em START.</p>";
    html += "<canvas id='tela' width='" + String(w) + "' height='" + String(h) + "' style='display:none;max-width:100%;height:auto;border:2px solid #444;border-radius:8px;margin:10px auto;'></canvas>";

    html += "<script>";
    html += "let curW=" + String(w) + ", curH=" + String(h) + ";";
    html += "async function trocarResolucao(){";
    html += "  const sel=document.getElementById('resSelect');";
    html += "  const s=document.getElementById('status');";
    html += "  s.textContent='Aplicando resolucao '+sel.value+'...';";
    html += "  try{";
    html += "    const r=await fetch('/resolucao?tam='+encodeURIComponent(sel.value),{cache:'no-store'});";
    html += "    if(!r.ok){s.textContent='Erro ao trocar resolucao';return;}";
    html += "    const d=await r.json();";
    html += "    curW=d.w; curH=d.h;";
    html += "    const cv=document.getElementById('tela');";
    html += "    cv.width=curW; cv.height=curH;";
    html += "    s.textContent='Resolucao: '+curW+'x'+curH+' ('+d.bytes+' bytes). Clique em START.';";
    html += "  }catch(e){";
    html += "    s.textContent='Falha ao aplicar resolucao';";
    html += "  }";
    html += "}";
    html += "function desenharRGB565(buf){";
    html += "  const cv=document.getElementById('tela');";
    html += "  const ctx=cv.getContext('2d');";
    html += "  const img=ctx.createImageData(curW,curH);";
    html += "  const u16=new Uint16Array(buf);";
    html += "  const out=img.data;";
    html += "  const n=Math.min(curW*curH, u16.length);";
    html += "  for(let i=0;i<n;i++){";
    html += "    let p=u16[i];";
    html += "    p=((p&0xff)<<8)|((p>>8)&0xff);";
    html += "    const r=((p>>11)&0x1f)*255/31;";
    html += "    const g=((p>>5)&0x3f)*255/63;";
    html += "    const b=(p&0x1f)*255/31;";
    html += "    const o=i*4;";
    html += "    out[o]=r; out[o+1]=g; out[o+2]=b; out[o+3]=255;";
    html += "  }";
    html += "  ctx.putImageData(img,0,0);";
    html += "  cv.style.display='block';";
    html += "}";
    html += "async function dispararCaptura(){";
    html += "  const s=document.getElementById('status');";
    html += "  const t0=performance.now();";
    html += "  s.textContent='Capturando...';";
    html += "  try{";
    html += "    const r=await fetch('/foto?t='+Date.now(),{cache:'no-store'});";
    html += "    if(!r.ok){s.textContent='Erro HTTP '+r.status;return;}";
    html += "    const buf=await r.arrayBuffer();";
    html += "    desenharRGB565(buf);";
    html += "    const dt=Math.round(performance.now()-t0);";
    html += "    s.textContent='Imagem: '+buf.byteLength+' bytes ('+curW+'x'+curH+') | Tempo: '+dt+' ms';";
    html += "  }catch(e){";
    html += "    s.textContent='Falha ao carregar imagem';";
    html += "  }";
    html += "}";
    html += "</script>";
  }
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleResolucao() {
  if (!camOk) {
    server.send(500, "application/json", "{\"ok\":false}");
    return;
  }
  framesize_t target = parseFrameSize(server.arg("tam"));
  bool ok = applyFrameSize(target);
  int w = getFrameWidth(currentFrameSize);
  int h = getFrameHeight(currentFrameSize);
  int bytesEsperados = w * h * 2;
  String json = "{\"ok\":" + String(ok ? "true" : "false") + ",\"w\":" + String(w) + ",\"h\":" + String(h) + ",\"bytes\":" + String(bytesEsperados) + "}";
  server.send(ok ? 200 : 500, "application/json", json);
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
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Content-Length", String(fb->len));
  server.send_P(200, "application/octet-stream", (const char *)fb->buf, fb->len);
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
  config.frame_size   = currentFrameSize;
  config.pixel_format = PIXFORMAT_RGB565;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_DRAM;
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
  server.on("/resolucao", handleResolucao);
  server.on("/config", handleConfig);
  server.on("/salvar", HTTP_POST, handleSalvar);
  server.on("/foto", handleFoto);
  server.begin();
}

void loop() {
  server.handleClient();
}
