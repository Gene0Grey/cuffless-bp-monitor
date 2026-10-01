// Wi-Fi and the live dashboard.
//
// The board joins the network in wifi_secrets.h; if there is none, or it cannot
// connect within 10 s, it opens its own hotspot instead. The dashboard is served
// at "/" and the samples stream to it as server-sent events on "/events":
// one frame every 40 ms with 8 ECG samples (200 Hz) and 2 PPG samples (50 Hz).
//
// Needs the "ESP Async WebServer" and "AsyncTCP" libraries (ESP32Async).
#pragma once
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include "dashboard_page.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef HOTSPOT_PASS
#define HOTSPOT_PASS "cuffless-bp"
#endif
#define WEB_HOST "cuffless-bp"

static AsyncWebServer webServer(80);
static AsyncEventSource webEvents("/events");
static String webIp, webSsid;
static bool webOnRouter = false;

static int16_t webEcg[8];
static int16_t webPpg[2][4];
static uint8_t webEcgN = 0, webPpgN = 0;
static uint32_t webSeq = 0;              // ECG samples since boot

static void webBegin(){
  if(strlen(WIFI_SSID)){
    WiFi.mode(WIFI_STA);
#ifdef WIFI_STATIC_IP
    WiFi.config(IPAddress(WIFI_STATIC_IP), IPAddress(WIFI_GATEWAY), IPAddress(WIFI_SUBNET));
#endif
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Wi-Fi: joining %s", WIFI_SSID);
    for(int i=0; i<40 && WiFi.status()!=WL_CONNECTED; i++){ delay(250); Serial.print('.'); }
    Serial.println();
    webOnRouter = WiFi.status()==WL_CONNECTED;
  }
  if(webOnRouter){
    WiFi.setSleep(false);                // steady 25 frames/s, no power-save bursts
    webSsid = WIFI_SSID;
    webIp = WiFi.localIP().toString();
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    char name[24];
    snprintf(name, sizeof name, "CufflessBP-%04X", (unsigned)(ESP.getEfuseMac() >> 32) & 0xFFFF);
    WiFi.softAP(name, HOTSPOT_PASS);
    webSsid = name;
    webIp = WiFi.softAPIP().toString();
    Serial.printf("Wi-Fi: hotspot %s, password %s\n", name, HOTSPOT_PASS);
  }
  MDNS.begin(WEB_HOST);

  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
    AsyncWebServerResponse *res = req->beginResponse(200, "text/html", DASHBOARD_GZ, DASHBOARD_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip");
    req->send(res);
  });
  webEvents.onConnect([](AsyncEventSourceClient *client){
    char info[192];
    snprintf(info, sizeof info,
             "{\"ip\":\"%s\",\"host\":\"%s.local\",\"net\":\"%s\",\"ssid\":\"%s\",\"up\":%lu}",
             webIp.c_str(), WEB_HOST, webOnRouter ? "wifi" : "hotspot", webSsid.c_str(),
             (unsigned long)millis());
    client->send(info, "i", millis(), 2000);
  });
  webServer.addHandler(&webEvents);
  webServer.onNotFound([](AsyncWebServerRequest *req){ req->send(404, "text/plain", "Not found"); });
  webServer.begin();
  Serial.printf("Dashboard: http://%s  (or http://%s.local)\n", webIp.c_str(), WEB_HOST);
}

// Call once per ECG sample (200 Hz).
static void webAddEcg(float ecg){
  webSeq++;
  if(webEcgN < 8) webEcg[webEcgN++] = (int16_t)lroundf(ecg);
}

// Call once per PPG update (50 Hz), after webAddEcg for the same loop.
static void webAddPpg(const float *ppg, const bool *present, const float *dc, bool leadsOff){
  if(webPpgN < 2) for(int ch=0; ch<4; ch++) webPpg[webPpgN][ch] = (int16_t)lroundf(ppg[ch]);
  webPpgN++;
  if(webPpgN < 2) return;
  if(webEcgN == 8 && webEvents.count()){
    char buf[320];
    int mask = 0;
    for(int ch=0; ch<4; ch++) if(present[ch]) mask |= 1 << ch;
    int n = snprintf(buf, sizeof buf, "{\"s\":%lu,\"lo\":%d,\"m\":%d,\"e\":[", (unsigned long)webSeq, leadsOff ? 1 : 0, mask);
    for(int i=0; i<8; i++) n += snprintf(buf+n, sizeof buf-n, i ? ",%d" : "%d", webEcg[i]);
    n += snprintf(buf+n, sizeof buf-n, "],\"p\":[");
    for(int j=0; j<2; j++){
      n += snprintf(buf+n, sizeof buf-n, "%s[%d,%d,%d,%d]", j ? "," : "",
                    webPpg[j][0], webPpg[j][1], webPpg[j][2], webPpg[j][3]);
    }
    snprintf(buf+n, sizeof buf-n, "],\"dc\":[%ld,%ld,%ld,%ld]}",
             lroundf(dc[0]), lroundf(dc[1]), lroundf(dc[2]), lroundf(dc[3]));
    webEvents.send(buf, "d");
  }
  webEcgN = 0;
  webPpgN = 0;
}
