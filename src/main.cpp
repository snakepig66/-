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

// =================================================================
// ==                      网络与API配置                           ==
// =================================================================
const char *ssid = "waveshare";    // 你的WiFi SSID
const char *password = "12345678"; // 你的WiFi密码

// 百度AI开放平台 API Key
String apiKey = "9kp0pGBQbQpOQqEfLAnYWOmy";
String secretKey = "q95IU4v99OMrZvdj9NHXbiOZ9LLVYXTn";

// API端点URL
const char *tokenUrl = "https://aip.baidubce.com/oauth/2.0/token";
const char *apiUrl = "https://aip.baidubce.com/rest/2.0/image-classify/v1/classify/ingredient";

// =================================================================
// ==                     OneNet 配置                      ==
// =================================================================
const char *mqtt_server = "mqtts.heclouds.com"; // OneNet Studio地址
const int mqtt_port = 1883;

// OneNet鉴权信息
const char *onenet_product_id = "96LmRjv77N";
const char *onenet_device_id = "niu";
const char *onenet_token = "version=2018-10-31&res=products%2F96LmRjv77N%2Fdevices%2Fniu&et=2000000000&method=md5&sign=fZy20DseTrk7WWVOOqh9GA%3D%3D"; // 或者是密码/AccessKey

// OneNet 物模型上报 Topic
const char *onenet_topic_post = "$sys/96LmRjv77N/niu/thing/property/post";

// 初始化 WiFiClient 和 PubSubClient
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// =================================================================
// ==                       硬件引脚配置                           ==
// =================================================================
#define JPG_SERIAL_RX_PIN 18 // 图像串口接收引脚 (连接到图像发送模块的TX)
#define JPG_SERIAL_TX_PIN 17 // 图像串口发送引脚 (连接到图像发送模块的RX)
#define SD_CS_PIN 10         // SD卡片选引脚
#define SD_MOSI_PIN 11       // SD卡MOSI引脚
#define SD_MISO_PIN 13       // SD卡MISO引脚
#define SD_SCK_PIN 12        // SD卡时钟引脚
#define HX711_DOUT_PIN 4     // HX711数据输出引脚
#define HX711_SCK_PIN 5      // HX711时钟引脚
#define RED_PIN 42
#define GREEN_PIN 2
#define BLUE_PIN 1
#define GND_PIN 41
const byte KEYPAD_ROWS = 4;
const byte KEYPAD_COLS = 4;
char keys[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};

byte rowPins[KEYPAD_ROWS] = {47, 21, 20, 19};
byte colPins[KEYPAD_COLS] = {16, 15, 7, 6};

// =================================================================
// ==                       软件与协议配置                         ==
// =================================================================
enum SystemMode
{
  AUTOMATIC_MODE,
  MANUAL_MODE
};
SystemMode currentMode = MANUAL_MODE;

const uint8_t START_MARKER[] = {0xAB, 0xCD, 0xEF};
const uint8_t END_MARKER[] = {0xFE, 0xDC, 0xBA};
float calibration_factor = 460;

// =================================================================
// ==                   FreeRTOS 任务与同步句柄                    ==
// =================================================================
TaskHandle_t wifiManagementTaskHandle;
TaskHandle_t oledDisplayTaskHandle;
TaskHandle_t oneNetTaskHandle;
EventGroupHandle_t wifiEventGroup;
SemaphoreHandle_t displayDataMutex;
const int WIFI_CONNECTED_BIT = BIT0;

// =================================================================
// ==                      全局对象与变量                          ==
// =================================================================

HardwareSerial ASRSerial(2);
#define ASR_RX_PIN 45
#define ASR_TX_PIN 48

HardwareSerial JPGSerial(1);
SPIClass SDSPI(HSPI);
HX711 scale;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
Keypad customKeypad = Keypad(makeKeymap(keys), rowPins, colPins, KEYPAD_ROWS, KEYPAD_COLS);

std::map<String, String> priceList;
String lastRecognizedItemName = "";
float lastUnitPrice = 0.0;
String manualPriceInput = "";

String cachedAccessToken = "";
unsigned long lastTokenTime = 0;
const unsigned long tokenValidityPeriod = 24 * 3600 * 1000UL;

// =================================================================
// ==                         函数声明                             ==
// =================================================================
void wifiManagementTask(void *pvParameters);
void oledDisplayTask(void *pvParameters);
void oneNetDataTask(void *pvParameters);
String getAccessToken();
String urlEncode(String str);
String recognizeIngredient(String accessToken, String base64Image);
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

// =================================================================
// ==                        主程序 Setup                          ==
// =================================================================
void setup()
{
  Serial.begin(115200);
  while (!Serial)
    ;
  Serial.println("\n\n--- ESP32-S3 双模式智能计价系统 ---");
  Serial.printf("主程序 setup() 运行在核心: %d\n", xPortGetCoreID());

  ASRSerial.begin(9600, SERIAL_8N1, ASR_RX_PIN, ASR_TX_PIN);
  Serial.println("ASRPRO 语音识别模块串口已在 GPIO 9/8 上初始化。");

  pinMode(GND_PIN, OUTPUT);
  digitalWrite(GND_PIN, LOW);
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);
  setLED(3); // 蓝色灯表示启动中

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

  JPGSerial.begin(921600, SERIAL_8N1, JPG_SERIAL_RX_PIN, JPG_SERIAL_TX_PIN);

  Serial.println("正在初始化 HX711 称重模块...");
  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
  scale.set_scale(calibration_factor);
  scale.tare();
  Serial.println("HX711 初始化完成，已去皮。");

  displayDataMutex = xSemaphoreCreateMutex();
  wifiEventGroup = xEventGroupCreate();
  // 【新增】配置MQTT服务器参数
  mqttClient.setServer(mqtt_server, mqtt_port);
  // 【新增】增大MQTT缓冲区，防止JSON数据过长导致发送失败
  mqttClient.setBufferSize(512);
  // Wifi管理任务会在后台自动连接，主程序无需等待
  xTaskCreatePinnedToCore(wifiManagementTask, "WiFiTask", 4096, NULL, 1, &wifiManagementTaskHandle, 0);
  xTaskCreatePinnedToCore(oledDisplayTask, "OLEDTask", 4096, NULL, 2, &oledDisplayTaskHandle, 0);
  xTaskCreatePinnedToCore(oneNetDataTask, "OneNetTask", 4096, NULL, 1, &oneNetTaskHandle, 1);

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
  Serial.println("正在尝试获取初始 Access Token...");
  // 延迟一小段时间，给WiFi连接的机会
  vTaskDelay(pdMS_TO_TICKS(3000));

  if (WiFi.status() == WL_CONNECTED)
  {
    cachedAccessToken = getAccessToken();
    if (cachedAccessToken.length() > 0)
    {
      lastTokenTime = millis();
      Serial.println("Access Token 获取成功!");
    }
    else
    {
      Serial.println("警告: 无法获取 Access Token，自动模式可能不可用。");
    }
  }
  else
  {
    Serial.println("警告: WiFi未连接，无法获取Access Token，自动模式暂不可用。");
  }

  Serial.println("\n--- 系统初始化完成，当前为手动模式 ---");
}

// =================================================================
// ==   主循环: 负责键盘检测与模式逻辑分发                        ==
// =================================================================
void loop()
{
  handleKeypadInput();
  handleAsrInput();
  if (currentMode == AUTOMATIC_MODE)
  {
    handleAutomaticMode();
  }
  else
  {
    handleManualMode();
  }
}

// =================================================================
// == 自动识别模式下的核心逻辑                              ==
// =================================================================
void handleAutomaticMode()
{
  // 在自动模式开始时，检查WiFi和Token
  if ((xEventGroupGetBits(wifiEventGroup) & WIFI_CONNECTED_BIT) == 0)
  {
    Serial.println("自动模式错误: WiFi 未连接!");
    // 可以在OLED上显示错误信息
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    u8g2.drawUTF8(10, 38, "错误: WiFi未连接!");
    u8g2.sendBuffer();
    delay(2000);
    // 自动切回手动模式，避免卡死
    currentMode = MANUAL_MODE;
    return;
  }

  if (findStartMarker())
  {
    // ... (此处省略未修改的图像处理代码)
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
    char *base64_buf = (char *)malloc(encoded_len);
    if (!base64_buf)
    {
      Serial.println("错误: Base64内存分配失败!");
      free(imageBuffer);
      return;
    }
    mbedtls_base64_encode((unsigned char *)base64_buf, encoded_len, &encoded_len, imageBuffer, image_size);
    free(imageBuffer);
    String imageBase64(base64_buf);
    free(base64_buf);
    Serial.printf("Base64编码完成, 耗时: %lu ms\n", millis() - startTime);

    if (millis() - lastTokenTime > tokenValidityPeriod || cachedAccessToken.length() == 0)
    {
      Serial.println("正在刷新或获取 Access Token...");
      String newAccessToken = getAccessToken();
      if (newAccessToken.length() > 0)
      {
        cachedAccessToken = newAccessToken;
        lastTokenTime = millis();
        Serial.println("Access Token 获取成功!");
      }
      else
      {
        Serial.println("错误: Access Token 获取失败。无法进行识别。");
        // 可以在OLED上显示错误
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_wqy12_t_gb2312);
        u8g2.drawUTF8(10, 38, "错误: Token获取失败");
        u8g2.sendBuffer();
        delay(2000);
        currentMode = MANUAL_MODE; // 切回手动模式
        return;
      }
    }

    Serial.println("正在发送图像进行识别...");
    startTime = millis();
    String recognizedName = recognizeIngredient(cachedAccessToken, imageBase64);
    Serial.printf("识别流程完成, 耗时: %lu ms\n", millis() - startTime);

    if (recognizedName.length() > 0)
    {
      if (xSemaphoreTake(displayDataMutex, portMAX_DELAY) == pdTRUE)
      {
        lastRecognizedItemName = recognizedName;
        lastUnitPrice = priceList.count(recognizedName) ? priceList[recognizedName].toFloat() : 0.0;
        xSemaphoreGive(displayDataMutex);
      }
      Serial.printf("\n--- 识别结果 ---\n==================================\n  商品: %s\n  单价: %.2f 元/千克\n==================================\n", recognizedName.c_str(), lastUnitPrice);
    }
    else
    {
      Serial.println("识别失败或API未返回有效结果。");
    }
    Serial.println("\n--- 系统准备就绪，等待下一张图片... ---");
  }
}

// =================================================================
// == 自定义价格模式下的核心逻辑                              ==
// =================================================================
void handleManualMode()
{
  delay(20);
}

// =================================================================
// == 处理键盘输入                                         ==
// =================================================================
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

// =================================================================
// == 执行去皮操作的函数                                   ==
// =================================================================
void performTare()
{
  Serial.println("'B'键按下，执行去皮操作...");
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(28, 38, "正在去皮...");
  u8g2.sendBuffer();

  scale.tare();

  Serial.println("去皮完成。");
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(34, 38, "去皮完成");
  u8g2.sendBuffer();
  delay(1000);
}

// =================================================================
// == OLED显示任务 (后台运行)  ==
// =================================================================
void oledDisplayTask(void *pvParameters)
{
  Serial.printf("OLED 显示任务已在核心 %d 上启动。\n", xPortGetCoreID());
  char line1[64], line2[32], line3[32], modeStr[32];

  String itemNameForDisplay;
  float unitPriceForDisplay;
  String priceInputForDisplay;

  for (;;)
  {
    float currentWeight = getWeight();
    if (currentWeight < 1)
    {
      currentWeight = 0;
    }

    if (xSemaphoreTake(displayDataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      itemNameForDisplay = lastRecognizedItemName;
      unitPriceForDisplay = lastUnitPrice;
      priceInputForDisplay = manualPriceInput;
      xSemaphoreGive(displayDataMutex);
    }

    float totalPrice = (currentWeight / 1000.0) * unitPriceForDisplay;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);

    if (currentMode == AUTOMATIC_MODE)
    {
      if (itemNameForDisplay.length() > 0)
      {
        snprintf(line1, sizeof(line1), "%s/%.2f元/kg", itemNameForDisplay.c_str(), unitPriceForDisplay);
      }
      else
      {
        // 在自动模式下，如果WiFi断开，也给提示
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

    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

// =================================================================
// ==         核心0 (Core 0) WiFi管理任务 (后台运行)              ==
// =================================================================
void wifiManagementTask(void *pvParameters)
{
  Serial.printf("WiFi 管理任务已在核心 %d 上启动。\n", xPortGetCoreID());
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    // 启动时非阻塞地尝试连接
    if (millis() > 30000 && WiFi.status() != WL_CONNECTED)
    {
      Serial.println("启动30秒后WiFi仍未连接，将继续在后台尝试...");
      break; // 30秒后不再阻塞，让主程序继续
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
      if (currentMode == AUTOMATIC_MODE)
        setLED(2);
      else
        setLED(3);
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

// =================================================================
// ==         【修正版】OneNet 数据发送任务 (已修复JSON格式错误)       ==
// =================================================================
void oneNetDataTask(void *pvParameters)
{
  Serial.printf("OneNet 任务已在核心 %d 上启动。\n", xPortGetCoreID());

  // 临时变量用于存储要发送的数据
  String currentItem;
  float currentPrice = 0.0;
  float currentWeight = 0.0;
  float currentTotal = 0.0;

  // 缓冲区足够大，防止JSON截断
  char jsonBuffer[1024];

  // 定义应答 Topic (用于接收平台回复，消除 subscription not exist 警告)
  String replyTopic = String(onenet_topic_post) + "/reply";

  for (;;)
  {
    // 1. 检查WiFi是否连接
    if (WiFi.status() == WL_CONNECTED)
    {

      // 2. 检查MQTT连接
      if (!mqttClient.connected())
      {
        Serial.print("尝试连接 OneNet MQTT...");
        // 尝试连接
        if (mqttClient.connect(onenet_device_id, onenet_product_id, onenet_token))
        {
          Serial.println("连接成功!");
          // 连接成功后，务必订阅应答 Topic
          mqttClient.subscribe(replyTopic.c_str());
          Serial.println("已订阅应答 Topic: " + replyTopic);
        }
        else
        {
          Serial.print("失败, rc=");
          Serial.print(mqttClient.state());
          Serial.println(" 3秒后重试");
          vTaskDelay(pdMS_TO_TICKS(3000));
          continue;
        }
      }

      // 3. 安全地获取数据 (加锁防止读取时数据突变)
      currentWeight = getWeight();
      if (currentWeight < 1)
        currentWeight = 0;

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

      // 防止商品名为空导致 JSON 格式错误
      if (currentItem.length() == 0)
        currentItem = "None";

      currentTotal = (currentWeight / 1000.0) * currentPrice;

      // 4. 构造 JSON (符合 OneNet Studio 物模型标准)
      // 注意：必须使用 {"value": 值} 的嵌套结构，否则报错 302
      snprintf(jsonBuffer, sizeof(jsonBuffer),
               "{\"id\": \"%lu\", \"version\": \"1.0\", \"params\": {"
               "\"Weight\": { \"value\": %.2f },"    // 数值型，保留2位小数
               "\"UnitPrice\": { \"value\": %.2f }," // 数值型
               "\"TotalCost\": { \"value\": %.2f }," // 数值型
               "\"ItemName\": { \"value\": \"%s\" }" // 字符串型，值需要引号
               "}}",
               millis(),
               currentWeight,
               currentPrice,
               currentTotal,
               currentItem.c_str());

      // 调试打印：在串口监视器检查生成的 JSON 是否包含 "value"
      Serial.print("发送OneNet JSON: ");
      Serial.println(jsonBuffer);

      // 5. 发布消息
      if (mqttClient.publish(onenet_topic_post, jsonBuffer))
      {
        // 发送成功时不刷屏，失败时提示即可
      }
      else
      {
        Serial.println("发送失败 (可能是包过大或连接断开)");
      }

      // 保持 MQTT 心跳
      mqttClient.loop();
    }

    // 发送频率：1秒一次
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
/*************************************************************************************************/
//                             辅助函数实现 (无重大修改)
/*************************************************************************************************/
float getWeight()
{
  int retryCount = 0;
  while (retryCount < 5 && !scale.is_ready())
  {
    delayMicroseconds(500);
    retryCount++;
  }
  if (retryCount >= 5)
  {
    return 0.0;
  }
  return scale.get_units(10);
}
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
String getAccessToken()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("错误(getAccessToken): WiFi未连接!");
    return "";
  }
  HTTPClient http;
  String accessToken = "";
  String postData = "grant_type=client_credentials&client_id=" + apiKey + "&client_secret=" + secretKey;
  http.begin(tokenUrl);
  http.addHeader("Content-Type", "application/x-w-form-urlencoded");
  int httpCode = http.POST(postData);
  if (httpCode == HTTP_CODE_OK)
  {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    accessToken = doc["access_token"].as<String>();
  }
  else
  {
    Serial.printf("[HTTP] 获取Token失败, HTTP代码: %d, 错误: %s\n", httpCode, http.errorToString(httpCode).c_str());
  }
  http.end();
  return accessToken;
}
String recognizeIngredient(String accessToken, String base64Image)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("错误(recognizeIngredient): WiFi未连接!");
    return "";
  }
  HTTPClient http;
  String itemName = "";
  String requestUrl = String(apiUrl) + "?access_token=" + accessToken;
  String postData = "image=" + urlEncode(base64Image);
  http.begin(requestUrl);
  http.setTimeout(20000);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpCode = http.POST(postData);
  if (httpCode == HTTP_CODE_OK)
  {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if (error)
    {
      Serial.print(F("JSON解析失败: "));
      Serial.println(error.c_str());
      http.end();
      return "";
    }
    if (doc.containsKey("result"))
    {
      JsonArray results = doc["result"].as<JsonArray>();
      JsonObject finalResult;
      if (results.size() > 0)
      {
        JsonObject firstResult = results[0];
        if (firstResult["name"] == "非果蔬食材" && results.size() > 1)
        {
          finalResult = results[1];
        }
        else
        {
          finalResult = firstResult;
        }
        itemName = finalResult["name"].as<String>();
        Serial.printf("识别成功: [%s] (置信度: %.1f%%)\n", itemName.c_str(), finalResult["score"].as<double>() * 100);
      }
      else
      {
        Serial.println("API返回结果为空。");
      }
    }
    else if (doc.containsKey("error_msg"))
    {
      Serial.print("API 返回错误: ");
      Serial.println(doc["error_msg"].as<String>());
    }
    else
    {
      Serial.println("API响应格式错误。");
    }
  }
  else
  {
    Serial.printf("服务器返回错误, HTTP状态码: %d\n", httpCode);
    Serial.println("错误详情: " + http.getString());
  }
  http.end();
  return itemName;
}
String urlEncode(String str)
{
  String encodedString = "";
  encodedString.reserve(str.length() * 1.1);
  for (char c : str)
  {
    if (isalnum(c))
    {
      encodedString += c;
    }
    else if (c == '+')
    {
      encodedString += "%2B";
    }
    else if (c == '/')
    {
      encodedString += "%2F";
    }
    else if (c == '=')
    {
      encodedString += "%3D";
    }
    else
    {
      char hex[4];
      sprintf(hex, "%%%02X", c);
      encodedString += hex;
    }
  }
  return encodedString;
}
void setLED(int color)
{
  digitalWrite(RED_PIN, LOW);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(BLUE_PIN, LOW);

  if (color == 1)
  {
    digitalWrite(RED_PIN, HIGH);
  }
  else if (color == 2)
  {
    digitalWrite(GREEN_PIN, HIGH);
  }
  else if (color == 3)
  {
    digitalWrite(BLUE_PIN, HIGH);
  }
}
// =================================================================
// ==              处理ASRPRO语音指令                           ==
// =================================================================
void handleAsrInput()
{
  if (ASRSerial.available())
  {
    uint8_t command = ASRSerial.read(); // 读取指令
    Serial.printf("收到语音指令代码: 0x%02X\n", command);

    switch (command)
    {
    case 0x01: // 假设 0x01 代表 "去皮"
      Serial.println("语音指令: 执行去皮。");
      performTare();
      break;

    case 0x02: // 假设 0x02 代表 "切换模式"
      Serial.println("语音指令: 切换模式。");
      // 这段逻辑与按下 'A' 键完全相同
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

    case 0x03: // 假设 0x03 代表 "确认" (用于手动模式)
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
        }
      }
      break;

      // 你可以根据ASRPRO设置的词条添加更多 case
      // 例如：case 0x04: handlePriceQuery("苹果"); break;

    default:
      Serial.println("收到未知语音指令。");
      break;
    }
  }
}
