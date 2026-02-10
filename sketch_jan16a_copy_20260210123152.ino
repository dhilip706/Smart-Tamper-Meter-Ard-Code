#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Adafruit_INA219.h>
#include <Adafruit_SSD1306.h>

/* ================= WIFI ================= */
const char* WIFI_SSID = "Redmi Note 13 5G";
const char* WIFI_PASS = "244466666";

/* ================= MQTT (EMQX) ================= */
const char* MQTT_BROKER = "p11d2100.ala.asia-southeast1.emqxsl.com";
const int   MQTT_PORT   = 8883;
const char* MQTT_USER   = "esp32";
const char* MQTT_PASS   = "Esp32mqtt";

/* ================= PINS ================= */
#define SDA_PIN   21
#define SCL_PIN   22
#define HALL_PIN  15
#define LED_BLUE  2

/* ================= OBJECTS ================= */
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);
Adafruit_INA219 ina219;
Adafruit_SSD1306 display(128, 64, &Wire, -1);

/* ================= VARIABLES ================= */
#define AVG_WINDOW 6
float currentBuf[AVG_WINDOW];
int bufIndex = 0;

float energy_kWh = 0.0;
unsigned long lastEnergyMs  = 0;
unsigned long lastDisplayMs = 0;
unsigned long lastPublishMs = 0;
unsigned long lastWifiTryMs = 0;
unsigned long lastMqttTryMs = 0;

/* ================= POLICE LED ================= */
bool tamperActive = false;
bool ledState = false;
bool policePhase = false;
unsigned long ledTimer = 0;

/* ================= HELPERS ================= */
float avgCurrent(float val) {
  currentBuf[bufIndex++] = val;
  if (bufIndex >= AVG_WINDOW) bufIndex = 0;

  float sum = 0;
  for (int i = 0; i < AVG_WINDOW; i++) sum += currentBuf[i];
  return sum / AVG_WINDOW;
}

/* ================= WIFI ================= */
void handleWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  if (millis() - lastWifiTryMs > 3000) {
    lastWifiTryMs = millis();
    Serial.println("📶 Connecting WiFi...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

/* ================= MQTT CONNECT CALLBACK ================= */
void onMqttConnect() {
  Serial.println("✅ MQTT CONNECTED");
  mqtt.publish("smartmeter/WIFI", "ONLINE", true);
}

/* ================= MQTT ================= */
void handleMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  if (millis() - lastMqttTryMs > 3000) {
    lastMqttTryMs = millis();

    String cid = "ESP32_" + String(ESP.getEfuseMac(), HEX);
    Serial.println("🔄 Trying MQTT connect...");

    if (mqtt.connect(
          cid.c_str(),
          MQTT_USER,
          MQTT_PASS,
          "smartmeter/WIFI",
          1,
          true,
          "OFFLINE")) {

      onMqttConnect();

    } else {
      Serial.print("❌ MQTT FAILED, rc=");
      Serial.println(mqtt.state());
    }
  }
}

/* ================= POLICE BLINK ================= */
void policeBlink() {
  unsigned long now = millis();

  if (!policePhase) {
    if (now - ledTimer >= 100) {
      ledTimer = now;
      ledState = !ledState;
      digitalWrite(LED_BLUE, ledState);
    }
    if (now % 1000 < 50) policePhase = true;
  } else {
    if (now - ledTimer >= 350) {
      ledTimer = now;
      ledState = !ledState;
      digitalWrite(LED_BLUE, ledState);
    }
    if (now % 1000 > 950) policePhase = false;
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  pinMode(HALL_PIN, INPUT_PULLUP);
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_BLUE, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  ina219.begin();
  ina219.setCalibration_32V_2A();

  for (int i = 0; i < AVG_WINDOW; i++) currentBuf[i] = 0;

  WiFi.mode(WIFI_STA);
  secureClient.setInsecure();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setKeepAlive(5);

  lastEnergyMs = millis();
}

/* ================= LOOP ================= */
void loop() {
  handleWiFi();
  handleMQTT();
  mqtt.loop();

  unsigned long now = millis();

  bool TAMPER = (digitalRead(HALL_PIN) == LOW);
  tamperActive = TAMPER;

  float V = ina219.getBusVoltage_V();
  float I = ina219.getCurrent_mA() / 1000.0;
  float P = V * I;
  float AVG_I = avgCurrent(I);

  if (now - lastEnergyMs >= 1000) {
    energy_kWh += (P / 1000.0) / 3600.0;
    lastEnergyMs = now;
  }

  if (now - lastDisplayMs >= 300) {
    lastDisplayMs = now;

    display.clearDisplay();
    display.setCursor(0, 0);

    display.printf("V: %.2f V\n", V);
    display.printf("I: %.2f A\n", I);
    display.printf("P: %.2f W\n", P);
    display.printf("AVG I: %.2f A\n", AVG_I);
    display.printf("E: %.6f kWh\n", energy_kWh);

    display.println();
    display.print("WIFI: ");
    display.println(WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE");

    display.print("TAMPER: ");
    display.println(TAMPER ? "YES" : "NO");

    display.display();
  }

  if (tamperActive) {
    policeBlink();
  } else {
    digitalWrite(LED_BLUE, LOW);
  }

  if (now - lastPublishMs >= 1000 && mqtt.connected()) {
    lastPublishMs = now;

    mqtt.publish("smartmeter/V", String(V, 2).c_str());
    mqtt.publish("smartmeter/I", String(I, 3).c_str());
    mqtt.publish("smartmeter/P", String(P, 2).c_str());
    mqtt.publish("smartmeter/AVG_I", String(AVG_I, 3).c_str());
    mqtt.publish("smartmeter/E", String(energy_kWh, 6).c_str());

    mqtt.publish("smartmeter/WIFI",
      WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE", true);

    mqtt.publish("smartmeter/TAMPER",
      TAMPER ? "YES" : "NO", true);
  }
}
