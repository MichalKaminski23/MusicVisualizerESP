#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/i2s.h>
#include <Adafruit_NeoPixel.h>

#define ONBOARD_LED_PIN 10
Adafruit_NeoPixel statusLed(1, ONBOARD_LED_PIN, NEO_RGB + NEO_KHZ800);

#define I2S_SCK_PIN GPIO_NUM_4
#define I2S_SD_PIN GPIO_NUM_5
#define I2S_WS_PIN GPIO_NUM_6
#define I2S_PORT I2S_NUM_0
#define BUFFER_LEN 128

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct AudioData
{
  uint8_t volume;
  uint8_t bass;
  uint8_t mid_high;
} audioPacket;

void setupI2S()
{
  i2s_config_t config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = 22050,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = BUFFER_LEN,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0};

  i2s_pin_config_t pins = {
      .bck_io_num = I2S_SCK_PIN,
      .ws_io_num = I2S_WS_PIN,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_SD_PIN};

  i2s_driver_install(I2S_PORT, &config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
}

void setup()
{
  Serial.begin(115200);

  statusLed.begin();
  statusLed.setPixelColor(0, statusLed.Color(0, 30, 0));
  statusLed.show();

  setupI2S();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Wymuszenie kanału 1 Wi-Fi dla stabilności ESP-NOW
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK)
  {
    Serial.println("Blad ESP-NOW TX");
    statusLed.setPixelColor(0, statusLed.Color(30, 0, 0));
    statusLed.show();
    return;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddress, 6);
  peer.channel = 1;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.println("Nadajnik TX wystartowal!");
}

void loop()
{
  int32_t samples[BUFFER_LEN];
  size_t bytesRead = 0;

  esp_err_t err = i2s_read(I2S_PORT, &samples, sizeof(samples), &bytesRead, pdMS_TO_TICKS(20));
  int count = bytesRead / sizeof(int32_t);

  if (err == ESP_OK && count > 0)
  {
    // 1. Obliczenie składowej stałej (DC offset)
    int64_t sumRaw = 0;
    for (int i = 0; i < count; i++)
    {
      samples[i] = samples[i] >> 14;
      sumRaw += samples[i];
    }
    int32_t dc_offset = sumRaw / count;

    // 2. Czysty sygnał AC (dźwięk)
    int64_t sumSquares = 0;
    for (int i = 0; i < count; i++)
    {
      int32_t clean = samples[i] - dc_offset;
      sumSquares += (int64_t)clean * clean;
    }

    uint32_t rms = sqrt(sumSquares / count);

    // 3. Progowanie (w ciszy RMS wynosi zwykle 30-100)
    int vol = 0;
    if (rms > 80)
    {
      vol = map(rms, 80, 2500, 0, 255);
      vol = constrain(vol, 0, 255);
    }

    audioPacket.volume = vol;
    audioPacket.bass = vol;
    audioPacket.mid_high = vol;

    // Serial.printf("RMS: %4u | DC: %6d | Vol: %3u\n", rms, dc_offset, audioPacket.volume);
  }

  esp_now_send(broadcastAddress, (uint8_t *)&audioPacket, sizeof(audioPacket));
  delay(20);
}