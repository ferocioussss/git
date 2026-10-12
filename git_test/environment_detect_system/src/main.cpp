/*
 * 基于ESP32的物联网环境监测系统（接入OneNET Studio）
 * 功能：采集温湿度、光照、空气质量、人体存在，本地OLED显示，自动报警与控制，云端远程监控。
 */

// ==================== 1. 库文件包含 ====================
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>       // MQTT库
#include <U8g2lib.h>            // OLED驱动库
#include "SHTSensor.h"          // SHT30库
#include <BH1750.h>             // 光照传感器库
#include "Adafruit_SGP30.h"     // 空气质量传感器库
#include <ArduinoJson.h>        // JSON解析与构建库

// ==================== 2. 硬件引脚与对象定义 ====================
// 引脚定义
#define PIN_PIR          4      // HC-SR501输出
#define PIN_BUZZER       13      // 蜂鸣器控制
#define PIN_FAN_RELAY    14    // 风扇继电器控制

// 传感器对象
SHTSensor sht(SHTSensor::SHT3X);  // 实例化SHT30
BH1750 lightMeter;                 // 实例化BH1750
Adafruit_SGP30 sgp;                // 实例化SGP30
// OLED对象 (SSD1306驱动, 128x64分辨率, I2C)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ==================== 3. WiFi与OneNET配置 (务必修改！！) ====================
// WiFi凭证
const char* WIFI_SSID     = "刘帅的iPhone";      // 替换为你的WiFi名称
const char* WIFI_PASSWORD = "14yhl9tt";  // 替换为你的WiFi密码

// OneNET Studio MQTT接入参数 (从平台获取)
const char* ONENET_PRODUCT_ID  = "4P8aXqE138";   // 产品ID
const char* ONENET_DEVICE_NAME = "dev1";  // 设备名称
const char* ONENET_DEVICE_ID   = "dev1";    // 设备ID
const char* ONENET_ACCESS_KEY  = "X1Hd/91VZdXrLWFNd2EHod2PP3VHR5U128/9od39p38";  

// MQTT服务器和端口 (OneNET Studio新版)
const char* MQTT_BROKER   = "studio-mqtt.heclouds.com";
const int   MQTT_PORT     = 1883;

// MQTT主题 (Topic) 格式
char topic_property_post[100]; // 用于属性上报
char topic_property_set[100];  // 用于接收属性设置(控制)

// 根据参数动态生成主题，在setup()中初始化
void generateMqttTopics() {
  sprintf(topic_property_post, "$sys/%s/%s/thing/property/post", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
  sprintf(topic_property_set,  "$sys/%s/%s/thing/property/set", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
}

// 生成连接密码(Token)，此处简化，实际应在电脑端生成长期Token填入下方
// 生成工具：https://tool.heclouds.com/
const char* MQTT_PASSWORD = "version=2018-10-31&res=products%2F4P8aXqE138%2Fdevices%2Fdev1&et=2059886369&method=sha1&sign=AjUt4NN684fow0y6YJAQ7cSE1sA%3D"; // 替换为你的Token

// ==================== 4. 全局变量定义 ====================
// 传感器数据变量
float env_temperature = 0;
float env_humidity = 0;
uint16_t env_light = 0;
uint16_t env_eco2 = 0;
uint16_t env_tvoc = 0;
bool env_human = false;

// 系统状态变量
bool fan_manual_mode = false; // 风扇手动模式标志
bool fan_status = false;      // 风扇当前状态
bool alert_status = false;    // 综合报警状态

// 控制阈值 
float threshold_temp_high = 30.0;
float threshold_humi_high = 100.0;
uint16_t threshold_eco2_high = 10000;
uint16_t threshold_tvoc_high = 5000;

// 定时与计数器
unsigned long lastSensorTime = 0;
unsigned long lastUploadTime = 0;
unsigned long lastDisplayTime = 0;
const long intervalSensor = 2000;   // 传感器读取间隔2s
const long intervalUpload = 10000;  // 数据上传间隔10s
const long intervalDisplay = 1000;  // 显示刷新间隔1s

// ==================== 5. MQTT客户端初始化 ====================
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
void mqttCallback(char* topic, byte* payload, unsigned int length);
// ==================== 6. 核心函数：WiFi与MQTT连接 ====================
void connectToWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void connectToOneNET() {
  generateMqttTopics();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  Serial.println("Connecting to OneNET MQTT...");
  while (!mqttClient.connected()) {
    if (mqttClient.connect(ONENET_DEVICE_ID, ONENET_PRODUCT_ID, MQTT_PASSWORD)) {
      Serial.println("OneNET MQTT Connected!");
      mqttClient.subscribe(topic_property_set);
      Serial.print("Subscribed to: ");
      Serial.println(topic_property_set);
      // 订阅属性上报的回复主题，用于获取服务器返回的错误信息
      String replyTopic = String("$sys/") + ONENET_PRODUCT_ID + "/" + ONENET_DEVICE_NAME + "/thing/property/post/reply";
      mqttClient.subscribe(replyTopic.c_str());
      Serial.print("Subscribed to reply topic: ");
      Serial.println(replyTopic);
      delay(500); // 等待订阅完成
    } else {
      Serial.print("Failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" Retry in 5 seconds...");
      delay(5000);
    }
  }
}

// ==================== 7. 核心函数：传感器数据读取 ====================
void readAllSensors() {
  // 1. 读取温湿度 (SHT30)
  if (sht.readSample()) {
    env_temperature = sht.getTemperature();
    env_humidity = sht.getHumidity();
  }

  // 2. 读取光照 (BH1750)
  if (lightMeter.measurementReady()) {
    env_light = lightMeter.readLightLevel();
  }

  // 3. 读取空气质量 (SGP30)
  if (sgp.IAQmeasure()) {
    env_eco2 = sgp.eCO2;
    env_tvoc = sgp.TVOC;
  }
  // SGP30需要定期测量原始信号以校准
  static unsigned long lastBaselineTime = 0;
  if (millis() - lastBaselineTime > 3600000) { // 每小时存储一次基线
    uint16_t eco2_base, tvoc_base;
    if (sgp.getIAQBaseline(&eco2_base, &tvoc_base)) {
      // 这里可以将基线保存到EEPROM，重启后恢复
    }
    lastBaselineTime = millis();
  }

  // 4. 读取人体感应 (HC-SR501)
  env_human = digitalRead(PIN_PIR);
}

// ==================== 8. 核心函数：本地自动控制与报警逻辑 ====================
void checkAndControl() {
  bool need_fan = false;
  bool need_alert = false;

  // 自动控制逻辑（仅在非手动模式下生效）
  if (!fan_manual_mode) {
    if (env_temperature > threshold_temp_high ||
        env_humidity > threshold_humi_high ||
        env_eco2 > threshold_eco2_high ||
        env_tvoc > threshold_tvoc_high) {
      need_fan = true;
    }
  }
  // 更新风扇状态
  digitalWrite(PIN_FAN_RELAY, need_fan ? LOW : HIGH); // 低电平启动 高电平停止
  fan_status = need_fan;

  // 综合报警逻辑（声光报警）
  if ((env_human && (env_eco2 > threshold_eco2_high || env_humidity > threshold_humi_high)) ||
      env_temperature > threshold_temp_high + 5.0) { // 温度超高时也报警
    need_alert = true;
  }
  alert_status = need_alert;
  // 控制蜂鸣器 低电平发声，高电平不发声
  digitalWrite(PIN_BUZZER, need_alert ? LOW : HIGH);
}

// ==================== 9. 核心函数：OLED显示 ====================
void updateDisplay() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr); // 选择字体

  // 第1行：温度
  u8g2.setCursor(0, 12);
  u8g2.print("T:");
  u8g2.print(env_temperature, 1);
  u8g2.print("C");

  // 第2行：湿度
  u8g2.setCursor(0, 24);
  u8g2.print("H:");
  u8g2.print(env_humidity, 1);
  u8g2.print("%");

  // 第3行：光照
  u8g2.setCursor(0, 36);
  u8g2.print("L:");
  u8g2.print(env_light);
  u8g2.print("lx");

  // 第4行：空气质量
  u8g2.setCursor(0, 48);
  u8g2.print("C:");
  u8g2.print(env_eco2);
  u8g2.print("|T:");
  u8g2.print(env_tvoc);

  // 第5行：状态
  u8g2.setCursor(0, 60);
  u8g2.print("P:");
  u8g2.print(env_human ? "Y" : "N");
  u8g2.print(" F:");
  u8g2.print(fan_status ? "ON" : "OFF");
  u8g2.print(" A:");
  u8g2.print(alert_status ? "!!" : "OK");

  u8g2.sendBuffer(); // 发送缓冲区内容到屏幕
}

// ==================== 10. 核心函数：数据上传至OneNET ====================
void uploadDataToCloud() {
  if (!mqttClient.connected()) {
    Serial.println("MQTT not connected, cannot upload data");
    return;
  }

  Serial.print("Publishing to topic: ");
  Serial.println(topic_property_post);

  // 手动构建符合新格式的 JSON 字符串
  char jsonBuffer[768]; // 适当增大缓冲区，实际长度约 300~400 字符

  snprintf(jsonBuffer, sizeof(jsonBuffer),
    "{"
      "\"id\":\"123\","
      "\"version\":\"1.0\","
      "\"params\":{"
        "\"temperature\":{\"value\":%.1f},"
        "\"humidity\":{\"value\":%.1f},"
        "\"light\":{\"value\":%d},"
        "\"eco2\":{\"value\":%d},"
        "\"tvoc\":{\"value\":%d},"
        "\"human\":{\"value\":%s},"
        "\"fan_switch\":{\"value\":%s},"
        "\"alert\":{\"value\":%s}"
      "}"
    "}",
    env_temperature,                // 浮点数
    env_humidity,                   // 浮点数
    env_light,                      // 整数
    env_eco2,                       // 整数
    env_tvoc,                       // 整数
    env_human ? "true" : "false",   // 布尔转字符串
    fan_status ? "true" : "false",  // 布尔转字符串
    alert_status ? "true" : "false" // 布尔转字符串
  );

  Serial.println("[DEBUG] JSON准备上传:");
  Serial.println(jsonBuffer);

  delay(50); // 给网络一些处理时间

  // 发布到 MQTT（假设 publish 函数接受 const char*）
  mqttClient.publish(topic_property_post, jsonBuffer);
}
// ==================== 11. 核心函数：处理云端下发的指令 ====================
 void mqttCallback(char* topic, byte* payload, unsigned int length) {
   Serial.print("RAW topic: ");
  Serial.println(topic);
  char raw[length+1];
  memcpy(raw, payload, length);
  raw[length] = '\0';
  Serial.print("RAW payload: ");
  Serial.println(raw);

  Serial.print("Message arrived on topic: ");
  Serial.println(topic);

  char msg[length + 1];
  strncpy(msg, (char*)payload, length);
  msg[length] = '\0';
  Serial.print("Payload: ");
  Serial.println(msg);

  // 处理属性上报响应（打印平台反馈）
  if (strstr(topic, "post/reply") != NULL) {
    JsonDocument doc;
    deserializeJson(doc, msg);
    int code = doc["code"] | -1;
    String errMsg = doc["msg"] | "";
    Serial.print("Platform post/reply - code: ");
    Serial.print(code);
    Serial.print(", msg: ");
    Serial.println(errMsg);
    return;
  }

  // 处理平台下发的属性设置指令
  if (strstr(topic, "thing/property/set") != NULL) {
    JsonDocument doc;
    deserializeJson(doc, msg);
    if (!doc["params"].isNull()) {
      JsonObject params = doc["params"];
      if (!params["fan_switch"].isNull()) {
        bool fan_switch_value = params["fan_switch"];
        fan_manual_mode = true;
        digitalWrite(PIN_FAN_RELAY, fan_switch_value ? LOW : HIGH);
        fan_status = fan_switch_value;
        Serial.println("Fan manually controlled via cloud.");
      }
      // 处理蜂鸣器开关（alert）
// 处理蜂鸣器开关（alert）
      if (!params["alert"].isNull()) {
        bool alert_value = params["alert"];
        // 注意：蜂鸣器触发电平可能需要根据你的硬件调整（LOW 或 HIGH）
        digitalWrite(PIN_BUZZER, alert_value ? LOW : HIGH);
        alert_status = alert_value;
        Serial.println("Alert manually controlled via cloud.");
      }
    }
    // **关键：回复确认消息**
    String replyTopic = String("$sys/") + ONENET_PRODUCT_ID + "/" + ONENET_DEVICE_NAME + "/thing/property/set_reply";
    String replyPayload = "{\"code\":200,\"msg\":\"success\"}";
    mqttClient.publish(replyTopic.c_str(), replyPayload.c_str());
    Serial.println("Sent set_reply to platform.");
    return;
  }

  // 其他主题可以忽略或打印
}
// ==================== 12. Arduino标准setup()和loop()函数 ====================
void setup() {
  Serial.println("Waiting for power stability...");
  delay(1000); // 等待1秒，让所有传感器电源完全稳定
  // 初始化串口
  Serial.begin(115200);
  Serial.println("\n====== ESP32 Environment Monitor System Boot ======");

  // 初始化GPIO
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_FAN_RELAY, OUTPUT);
  digitalWrite(PIN_BUZZER, HIGH);
  digitalWrite(PIN_FAN_RELAY, HIGH); // 继电器初始为断开

  // 初始化I2C总线
  Wire.begin();

  // 1. 首先，明确初始化I2C总线并设置可靠的低速
Wire.begin(21, 22); // 明确指定引脚
Wire.setClock(100000); // 强制设置为100kHz标准速度，提高稳定性
delay(500); // 短暂延迟

Serial.println("Initializing sensors...");
delay(100);
// 2. 初始化SHT30 (使用扫描到的地址0x44)
if (sht.init()) {
    sht.setAccuracy(SHTSensor::SHT_ACCURACY_MEDIUM); // 可选：设置精度
    Serial.println("SHT30 init OK.");
} else {
    Serial.println("SHT30 init FAILED!");
}
delay(150);
// 3. 初始化BH1750 (地址0x23)
if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 init OK.");
} else {
    Serial.println("BH1750 init FAILED!");
}
delay(200);
// 4. 初始化SGP30 (地址0x58)
if (sgp.begin()) {
    Serial.println("SGP30 init OK.");
    // 如果之前保存过基线，可以在这里读取并设置
    // sgp.setIAQBaseline(savedECO2Baseline, savedTVOCBaseline);
} else {
    Serial.println("SGP30 init FAILED!");
}

// 5. 初始化OLED (地址0x3C) - 确保在I2C初始化之后
u8g2.begin();
u8g2.clearBuffer();
u8g2.setFont(u8g2_font_ncenB08_tr);
u8g2.drawStr(0, 20, "Sensors Init...");
u8g2.sendBuffer();
Serial.println("OLED init OK.");

  // 连接网络和云端
  connectToWiFi();
  mqttClient.setBufferSize(1024);
  mqttClient.setKeepAlive(60); 
  connectToOneNET();

  Serial.println("====== System Initialization Complete! ======");
  u8g2.clearBuffer();
  u8g2.drawStr(0, 20, "Ready.");
  u8g2.drawStr(0, 40, WiFi.localIP().toString().c_str());
  u8g2.sendBuffer();
  delay(2000);
}

void loop() {
  unsigned long currentMillis = millis();

   if (!mqttClient.connected()) {
    static unsigned long lastReconnectAttempt = 0;
    if (currentMillis - lastReconnectAttempt > 30000) {
      lastReconnectAttempt = currentMillis;
      Serial.println("Attempting MQTT reconnection...");
      connectToOneNET();
    }
  } else {
    mqttClient.loop();
  }

  // 定时读取传感器
  if (currentMillis - lastSensorTime >= intervalSensor) {
    lastSensorTime = currentMillis;
    readAllSensors();
    checkAndControl(); // 根据新数据执行控制逻辑
  }

  // 定时更新显示
  if (currentMillis - lastDisplayTime >= intervalDisplay) {
    lastDisplayTime = currentMillis;
    updateDisplay();
  }

  // 定时上传数据到云端
  if (currentMillis - lastUploadTime >= intervalUpload) {
    lastUploadTime = currentMillis;
    uploadDataToCloud();
  }
}
