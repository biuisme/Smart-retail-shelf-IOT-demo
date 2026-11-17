#include "HX711.h"
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ============================================================
// DE TAI 41 - KE HANG THONG MINH BAO TON KHO
// SV1: ESP32 / Wokwi / Firmware
// Dashboard: Node-RED (xem nodered/smart-shelf-dashboard.json)
// Tai lieu: README.md (so do dau noi, giao thuc MQTT, hieu chuan)
// ============================================================

// 1. WIFI VA MQTT (HIVEMQ PUBLIC - CHI DE THU NGHIEM)
const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

const char* DEVICE_ID = "shelf_01";
const char* topic_telemetry = "smartretail/shelves/shelf_01/telemetry";
const char* topic_commands  = "smartretail/shelves/shelf_01/commands";
const char* topic_status    = "smartretail/shelves/shelf_01/status";

// Last Will: broker tu gui OFFLINE neu mat ket noi MQTT bat ngo.
// ONLINE duoc ESP32 publish sau khi ket noi thanh cong.
const char* mqtt_offline_payload =
  "{\"device_id\":\"shelf_01\",\"connection\":\"OFFLINE\"}";
const char* mqtt_online_payload =
  "{\"device_id\":\"shelf_01\",\"connection\":\"ONLINE\"}";

WiFiClient espClient;
PubSubClient client(espClient);
Preferences preferences;
HX711 scale;
LiquidCrystal_I2C lcd(0x27, 16, 2);
long saved_offset = 0;

// 2. KET NOI PHAN CUNG (GIU NGUYEN diagram.json HIEN TAI)
const int LOADCELL_DOUT_PIN = 4;
const int LOADCELL_SCK_PIN = 5;
const int LED_PIN = 15;

// 3. THONG SO CAN VA NGUONG CANH BAO
// He so 0.42 la gia tri da hoat dong tren mo phong Wokwi.
// Khi lap loadcell 50kg THAT, bat buoc hieu chuan lai.
float calibration_factor = 0.42f;
const int WEIGHT_PER_ITEM = 330;  // 330g / san pham
const int MIN_STOCK_LEVEL = 3;   // Canh bao khi con 0, 1 hoac 2 san pham
const int TOLERANCE = 50;        // +-50g (phai dong bo voi slide)

// 4. CAC BO DEM THOI GIAN
const unsigned long MEASURE_INTERVAL = 2000;    // Doc va gui moi 2 giay
const unsigned long MQTT_RETRY_INTERVAL = 5000; // Thu lai MQTT moi 5 giay
const unsigned long WIFI_RETRY_INTERVAL = 10000;// Thu lai Wi-Fi moi 10 giay
const unsigned long AUTO_ZERO_TIME = 10000;     // Theo doi troi 0g trong 10 giay
const unsigned long RESET_HOLD_TIME = 10000;    // Tat LED sau RESET trong 10 giay

unsigned long lastMeasureMillis = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;
unsigned long zeroDriftStart = 0;
unsigned long resetAlertMillis = 0;
bool mqttAttempted = false;
bool wifiWasConnected = false;
bool force_reset_alert = false;
String mqttClientId;

// 5. LOC TRUNG VI CUA 5 MAU
float getMedianWeight() {
  float samples[5];
  for (int i = 0; i < 5; i++) {
    samples[i] = scale.get_units(1);
  }
  for (int i = 0; i < 4; i++) {
    for (int j = i + 1; j < 5; j++) {
      if (samples[i] > samples[j]) {
        float temp = samples[i];
        samples[i] = samples[j];
        samples[j] = temp;
      }
    }
  }
  return samples[2];
}

// 6. WIFI: KHONG CHO VO HAN TRONG setup()
void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  Serial.println("Dang ket noi Wi-Fi: Wokwi-GUEST");
  WiFi.begin(ssid, password, 6); // Wokwi-GUEST su dung kenh 6
  lastWifiAttempt = millis();
}

void maintain_wifi(unsigned long now) {
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    if (!wifiWasConnected) {
      Serial.println("Da ket noi Wi-Fi thanh cong!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    }
  } else {
    if (wifiWasConnected) {
      Serial.println("Wi-Fi OFFLINE! ESP32 van tiep tuc do can.");
    }
    if (now - lastWifiAttempt >= WIFI_RETRY_INTERVAL) {
      lastWifiAttempt = now;
      Serial.println("Dang thu ket noi lai Wi-Fi...");
      WiFi.reconnect();
    }
  }

  wifiWasConnected = wifiConnected;
}

// 7. NHAN LENH DIEU KHIEN TU NODE-RED
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, topic_commands) != 0) return;

  String messageTemp;
  for (unsigned int i = 0; i < length; i++) {
    messageTemp += (char)payload[i];
  }

  Serial.print("Nhan lenh tu [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(messageTemp);

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, messageTemp);
  if (error) {
    Serial.print("Loi JSON: ");
    Serial.println(error.c_str());
    return;
  }

  const char* command = doc["command"] | "";
  if (strcmp(command, "RESET_ALERT") == 0) {
    force_reset_alert = true;
    resetAlertMillis = millis();
    digitalWrite(LED_PIN, LOW);
    Serial.println(">> DA NHAN LENH RESET_ALERT: Tat LED trong 10 giay!");
  } else {
    Serial.println("Lenh khong hop le / khong duoc ho tro.");
  }
}

// 8. MQTT: KHONG DUNG while CHO KET NOI LAI
void maintain_mqtt(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) return;

  if (client.connected()) {
    client.loop();
    return;
  }

  if (mqttAttempted && now - lastMqttAttempt < MQTT_RETRY_INTERVAL) {
    return;
  }
  mqttAttempted = true;
  lastMqttAttempt = now;

  Serial.printf("[%lu ms] Dang ket noi MQTT...", now);

  // Client ID on dinh theo MAC, tranh trung voi client khac.
  bool ok = client.connect(
    mqttClientId.c_str(),
    topic_status,
    1,                 // Will QoS
    true,              // Will retain
    mqtt_offline_payload
  );

  if (ok) {
    Serial.println(" Thanh cong!");
    if (client.subscribe(topic_commands)) {
      Serial.println("Da subscribe topic commands!");
    } else {
      Serial.println("Subscribe topic commands that bai!");
    }

    bool published = client.publish(topic_status, mqtt_online_payload, true);
    Serial.println(published ? "Da publish ONLINE (LWT status topic)."
                             : "Khong publish duoc ONLINE!");
  } else {
    Serial.print(" That bai! rc=");
    Serial.println(client.state());
    Serial.println("Se thu lai MQTT sau 5 giay...");
  }
}

// 9. HIEN THI LCD
void display_lcd(int weight_g, int quantity, const String& status_str) {
  char firstLine[17];
  snprintf(firstLine, sizeof(firstLine), "Weight: %dg", weight_g);

  lcd.setCursor(0, 0);
  lcd.print("                ");
  lcd.setCursor(0, 0);
  lcd.print(firstLine);

  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  if (status_str == "ERR:OBJECT") {
    lcd.print("ERR: OBJECT!");
  } else {
    lcd.print("Qty:");
    lcd.print(quantity);
    lcd.print(status_str == "LOW_STOCK" ? " | LOW !" : " | OK");
  }
}

// 10. KHOI DONG
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Khoi dong can...");

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(calibration_factor);

  preferences.begin("scale", false);
  if (preferences.isKey("offset")) {
    saved_offset = preferences.getLong("offset", 0);
    scale.set_offset(saved_offset);
    Serial.printf("Da nap offset tu Flash: %ld\n", saved_offset);
  } else {
    scale.tare(); // Dat tai ve 0 truoc khi khoi dong mo phong
    saved_offset = scale.get_offset();
    preferences.putLong("offset", saved_offset);
    Serial.printf("Da Tare va luu offset: %ld\n", saved_offset);
  }
  preferences.end();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqtt_callback);
  client.setKeepAlive(15);     // Broker co the phat LWT sau khi qua keepalive
  client.setSocketTimeout(2); // Gioi han mot phan thoi gian doi MQTT

  mqttClientId = String("ESP32-SmartShelf-") +
                 String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);

  setup_wifi();
  lcd.clear();
  Serial.println("He thong Ke Hang Thong Minh san sang!");
}

// 11. VONG LAP CHINH
void loop() {
  unsigned long now = millis();
  maintain_wifi(now);
  maintain_mqtt(now);

  // Sau 10 giay, cho phep LED canh bao hoat dong tro lai.
  if (force_reset_alert && now - resetAlertMillis >= RESET_HOLD_TIME) {
    force_reset_alert = false;
    Serial.println(">> Het 10s RESET_ALERT, bat lai giam sat LED.");
  }

  if (now - lastMeasureMillis < MEASURE_INTERVAL) return;
  lastMeasureMillis = now;

  if (!scale.is_ready()) {
    Serial.println("Loi HX711: khong san sang!");
    return;
  }

  // 1. Loc trung vi
  float current_weight = getMedianWeight();

  // 2. Auto-zero khi sai lech nho gan muc 0 duy tri 10 giay.
  // Chi dung khi ke thuc su trong, khong tu tare khi dang co hang.
  if (fabsf(current_weight) > 0.5f &&
      fabsf(current_weight) <= TOLERANCE) {
    if (zeroDriftStart == 0) {
      zeroDriftStart = now;
    } else if (now - zeroDriftStart >= AUTO_ZERO_TIME) {
      scale.tare();
      current_weight = 0;
      zeroDriftStart = 0;
      Serial.println(">> [AUTO-ZERO] Da tare lai tai moc 0g.");
    }
  } else {
    zeroDriftStart = 0;
  }

  // 3. Chan nhieu am va nhieu gan 0g
  if (current_weight <= TOLERANCE) current_weight = 0;

  // 4. Dem san pham va phat hien sai lech khoi luong
  int rounded_weight = (int)lroundf(current_weight);
  int quantity = (int)lroundf((float)rounded_weight / WEIGHT_PER_ITEM);
  int expected_weight = quantity * WEIGHT_PER_ITEM;
  int weight_diff = abs(rounded_weight - expected_weight);
  bool is_foreign_object = rounded_weight > 0 && weight_diff > TOLERANCE;

  String status_str = "OK";
  bool turn_on_led = false;
  if (is_foreign_object) {
    status_str = "ERR:OBJECT";
    turn_on_led = true;
  } else if (quantity < MIN_STOCK_LEVEL) {
    status_str = "LOW_STOCK";
    turn_on_led = true;
  }

  if (force_reset_alert) turn_on_led = false;
  digitalWrite(LED_PIN, turn_on_led ? HIGH : LOW);

  // 5. JSON telemetry: luon co 4 truong da thong nhat voi SV2
  StaticJsonDocument<256> tx_doc;
  tx_doc["device_id"] = DEVICE_ID;
  tx_doc["weight_g"] = rounded_weight;
  tx_doc["quantity"] = quantity;
  tx_doc["alert_status"] = status_str;

  char jsonBuffer[256];
  serializeJson(tx_doc, jsonBuffer, sizeof(jsonBuffer));

  bool sent = client.connected() &&
              client.publish(topic_telemetry, jsonBuffer);
  if (sent) {
    Serial.print("Da gui MQTT: ");
    Serial.println(jsonBuffer);
  } else {
    Serial.println("MQTT OFFLINE - Khong gui duoc du lieu!");
  }

  // 6. Cap nhat man hinh ke hang
  display_lcd(rounded_weight, quantity, status_str);
}
