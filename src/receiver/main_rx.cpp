#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <FastLED.h>

#define RING_PIN 3
#define NUM_RING_LEDS 24
#define STATUS_PIN 10
#define NUM_STATUS_LEDS 1
#define MAX_BRIGHTNESS 160
#define MIN_BRIGHTNESS 15

CRGB ringLeds[NUM_RING_LEDS];
CRGB statusLed[NUM_STATUS_LEDS];

struct AudioData
{
  uint8_t volume;
  uint8_t bass;
  uint8_t mid_high;
};

AudioData receivedData;
volatile bool newPacket = false;
volatile unsigned long lastPacketTime = 0;
volatile int lastRssi = 0;
volatile uint32_t packetCounter = 0;

bool ledsEnabled = true;
float smoothedVolume = 0.0;

WebServer server(80);

// Obsługa odbioru ESP-NOW wraz z odczytem RSSI
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len)
{
  if (len == sizeof(AudioData))
  {
    memcpy(&receivedData, data, sizeof(AudioData));
    wifi_promiscuous_pkt_t *prom = (wifi_promiscuous_pkt_t *)(data - sizeof(wifi_pkt_rx_ctrl_t));
    lastRssi = prom->rx_ctrl.rssi;
    newPacket = true;
    lastPacketTime = millis();
    packetCounter++;
  }
}

// Odpowiedź JSON dla dynamicznego odświeżania strony (AJAX)
void handleStatus()
{
  bool txConnected = (millis() - lastPacketTime < 1000);
  String json = "{";
  json += "\"connected\":" + String(txConnected ? "true" : "false") + ",";
  json += "\"rssi\":" + String(lastRssi) + ",";
  json += "\"volume\":" + String(receivedData.volume) + ",";
  json += "\"packets\":" + String(packetCounter) + ",";
  json += "\"leds\":" + String(ledsEnabled ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// Przełącznik zasilania programowego LED
void handleToggle()
{
  ledsEnabled = !ledsEnabled;
  if (!ledsEnabled)
  {
    FastLED.clear();
    FastLED.show();
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// Główna strona panelu sterowania
void handleRoot()
{
  String html = "<!DOCTYPE html><html lang='pl'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Visualizer Control Panel</title><style>";
  html += "body{font-family:Arial,sans-serif;background:#121212;color:#eee;text-align:center;padding:20px;}";
  html += ".card{background:#1e1e1e;border-radius:12px;padding:20px;max-width:400px;margin:auto;box-shadow:0 4px 10px rgba(0,0,0,0.5);}";
  html += ".btn{display:inline-block;padding:12px 24px;margin:15px;font-size:16px;border:none;border-radius:8px;cursor:pointer;font-weight:bold;text-decoration:none;color:#fff;}";
  html += ".btn-on{background:#2e7d32;} .btn-off{background:#c62828;}";
  html += ".stat{margin:10px 0;font-size:18px;} .val{font-weight:bold;color:#4fc3f7;}";
  html += ".bar-box{background:#333;height:20px;border-radius:10px;overflow:hidden;margin:10px 0;}";
  html += ".bar{background:#00e676;height:100%;width:0%;transition:width 0.1s;}";
  html += "</style></head><body><div class='card'>";
  html += "<h2>Music Visualizer RX</h2>";
  html += "<div class='stat'>Połączenie z TX: <span id='status' class='val'>--</span></div>";
  html += "<div class='stat'>Moc sygnału (RSSI): <span id='rssi' class='val'>--</span> dBm</div>";
  html += "<div class='stat'>Odebrane pakiety: <span id='packets' class='val'>--</span></div>";
  html += "<div class='stat'>Głośność: <span id='vol' class='val'>--</span>/255</div>";
  html += "<div class='bar-box'><div id='vol-bar' class='bar'></div></div>";
  html += "<a href='/toggle' class='btn " + String(ledsEnabled ? "btn-off" : "btn-on") + "'>";
  html += String(ledsEnabled ? "Wyłącz LEDy" : "Włącz LEDy") + "</a>";
  html += "</div><script>";
  html += "setInterval(()=>{fetch('/status').then(r=>r.json()).then(d=>{";
  html += "document.getElementById('status').innerText = d.connected ? 'ONLINE' : 'OFFLINE';";
  html += "document.getElementById('status').style.color = d.connected ? '#00e676' : '#ff5252';";
  html += "document.getElementById('rssi').innerText = d.rssi;";
  html += "document.getElementById('packets').innerText = d.packets;";
  html += "document.getElementById('vol').innerText = d.volume;";
  html += "document.getElementById('vol-bar').style.width = (d.volume/255*100) + '%';";
  html += "});}, 200);";
  html += "</script></body></html>";
  server.send(200, "text/html", html);
}

void setup()
{
  Serial.begin(115200);

  FastLED.addLeds<WS2812B, STATUS_PIN, RGB>(statusLed, NUM_STATUS_LEDS);
  FastLED.addLeds<WS2812B, RING_PIN, GRB>(ringLeds, NUM_RING_LEDS);
  FastLED.setBrightness(MAX_BRIGHTNESS);

  statusLed[0] = CRGB::Blue;
  FastLED.clear();
  FastLED.show();

  // Tryb AP_STA pozwala jednocześnie hostować Wi-Fi i odbierać ESP-NOW
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Visualizer-RX", "123456789", 1); 

  if (esp_now_init() != ESP_OK)
  {
    statusLed[0] = CRGB::Red;
    FastLED.show();
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/toggle", handleToggle);
  server.begin();

  Serial.println("Serwer HTTP uruchomiony!");
  Serial.print("Adres IP: ");
  Serial.println(WiFi.softAPIP());
}

void loop()
{
  server.handleClient();

  unsigned long now = millis();

  // Brak połączenia z TX
  if (now - lastPacketTime > 1000)
  {
    statusLed[0] = CRGB(30, 0, 0);
    fadeToBlackBy(ringLeds, NUM_RING_LEDS, 10);
    FastLED.show();
    delay(10);
    return;
  }

  statusLed[0] = CRGB(0, 30, 0);

  if (!ledsEnabled)
  {
    FastLED.clear();
    FastLED.show();
    delay(10);
    return;
  }

  // Wygładzanie i render efektu na pierścieniu
  if (receivedData.volume > smoothedVolume)
  {
    smoothedVolume = receivedData.volume;
  }
  else
  {
    smoothedVolume += (receivedData.volume - smoothedVolume) * 0.15;
  }

  uint8_t currentBrightness = map((int)smoothedVolume, 0, 255, MIN_BRIGHTNESS, 255);
  uint8_t hue = map((int)smoothedVolume, 0, 255, 160, 0);

  fill_solid(ringLeds, NUM_RING_LEDS, CHSV(hue, 240, currentBrightness));
  FastLED.show();

  delay(10);
}