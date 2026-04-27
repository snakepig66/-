#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include <map>
#include <HardwareSerial.h>
// #include "esp_psram.h"      // 引入PSRAM头文件，用于处理大图像
#include "mbedtls/base64.h" // 引入ARM官方高性能Base64库
#include "HX711.h"          // 引入HX711称重传感器库
#include <Wire.h>           // I2C 库，U8g2 需要
#include <U8g2lib.h>        // OLED 显示库
#include <Keypad.h>         // 4x4矩阵键盘库
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>  
// ======================== 网络与 API 配置 ========================
const char *ssid = "waveshare";    // 你的WiFi SSID
const char *password = "12345678"; // 你的WiFi密码

// Qwen (阿里云百炼) API 配置
String qwenApiKey = "sk-8003480a642b4c75aaf683be2bea5355"; // 阿里云百炼 API Key
const char *apiUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";

// =========================== OneNet 配置 ==========================
const char *mqtt_server_onenet = "mqtts.heclouds.com"; 
const int mqtt_port_onenet = 1883;

const char *onenet_product_id = "96LmRjv77N";
const char *onenet_device_id = "niu";
const char *onenet_token = "version=2018-10-31&res=products%2F96LmRjv77N%2Fdevices%2Fniu&et=2000000000&method=md5&sign=fZy20DseTrk7WWVOOqh9GA%3D%3D"; 
const char *onenet_topic_post = "$sys/96LmRjv77N/niu/thing/property/post";

// ===================== 公共 MQTT 配置（小程序直连） =====================
const char *mqtt_server_public = "broker.emqx.io";
const int mqtt_port_public = 1883;
// ClientID 需唯一，可加随机数避免冲突
const char *public_client_id = "esp32_scale_device_niu_8848"; 
const char *public_topic_post = "smart_scale/app/data_stream";

// ===================== 初始化双路 WiFiClient 客户端 =====================
WiFiClient espClientOneNet;
PubSubClient mqttClientOneNet(espClientOneNet);

WiFiClient espClientPublic;
PubSubClient mqttClientPublic(espClientPublic);

// =========================== 硬件引脚配置 ==========================
#define PIR_PIN 45           // HC-SR501 人体红外传感器引脚
#define JPG_SERIAL_RX_PIN 44 // 图像串口接收引脚 (连接到图像发送模块的TX)
//#define JPG_SERIAL_TX_PIN 17 // 图像串口发送引脚 (连接到图像发送模块的RX)
#define JPG_SERIAL_TX_PIN 1  // 使用空闲 1 引脚接摄像头板 RX (44)
#define SD_CS_PIN 10         // SD卡片选引脚
#define SD_MOSI_PIN 11       // SD卡MOSI引脚
#define SD_MISO_PIN 13       // SD卡MISO引脚
#define SD_SCK_PIN 12        // SD卡时钟引脚
#define HX711_DOUT_PIN 4     // HX711数据输出引脚
#define HX711_SCK_PIN 5      // HX711时钟引脚
#define RGB_LED_PIN 48       // led
const byte KEYPAD_ROWS = 4;
const byte KEYPAD_COLS = 4;
char keys[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};

byte rowPins[KEYPAD_ROWS] = {47, 21, 20, 19};
byte colPins[KEYPAD_COLS] = {16, 15, 7, 6};

// ========================= 软件与协议配置 =========================
enum SystemMode
{
  AUTOMATIC_MODE,
  MANUAL_MODE
};
SystemMode currentMode = MANUAL_MODE;

const uint8_t START_MARKER[] = {0xAB, 0xCD, 0xEF};
const uint8_t END_MARKER[] = {0xFE, 0xDC, 0xBA};
float calibration_factor = 460;


// ===================== FreeRTOS 任务与同步句柄 =====================
TaskHandle_t wifiManagementTaskHandle;
TaskHandle_t lcdDisplayTaskHandle;
TaskHandle_t oneNetTaskHandle;
TaskHandle_t aiRecognitionTaskHandle; // AI 后台识别任务句柄

EventGroupHandle_t wifiEventGroup;
SemaphoreHandle_t displayDataMutex;
SemaphoreHandle_t aiDataMutex;        // 用于保护 AI 任务数据的锁
const int WIFI_CONNECTED_BIT = BIT0;

// ========================= 全局对象与变量 =========================
HardwareSerial ASRSerial(2);
#define ASR_RX_PIN 8
#define ASR_TX_PIN 9

HardwareSerial JPGSerial(1);
SPIClass SDSPI(HSPI);
HX711 scale;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
Keypad customKeypad = Keypad(makeKeymap(keys), rowPins, colPins, KEYPAD_ROWS, KEYPAD_COLS);

Adafruit_NeoPixel onboardLED(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

std::map<String, String> priceList;
String lastRecognizedItemName = "";
float lastUnitPrice = 0.0;
String manualPriceInput = "";

// 全局重量缓存，防止多任务争抢 HX711 死锁，提高读取速度
volatile float global_weight = 0.0; 

// AI 后台识别所需变量
volatile bool isRecognizing = false;
String pendingImageBase64 = "";

// 屏幕休眠控制
unsigned long last_motion_time = 0;
bool is_sleeping = false;

// ============================ 函数声明 ============================
void wifiManagementTask(void *pvParameters);
void lcdDisplayTask(void *pvParameters);
void oneNetDataTask(void *pvParameters);
void aiRecognitionTask(void *pvParameters); // 【优化】新增任务：负责处理耗时的AI网络请求
void checkIncomingImage(); // 检查是否有传入图像

// 参数采用 const String&，避免大字符串深拷贝
String recognizeIngredient(const String& base64Image); 

void loadPriceList();
bool findStartMarker();
void flushSerialBuffer(size_t count);
float getWeight();
void setLED(int color);
void handleKeypadInput();
void performTare();
void handleAutomaticMode();
void handleManualMode();
void handleAsrInput();
// =========================== 主程序 Setup ==========================
void setup()
{
  Serial.begin(115200);
  while (!Serial)
    ;
  Serial.println("\n\n--- ESP32-S3 双模式智能计价系统 (Qwen AI 优化版) ---");
  Serial.printf("主程序 setup() 运行在核心: %d\n", xPortGetCoreID());

  ASRSerial.begin(9600, SERIAL_8N1, ASR_RX_PIN, ASR_TX_PIN);
  Serial.println("ASRPRO 语音识别模块串口已在 GPIO 8/9 上初始化。");

  onboardLED.begin();
  onboardLED.setBrightness(50); // 设置亮度(0-255)，50为推荐值
  onboardLED.show();
  setLED(1); // 初始化为红灯（未连接WiFi）

  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawStr(0, 15, "系统启动中...");
  u8g2.sendBuffer();

  if (!psramFound())
  {
    Serial.println("错误: PSRAM未找到!");
    while (1)
      ;
  }
  Serial.printf("PSRAM 总大小: %d bytes, 可用: %d bytes\n", ESP.getPsramSize(), ESP.getFreePsram());

  // 分配更大串口接收缓冲区(32KB)，防止高波特率下丢包
  JPGSerial.setRxBufferSize(32768); 
  JPGSerial.begin(921600, SERIAL_8N1, JPG_SERIAL_RX_PIN, JPG_SERIAL_TX_PIN);
  Serial.printf("JPGSerial 已初始化, RX引脚: %d, TX引脚: %d\n", JPG_SERIAL_RX_PIN, JPG_SERIAL_TX_PIN);

  Serial.println("正在初始化 HX711 称重模块...");
  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
  scale.set_scale(calibration_factor);
  scale.tare();
  Serial.println("HX711 初始化完成，已去皮。");

  pinMode(PIR_PIN, INPUT);
  last_motion_time = millis(); // 初始化人体红外时间

  displayDataMutex = xSemaphoreCreateMutex();
  aiDataMutex = xSemaphoreCreateMutex(); // 初始化 AI 数据锁
  wifiEventGroup = xEventGroupCreate();
  
  mqttClientOneNet.setServer(mqtt_server_onenet, mqtt_port_onenet);
  mqttClientOneNet.setBufferSize(512);

  mqttClientPublic.setServer(mqtt_server_public, mqtt_port_public);
  mqttClientPublic.setBufferSize(512);
  
  xTaskCreatePinnedToCore(wifiManagementTask, "WiFiTask", 4096, NULL, 1, &wifiManagementTaskHandle, 0);
  xTaskCreatePinnedToCore(lcdDisplayTask, "LCDTask", 4096, NULL, 2, &lcdDisplayTaskHandle, 0);
  xTaskCreatePinnedToCore(oneNetDataTask, "OneNetTask", 4096, NULL, 1, &oneNetTaskHandle, 1);
  
  // 分配 8KB 栈给 AI 任务，核心 0 处理网络请求
  xTaskCreatePinnedToCore(aiRecognitionTask, "AITask", 8192, NULL, 1, &aiRecognitionTaskHandle, 0);

  Serial.println("正在初始化 SD 卡...");
  SDSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, -1);
  if (!SD.begin(SD_CS_PIN, SDSPI))
  {
    Serial.println("SD 卡初始化失败!");
    while (1)
      ;
  }
  Serial.println("SD 卡初始化成功!");

  loadPriceList();

  Serial.println("系统正在启动，WiFi将在后台连接...");
  Serial.println("\n--- 系统初始化完成，当前为手动模式 ---");
}

// =========================== 主循环 loop ===========================
// 负责键盘检测、传感器读取与模式分发
void loop()
{
  // 非阻塞读取重量，缓存到全局变量
  if (scale.is_ready()) {
    float temp_weight = scale.get_units(3); // 降低采样数到3，大幅提升循环帧率
    if (temp_weight >= 0) {
      global_weight = temp_weight; 
    }
  }

  handleKeypadInput();
  handleAsrInput();
  
  checkIncomingImage(); // 随时监听并处理摄像头传来的图像
  
  if (currentMode == MANUAL_MODE)
  {
    handleManualMode();
  }
}

// ===================== 自动处理图像核心逻辑 =====================
void checkIncomingImage()
{
  if (JPGSerial.available()) {
    // 这里加个提示看看是不是收到了随便什么数据
    // Serial.println(">>> 发现 JPGSerial 串口有数据进来...");
  }
  
  if (findStartMarker())
  {
    Serial.println(">>> 成功匹配到图片起始标记 START_MARKER！");
    if ((xEventGroupGetBits(wifiEventGroup) & WIFI_CONNECTED_BIT) == 0)
    {
      Serial.println("收到图像，但WiFi未连接，无法识别!");
      // 等待一点时间再清空串口缓冲，防止还没发完
      delay(500);
      flushSerialBuffer(100000); 
      return;
    }

    // 不管之前是什么模式，收到图片立刻切换为自动模式
    if (currentMode == MANUAL_MODE)
    {
      currentMode = AUTOMATIC_MODE;
      Serial.println("收到摄像头拍照，自动切换到自动识别模式");
      if (xSemaphoreTake(displayDataMutex, portMAX_DELAY) == pdTRUE)
      {
        lastRecognizedItemName = "";
        lastUnitPrice = 0.0;
        manualPriceInput = "";
        xSemaphoreGive(displayDataMutex);
      }
    }

    uint32_t image_size = 0;
    if (JPGSerial.readBytes((uint8_t *)&image_size, sizeof(image_size)) != sizeof(image_size))
    {
      Serial.println("错误: 读取图像大小超时。");
      return;
    }
    if (image_size == 0 || image_size > ESP.getFreePsram())
    {
      Serial.printf("错误: 无效或过大的图像尺寸: %u\n", image_size);
      return;
    }
    Serial.printf("找到图像, 大小: %u 字节。准备接收...\n", image_size);
    uint8_t *imageBuffer = (uint8_t *)ps_malloc(image_size);
    if (!imageBuffer)
    {
      Serial.println("错误: PSRAM分配内存失败!");
      flushSerialBuffer(image_size + sizeof(END_MARKER));
      return;
    }
    size_t bytes_read = JPGSerial.readBytes(imageBuffer, image_size);
    if (bytes_read != image_size)
    {
      Serial.printf("错误: 图像接收不完整 (预期 %u, 实际 %u)。\n", image_size, bytes_read);
      free(imageBuffer);
      flushSerialBuffer(sizeof(END_MARKER));
      return;
    }
    uint8_t end_buffer[sizeof(END_MARKER)];
    if (JPGSerial.readBytes(end_buffer, sizeof(END_MARKER)) != sizeof(END_MARKER) ||
        memcmp(end_buffer, END_MARKER, sizeof(END_MARKER)) != 0)
    {
      Serial.println("警告: 未找到结束标记!");
      free(imageBuffer);
      return;
    }
    Serial.println("图像接收完毕, 正在处理...");

    Serial.println("正在进行Base64编码...");
    unsigned long startTime = millis();
    size_t encoded_len = 0;
    mbedtls_base64_encode(NULL, 0, &encoded_len, imageBuffer, image_size);
    
    // Base64 字符串放入 PSRAM，防止内存溢出
    char *base64_buf = (char *)ps_malloc(encoded_len);
    if (!base64_buf)
    {
      Serial.println("错误: Base64 PSRAM内存分配失败!");
      free(imageBuffer);
      return;
    }
    mbedtls_base64_encode((unsigned char *)base64_buf, encoded_len, &encoded_len, imageBuffer, image_size);
    free(imageBuffer); // 释放原始图片内存
    String imageBase64(base64_buf);
    free(base64_buf);  // 释放C字符串内存
    Serial.printf("Base64编码完成, 耗时: %lu ms\n", millis() - startTime);

    // 不阻塞等待 API，Base64 数据交由后台 AI 任务
    if (xSemaphoreTake(aiDataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      if (!isRecognizing) 
      {
        pendingImageBase64 = imageBase64;
        isRecognizing = true;
        Serial.println("图像已交由后台 AI 任务处理，系统恢复监听按键与语音...");
      }
      else 
      {
        Serial.println("警告: 上一张图像仍在识别中，忽略当前图像。");
      }
      xSemaphoreGive(aiDataMutex);
    }
  }
}

// ===================== 手动价格模式核心逻辑 =====================
void handleManualMode()
{
  delay(10); // 降低空转功耗
}

// =========================== 键盘输入处理 ===========================
void handleKeypadInput()
{
  char key = customKeypad.getKey();
  if (key)
  {
    Serial.printf("键盘按下: %c\n", key);

    if (key == 'A')
    {
      if (currentMode == AUTOMATIC_MODE)
      {
        currentMode = MANUAL_MODE;
        Serial.println("模式切换 -> 自定义价格模式");
      }
      else
      {
        currentMode = AUTOMATIC_MODE;
        Serial.println("模式切换 -> 自动识别模式");
      }
      if (xSemaphoreTake(displayDataMutex, portMAX_DELAY) == pdTRUE)
      {
        lastRecognizedItemName = "";
        lastUnitPrice = 0.0;
        manualPriceInput = "";
        xSemaphoreGive(displayDataMutex);
      }
      return;
    }

    if (key == 'B')
    {
      performTare();
      return;
    }

    if (currentMode == MANUAL_MODE)
    {
      if (isdigit(key))
      {
        if (manualPriceInput.length() < 6)
        {
          manualPriceInput += key;
        }
      }
      else if (key == 'C')
      {
        if (manualPriceInput.indexOf('.') == -1 && manualPriceInput.length() > 0)
        {
          manualPriceInput += '.';
        }
      }
      else if (key == '*')
      {
        manualPriceInput = "";
      }
      else if (key == 'D')
      {
        if (xSemaphoreTake(displayDataMutex, portMAX_DELAY) == pdTRUE)
        {
          lastUnitPrice = manualPriceInput.toFloat();
          lastRecognizedItemName = "自定义商品";
          manualPriceInput = "";
          xSemaphoreGive(displayDataMutex);
          Serial.printf("自定义价格已设定: %.2f 元/千克\n", lastUnitPrice);
        }
      }
    }
  }
}

// =========================== 语音指令处理 ===========================
void handleAsrInput()
{
  if (ASRSerial.available())
  {
    uint8_t command = ASRSerial.read(); 
    Serial.printf("收到语音指令代码: 0x%02X\n", command);

    switch (command)
    {
    case 0x01: 
      Serial.println("语音指令: 执行去皮。");
      performTare();
      break;

    case 0x02: 
      Serial.println("语音指令: 切换模式。");
      if (currentMode == AUTOMATIC_MODE)
      {
        currentMode = MANUAL_MODE;
        Serial.println("模式切换 -> 自定义价格模式");
      }
      else
      {
        currentMode = AUTOMATIC_MODE;
        Serial.println("模式切换 -> 自动识别模式");
      }
      if (xSemaphoreTake(displayDataMutex, portMAX_DELAY) == pdTRUE)
      {
        lastRecognizedItemName = "";
        lastUnitPrice = 0.0;
        manualPriceInput = "";
        xSemaphoreGive(displayDataMutex);
      }
      break;

    case 0x03: 
      if (currentMode == MANUAL_MODE && manualPriceInput.length() > 0)
      {
        Serial.println("语音指令: 确认价格。");
        if (xSemaphoreTake(displayDataMutex, portMAX_DELAY) == pdTRUE)
        {
          lastUnitPrice = manualPriceInput.toFloat();
          lastRecognizedItemName = "自定义商品";
          manualPriceInput = "";
          xSemaphoreGive(displayDataMutex);
          Serial.printf("自定义价格已设定: %.2f 元/千克\n", lastUnitPrice);
          ASRSerial.print("价格已确认"); // 语音反馈
        }
      }
      break;

    case 0x04: 
      Serial.println("语音指令: 请求当前种类。");
      if (xSemaphoreTake(displayDataMutex, portMAX_DELAY) == pdTRUE)
      {
        String name = lastRecognizedItemName;
        xSemaphoreGive(displayDataMutex);
        if (name.length() > 0)
        {
          ASRSerial.print("当前商品是" + name);
          Serial.println("已向ASR发送TTS播报: 当前商品是" + name);
        }
        else
        {
          ASRSerial.print("当前未识别到商品");
          Serial.println("已向ASR发送TTS播报: 当前未识别到商品");
        }
      }
      break;

    default:
      Serial.println("收到未知语音指令。");
      break;
    }
  }
}

// =========================== 去皮操作函数 ===========================
void performTare()
{
  Serial.println("'B'键按下，执行去皮操作...");
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(28, 38, "正在去皮...");
  u8g2.sendBuffer();

  scale.tare();
  global_weight = 0.0; // 去皮后重置全局重量


  Serial.println("去皮完成。");
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(34, 38, "去皮完成");
  u8g2.sendBuffer();
  delay(1000); // OLED 显示提示，短暂阻塞可接受
}
// =========================== LCD 显示任务 ===========================
// 后台高刷新率运行
void lcdDisplayTask(void *pvParameters)
{
  Serial.printf("LCD 显示任务已在核心 %d 上启动。\n", xPortGetCoreID());
  char line1[64], line2[32], line3[32];

  String itemNameForDisplay;
  float unitPriceForDisplay;
  String priceInputForDisplay;
  bool aiIsWorking = false;

  for (;;)
  {
    // 直接读取全局重量缓存，极快
    float currentWeight = global_weight;
    if (currentWeight < 1)
    {
      currentWeight = 0;
    }

    if (xSemaphoreTake(displayDataMutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
      itemNameForDisplay = lastRecognizedItemName;
      unitPriceForDisplay = lastUnitPrice;
      priceInputForDisplay = manualPriceInput;
      xSemaphoreGive(displayDataMutex);
    }
    
    // 检查 AI 是否正在后台识别
    if (xSemaphoreTake(aiDataMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      aiIsWorking = isRecognizing;
      xSemaphoreGive(aiDataMutex);
    }

    float totalPrice = (currentWeight / 1000.0) * unitPriceForDisplay;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);

    if (currentMode == AUTOMATIC_MODE)
    {
      if (aiIsWorking) 
      {
        strcpy(line1, "状态: 正在向AI识别...");
      }
      else if (itemNameForDisplay.length() > 0)
      {
        snprintf(line1, sizeof(line1), "%s/%.2f元/kg", itemNameForDisplay.c_str(), unitPriceForDisplay);
      }
      else
      {
        if ((xEventGroupGetBits(wifiEventGroup) & WIFI_CONNECTED_BIT) == 0)
        {
          strcpy(line1, "模式:自动-无网络!");
        }
        else
        {
          strcpy(line1, "模式:自动-请放置物品");
        }
      }
    }
    else
    { // MANUAL_MODE
      if (priceInputForDisplay.length() > 0)
      {
        snprintf(line1, sizeof(line1), "输入价格: %s_", priceInputForDisplay.c_str());
      }
      else if (unitPriceForDisplay > 0)
      {
        snprintf(line1, sizeof(line1), "%s/%.2f元/kg", itemNameForDisplay.c_str(), unitPriceForDisplay);
      }
      else
      {
        strcpy(line1, "模式:手动-请输入单价");
      }
    }

    snprintf(line2, sizeof(line2), "重量: %.0f g", currentWeight);
    snprintf(line3, sizeof(line3), "总价: %.2f 元", totalPrice);

    u8g2.drawUTF8(0, 14, line1);
    u8g2.drawUTF8(0, 38, line2);
    u8g2.drawUTF8(0, 60, line3);
    u8g2.sendBuffer();
    // 实时打包 JSON 数据发给摄像头屏幕
    String screenItemName = itemNameForDisplay;
    String screenStatus;
    
    if (aiIsWorking) {
        screenStatus = "AI识别中...";
    } else if (currentMode == AUTOMATIC_MODE) {
        screenStatus = "自动模式";
    } else {
        screenStatus = "手动模式";
    }
    
    // 商品名称为空时，显示友好提示
    if (screenItemName.length() == 0) {
      if (aiIsWorking) screenItemName = "AI云端识别中...";
      else if (currentMode == MANUAL_MODE) screenItemName = "手动计价模式";
      else screenItemName = "等待拍照识别...";
    }

    // 检查 HC-SR501 状态，控制休眠
    static int last_pir_state = LOW;
    int current_pir_state = digitalRead(PIR_PIN);
    
    if (current_pir_state != last_pir_state) {
        Serial.printf("[PIR] HC-SR501 状态改变: %s\n", current_pir_state == HIGH ? "有人 (HIGH)" : "无人 (LOW)");
        last_pir_state = current_pir_state;
    }

    if (current_pir_state == HIGH) {
      last_motion_time = millis();
      if (is_sleeping) {
          Serial.println("[PIR] 唤醒屏幕");
      }
      is_sleeping = false;
    } else if (!is_sleeping && (millis() - last_motion_time > 60000)) {
      Serial.println("[PIR] 60秒无人，触发休眠");
      is_sleeping = true;
    }

    // 注意结尾需加 \n，摄像头板按换行拆包
    char screenJson[256];
    snprintf(screenJson, sizeof(screenJson),
             "{\"status\":\"%s\",\"item\":\"%s\",\"weight\":%.0f,\"price\":%.2f,\"total\":%.2f,\"input\":\"%s\",\"sleep\":%d}\n", 
             screenStatus.c_str(), screenItemName.c_str(), currentWeight, unitPriceForDisplay, totalPrice, priceInputForDisplay.c_str(), is_sleeping ? 1 : 0);
    
    static String lastSentJson = "";
    static unsigned long lastSendTime = 0;
    String currentJson = String(screenJson);
    
    // 数据变化 或 每隔1秒 定时发送，防止摄像头端超时导致屏幕闪退
    if (currentJson != lastSentJson || (millis() - lastSendTime > 1000)) {
      if (millis() > 6000) { // 开机前6秒不通过TX发送数据，防止干扰屏幕端开机
        JPGSerial.print(screenJson);
      }
      lastSentJson = currentJson;
      lastSendTime = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // 10Hz 刷新率，画面流畅
  }
}

// =========================== WiFi 管理任务（核心 0） ===========================
// 后台运行
void wifiManagementTask(void *pvParameters)
{
  Serial.printf("WiFi 管理任务已在核心 %d 上启动。\n", xPortGetCoreID());
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() > 30000 && WiFi.status() != WL_CONNECTED)
    {
      Serial.println("启动30秒后WiFi仍未连接，将继续在后台尝试...");
      break; 
    }
    Serial.print(".");
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nWiFi 首次连接成功!");
  }

  for (;;)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      xEventGroupSetBits(wifiEventGroup, WIFI_CONNECTED_BIT);
      setLED(2);
    }
    else
    {
      xEventGroupClearBits(wifiEventGroup, WIFI_CONNECTED_BIT);
      Serial.println("\n WiFi 连接已断开, 正在尝试重连...");
      setLED(1);
      WiFi.disconnect();
      WiFi.reconnect();
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// =========================== AI 后台识别任务 ===========================
// 彻底释放主循环
void aiRecognitionTask(void *pvParameters)
{
  Serial.printf("AI 异步识别任务已在核心 %d 上启动。\n", xPortGetCoreID());
  for (;;)
  {
    String localBase64 = "";
    
    // 检查是否有等待处理的图片
    if (xSemaphoreTake(aiDataMutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
      if (isRecognizing && pendingImageBase64.length() > 0)
      {
        localBase64 = pendingImageBase64; // 拷贝到本地
        pendingImageBase64 = "";          // 立即清空全局缓存释放内存
      }
      xSemaphoreGive(aiDataMutex);
    }

    if (localBase64.length() > 0)
    {
      Serial.println("后台 AI 任务: 正在发送图像至 Qwen 进行识别...");
      unsigned long startTime = millis();
      
      // 调用识别 API
      String recognizedName = recognizeIngredient(localBase64);
      
      Serial.printf("后台 AI 任务: 识别流程完成, 耗时: %lu ms\n", millis() - startTime);

      if (recognizedName.length() > 0)
      {
        if (xSemaphoreTake(displayDataMutex, portMAX_DELAY) == pdTRUE)
        {
          lastRecognizedItemName = recognizedName;
          lastUnitPrice = priceList.count(recognizedName) ? priceList[recognizedName].toFloat() : 0.0;
          xSemaphoreGive(displayDataMutex);
        }
        Serial.printf("\n--- 识别结果 ---\n  商品: %s\n  单价: %.2f 元/千克\n----------------\n", recognizedName.c_str(), lastUnitPrice);
      }
      else
      {
        Serial.println("识别失败或API未返回有效结果。");
      }

      // 重置识别状态
      if (xSemaphoreTake(aiDataMutex, portMAX_DELAY) == pdTRUE)
      {
        isRecognizing = false;
        xSemaphoreGive(aiDataMutex);
      }
      Serial.println("--- 系统准备就绪，等待下一张图片... ---");
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // 闲置时休眠，不占CPU
  }
}

// =========================== 双路 MQTT 数据发送任务 ===========================
// 发往 OneNet 与小程序
void oneNetDataTask(void *pvParameters)
{
  Serial.printf("双路 MQTT 发送任务已在核心 %d 上启动。\n", xPortGetCoreID());

  String currentItem;
  float currentPrice = 0.0;
  float currentWeight = 0.0;
  float currentTotal = 0.0;
  char jsonBuffer[1024];
  
  String replyTopic = String(onenet_topic_post) + "/reply";

  for (;;)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      // 1. 维护 OneNet 连接
      if (!mqttClientOneNet.connected())
      {
        if (mqttClientOneNet.connect(onenet_device_id, onenet_product_id, onenet_token))
        {
          mqttClientOneNet.subscribe(replyTopic.c_str());
        }
      }

      // 2. 维护 公共 EMQX 连接
      if (!mqttClientPublic.connected())
      {
        mqttClientPublic.connect(public_client_id);
      }

      // 3. 获取数据并打包为 JSON
      currentWeight = global_weight; // 使用内存中的重量，不阻塞
      if (currentWeight < 1) currentWeight = 0;

      if (xSemaphoreTake(displayDataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
      {
        currentItem = lastRecognizedItemName;
        currentPrice = lastUnitPrice;
        xSemaphoreGive(displayDataMutex);
      }
      else
      {
        currentItem = "Unknown";
      }

      if (currentItem.length() == 0) currentItem = "None";
      currentTotal = (currentWeight / 1000.0) * currentPrice;

      snprintf(jsonBuffer, sizeof(jsonBuffer),
               "{\"id\": \"%lu\", \"version\": \"1.0\", \"params\": {"
               "\"Weight\": { \"value\": %.2f },"    
               "\"UnitPrice\": { \"value\": %.2f }," 
               "\"TotalCost\": { \"value\": %.2f }," 
               "\"ItemName\": { \"value\": \"%s\" }" 
               "}}",
               millis(), currentWeight, currentPrice, currentTotal, currentItem.c_str());

      // 4. 双路同时发送数据
      if (mqttClientOneNet.connected()) {
        mqttClientOneNet.publish(onenet_topic_post, jsonBuffer);
        mqttClientOneNet.loop();
      }

      if (mqttClientPublic.connected()) {
        mqttClientPublic.publish(public_topic_post, jsonBuffer);
        mqttClientPublic.loop();
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// =========================== 辅助函数实现 ===========================

void flushSerialBuffer(size_t count)
{
  uint32_t start_time = millis();
  size_t flushed_count = 0;
  while (flushed_count < count && millis() - start_time < 2000)
  {
    if (JPGSerial.available())
    {
      JPGSerial.read();
      flushed_count++;
    }
  }
}

bool findStartMarker()
{
  int bytes_matched = 0;
  while (JPGSerial.available())
  {
    uint8_t byte_in = JPGSerial.read();
    if (byte_in == START_MARKER[bytes_matched])
    {
      bytes_matched++;
      if (bytes_matched == sizeof(START_MARKER))
      {
        return true;
      }
    }
    else
    {
      bytes_matched = (byte_in == START_MARKER[0]) ? 1 : 0;
    }
  }
  return false;
}

void loadPriceList()
{
  Serial.println("\n--- 正在加载价目表 ---");
  File file = SD.open("/pricelist.csv", FILE_READ);
  if (!file)
  {
    Serial.println("错误: 打开 /pricelist.csv 文件失败!");
    return;
  }
  while (file.available())
  {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
      continue;
    int commaIndex = line.indexOf(',');
    if (commaIndex != -1)
    {
      String name = line.substring(0, commaIndex);
      String price = line.substring(commaIndex + 1);
      priceList[name] = price;
    }
  }
  file.close();
  Serial.printf("价目表加载完毕。共加载了 %d 个条目。\n", priceList.size());
}

// =========================== Qwen 视觉识别 API 调用 ===========================
// 参数采用常量引用，避免大字符串内存溢出
String recognizeIngredient(const String& base64Image) 
{
  Serial.println(">>> 进入 recognizeIngredient 函数");
  Serial.printf(">>> 当前可用堆内存 (Heap): %d bytes\n", ESP.getFreeHeap());
  Serial.printf(">>> 当前可用 PSRAM: %d bytes\n", ESP.getFreePsram());
  Serial.printf(">>> 待发送的 Base64 图像长度: %d 字节\n", base64Image.length());

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("错误(recognizeIngredient): WiFi未连接!");
    return "";
  }
  HTTPClient http;
  String itemName = "";
  
  http.begin(apiUrl);
  http.setTimeout(30000); // 视觉大模型推理时间较长，设置30秒超时
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + qwenApiKey);

  // 拼接 JSON Payload 
  Serial.println(">>> 正在拼接 JSON Payload...");
  String payload;
  payload.reserve(base64Image.length() + 512); // 避免内存碎片
  
  // Prompt (提示词)
  payload = "{\"model\":\"qwen-vl-plus\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"你是一个电子秤的视觉识别程序。请识别图片中的果蔬或农产品。请仅输出确切的名称（如：苹果、土豆、胡萝卜），绝不要输出任何多余的字符、标点符号或解释说明。如果无法识别或不是常见食材，请直接输出'非果蔬食材'。\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
  payload += base64Image;
  payload += "\"}}]}]}";

  Serial.printf(">>> Payload 拼接完成，总长度: %d 字节\n", payload.length());
  Serial.println(">>> 正在发送 POST 请求至 Qwen API...");

  int httpCode = http.POST(payload);
  
  Serial.printf(">>> POST 请求结束，HTTP 状态码: %d\n", httpCode);

  if (httpCode == HTTP_CODE_OK)
  {
    String response = http.getString();
    Serial.println(">>> 收到服务器响应 (OK)，原始数据长度: " + String(response.length()));
    Serial.println(">>> 响应内容片段: " + response.substring(0, 150) + "..."); // 打印前面一部分看看
    
    // 【优化】使用 1024 大小足以解析我们需要提取的 Qwen 返回的极简结构，不用消耗过多堆栈
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, response);
    
    if (error)
    {
      Serial.print(F(">>> JSON解析失败: "));
      Serial.println(error.c_str());
      http.end();
      return "";
    }
    
    if (doc.containsKey("choices"))
    {
      itemName = doc["choices"][0]["message"]["content"].as<String>();
      itemName.trim(); // 移除前后的空格或换行符
      Serial.printf(">>> Qwen 识别成功: [%s]\n", itemName.c_str());
    }
    else if (doc.containsKey("error"))
    {
      Serial.print(">>> API 返回错误: ");
      Serial.println(doc["error"]["message"].as<String>());
    }
    else 
    {
      Serial.println(">>> 未知错误：JSON 未包含 choices 也没有 error 字段！");
      Serial.println(">>> 完整 JSON 数据: " + response);
    }
  }
  else if (httpCode < 0)
  {
    Serial.printf(">>> 请求失败！HTTPClient 内部错误代码: %d\n", httpCode);
    Serial.printf(">>> 错误含义: %s\n", http.errorToString(httpCode).c_str());
  }
  else
  {
    Serial.printf(">>> 服务器返回非正常 HTTP 状态码: %d\n", httpCode);
    Serial.println(">>> 错误详情: " + http.getString());
  }
  http.end();
  
  // 安全过滤：防止大模型偶尔输出带标点符号的结尾
  itemName.replace("。", "");
  itemName.replace("！", "");
  
  Serial.printf(">>> recognizeIngredient 返回的结果: [%s]\n", itemName.c_str());
  return itemName;
}

void setLED(int color) {
  onboardLED.clear();
  if (color == 1) { // 红色 (未联网)
    onboardLED.setPixelColor(0, onboardLED.Color(100, 0, 0));
  } else if (color == 2) { // 绿色 (已联网)
    onboardLED.setPixelColor(0, onboardLED.Color(0, 100, 0));
  }
  onboardLED.show();
}