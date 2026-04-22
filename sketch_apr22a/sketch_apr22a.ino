#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h> // เปลี่ยนเป็น Adafruit_BMP280.h หากใช้ BMP
#include <Adafruit_TSL2591.h>
#include <driver/i2s.h>

// --- ตั้งค่า WiFi และ Supabase ---
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* SUPABASE_URL = "https://yuubrzgdmgnajmeajkih.supabase.co/rest/v1/"; // ชื่อตารางของคุณ
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inl1dWJyemdkbWduYWptZWFqa2loIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzY4NDI2MjAsImV4cCI6MjA5MjQxODYyMH0.Jz0k39_YwLkLDpe62iUoF5FH2R_HHbGV8PMlPQVK-LE";

// --- กำหนดพิน ---
#define AD_KEYBOARD_PIN 34
#define IR_PIN 33
#define DUST_LED_PIN 32
#define DUST_OUT_PIN 35
#define I2S_WS 15
#define I2S_SCK 14
#define I2S_SD 36

// --- ออบเจกต์ ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
Adafruit_BME280 bme;
Adafruit_TSL2591 tsl = Adafruit_TSL2591(2591);

// --- ตัวแปรระบบ ---
int currentPage = 0;
const int MAX_PAGES = 4;
unsigned long lastUpdate = 0;
unsigned long lastCloudUpdate = 0;
const int updateInterval = 500;
const int cloudInterval = 10000; // ส่งข้อมูลไป Supabase ทุก 10 วินาที

// ตัวแปรเก็บค่าเซ็นเซอร์ (Global)
float g_dust = 0, g_temp = 0, g_hum = 0, g_pres = 0;
uint16_t g_vis = 0;
int32_t g_sound = 0;

int lastButtonState = 0;
unsigned long lastDebounceTime = 0;
int debounceDelay = 200;

void setup() {
  Serial.begin(115200);
  
  pinMode(IR_PIN, INPUT_PULLUP);
  pinMode(DUST_LED_PIN, OUTPUT);
  pinMode(AD_KEYBOARD_PIN, INPUT);

  lcd.init();
  lcd.backlight();
  lcd.print("Connecting WiFi...");

  // เริ่มต้น WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  lcd.clear();
  lcd.print("WiFi Connected");

  Wire.begin(21, 22);
  if (!bme.begin(0x76)) Serial.println("BME280 fail!");
  if (!tsl.begin()) Serial.println("TSL2591 fail!");

  // I2S สำหรับ INMP441
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = false
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  delay(1000);
  lcd.clear();
}

void loop() {
  handleButtons();
  updateSensorValues(); // อ่านค่าใส่ตัวแปร Global

  // ส่งข้อมูลไป Supabase ตามเวลาที่กำหนด
  if (millis() - lastCloudUpdate > cloudInterval) {
    sendDataToSupabase();
    lastCloudUpdate = millis();
  }

  // อัปเดตจอ
  if (millis() - lastUpdate > updateInterval) {
    displayPage(currentPage);
    lastUpdate = millis();
  }
}

void updateSensorValues() {
  // Dust
  digitalWrite(DUST_LED_PIN, LOW);
  delayMicroseconds(280);
  int voMeasured = analogRead(DUST_OUT_PIN);
  delayMicroseconds(40);
  digitalWrite(DUST_LED_PIN, HIGH);
  float calcVoltage = voMeasured * (3.3 / 4095.0);
  g_dust = 170 * calcVoltage - 0.1;
  if (g_dust < 0) g_dust = 0;

  // BME
  g_temp = bme.readTemperature();
  g_hum = bme.readHumidity();
  g_pres = bme.readPressure() / 100.0F;

  // Light
  uint32_t lum = tsl.getFullLuminosity();
  g_vis = (lum & 0xFFFF) - (lum >> 16);

  // Sound (I2S)
  size_t bytesIn = 0;
  int32_t samples[64];
  i2s_read(I2S_NUM_0, &samples, sizeof(samples), &bytesIn, 0);
  int32_t max_v = 0;
  int samples_read = bytesIn / sizeof(int32_t);
  for (int i = 0; i < samples_read; i++) {
    int32_t val = abs(samples[i] >> 14);
    if (val > max_v) max_v = val;
  }
  g_sound = max_v;
}

void sendDataToSupabase() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(SUPABASE_URL);
    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));
    http.addHeader("Content-Type", "application/json");

    String json = "{\"dust\":" + String(g_dust) + 
                  ",\"temp\":" + String(g_temp) + 
                  ",\"humi\":" + String(g_hum) + 
                  ",\"pres\":" + String(g_pres) + 
                  ",\"light\":" + String(g_vis) + 
                  ",\"sound\":" + String(g_sound) + "}";

    int httpResponseCode = http.POST(json);
    Serial.print("HTTP Code: "); Serial.println(httpResponseCode);
    http.end();
  }
}

void handleButtons() {
  int analogVal = analogRead(AD_KEYBOARD_PIN);
  int currentButton = 0;
  if (analogVal < 200) currentButton = 1;
  else if (analogVal < 1000) currentButton = 2;
  else if (analogVal < 2000) currentButton = 3;

  if (currentButton != 0 && (millis() - lastDebounceTime) > debounceDelay) {
    if (currentButton == 2) { currentPage--; if (currentPage < 0) currentPage = MAX_PAGES; lcd.clear(); }
    else if (currentButton == 3) { currentPage++; if (currentPage > MAX_PAGES) currentPage = 0; lcd.clear(); }
    lastDebounceTime = millis();
  }
}

void displayPage(int page) {
  switch (page) {
    case 0: 
      lcd.setCursor(0, 0); lcd.print("--- Dust Sensor ---");
      lcd.setCursor(0, 2); lcd.print("Density: "); lcd.print(g_dust); lcd.print(" ug/m3 ");
      break;
    case 1:
      lcd.setCursor(0, 0); lcd.print("--- BME280 Env ---");
      lcd.setCursor(0, 1); lcd.print("Temp: "); lcd.print(g_temp, 1);
      lcd.setCursor(0, 2); lcd.print("Humi: "); lcd.print(g_hum, 1);
      break;
    case 2:
      lcd.setCursor(0, 0); lcd.print("--- Light Sensor ---");
      lcd.setCursor(0, 1); lcd.print("Visible: "); lcd.print(g_vis);
      break;
    case 3:
      lcd.setCursor(0, 0); lcd.print("--- IR Sensor ---");
      lcd.setCursor(0, 2); lcd.print(digitalRead(IR_PIN) == LOW ? "DETECTED " : "CLEAR    ");
      break;
    case 4:
      lcd.setCursor(0, 0); lcd.print("--- INMP441 Mic ---");
      lcd.setCursor(0, 2); lcd.print("Amp: "); lcd.print(g_sound);
      break;
  }
}
