// =================================================================
// ==      摄像头 & LCD 一体化主控面板 - 纯显示+动态闪烁光标版      ==
// =================================================================
#include "esp_camera.h"
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>

// 声明中文字体
LV_FONT_DECLARE(font_123);
LV_IMG_DECLARE(boot_logo);

// --- HARDWARE PINS ---
#define PWDN_GPIO_NUM 17   
#define RESET_GPIO_NUM -1  
#define XCLK_GPIO_NUM 8
#define SIOD_GPIO_NUM 21 
#define SIOC_GPIO_NUM 16
#define Y9_GPIO_NUM 2
#define Y8_GPIO_NUM 7
#define Y7_GPIO_NUM 10
#define Y6_GPIO_NUM 14
#define Y5_GPIO_NUM 11
#define Y4_GPIO_NUM 15
#define Y3_GPIO_NUM 13
#define Y2_GPIO_NUM 12
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 4
#define PCLK_GPIO_NUM 9

// LCD Pins
#define EXAMPLE_PIN_NUM_LCD_SCLK 39
#define EXAMPLE_PIN_NUM_LCD_MOSI 38
#define EXAMPLE_PIN_NUM_LCD_MISO 40
#define EXAMPLE_PIN_NUM_LCD_DC 42
#define EXAMPLE_PIN_NUM_LCD_RST -1
#define EXAMPLE_PIN_NUM_LCD_CS 45
#define EXAMPLE_PIN_NUM_LCD_BL 1

#define BUTTON_PIN 18         // 物理按键(用于拍照)
#define HOST_TX_PIN 43        
#define HOST_RX_PIN 44        
// --- END HARDWARE PINS ---

// --- DISPLAY & LVGL SETUP ---
#define EXAMPLE_LCD_ROTATION 3 
#define EXAMPLE_LCD_H_RES 240
#define EXAMPLE_LCD_V_RES 320
Arduino_DataBus *bus = new Arduino_ESP32SPI(EXAMPLE_PIN_NUM_LCD_DC, EXAMPLE_PIN_NUM_LCD_CS, EXAMPLE_PIN_NUM_LCD_SCLK, EXAMPLE_PIN_NUM_LCD_MOSI, EXAMPLE_PIN_NUM_LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(bus, EXAMPLE_PIN_NUM_LCD_RST, EXAMPLE_LCD_ROTATION, true, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
uint32_t screenWidth;
uint32_t screenHeight;
lv_disp_draw_buf_t draw_buf;
lv_color_t *disp_draw_buf;
lv_disp_drv_t disp_drv;
static SemaphoreHandle_t lvgl_api_mux = NULL;

// --- LVGL UI OBJECTS ---
lv_obj_t *label_status;
lv_obj_t *label_item;
lv_obj_t *label_weight;
lv_obj_t *label_price;
lv_obj_t *label_total;

// --- 光标闪烁相关的全局变量 ---
String current_item = "--";
String current_input = "";
bool cursor_state = false;

// --- SERIAL COMMUNICATION ---
HardwareSerial HostSerial(1); 
const uint8_t START_MARKER[] = {0xAB, 0xCD, 0xEF};
const uint8_t END_MARKER[] = {0xFE, 0xDC, 0xBA};

// =================================================================
// ==                     LVGL & GFX Helpers                      ==
// =================================================================
bool lvgl_lock(int timeout_ms) { return xSemaphoreTakeRecursive(lvgl_api_mux, (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE; }
void lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_api_mux); }
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
  lv_disp_flush_ready(disp_drv);
}

// =================================================================
// ==                      光标闪烁定时器回调                      ==
// =================================================================
static void blink_timer_cb(lv_timer_t * timer) {
    cursor_state = !cursor_state; // 翻转光标状态
    
    // 定时器属于 LVGL 内部任务，这里不需要加锁
    if (current_input.length() > 0) {
        if (cursor_state) {
            lv_label_set_text_fmt(label_item, "输入单价: %s_", current_input.c_str());
        } else {
            lv_label_set_text_fmt(label_item, "输入单价: %s ", current_input.c_str());
        }
    } else {
        lv_label_set_text_fmt(label_item, "商品: %s", current_item.c_str());
    }
}

// =================================================================
// ==                         LVGL UI Build                       ==
// =================================================================
void create_ui() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F1923), LV_PART_MAIN);

    // --- 公共样式 ---
    static lv_style_t style_cn;
    lv_style_init(&style_cn);
    lv_style_set_text_font(&style_cn, &font_123);
    lv_style_set_text_color(&style_cn, lv_color_hex(0xCCDDEE));

    // ============= 顶部状态栏 =============
    lv_obj_t *bar_top = lv_obj_create(scr);
    lv_obj_set_size(bar_top, 320, 36);
    lv_obj_align(bar_top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar_top, lv_color_hex(0x1A2E40), 0);
    lv_obj_set_style_border_width(bar_top, 0, 0);
    lv_obj_set_style_radius(bar_top, 0, 0);
    lv_obj_set_style_pad_all(bar_top, 0, 0);
    lv_obj_clear_flag(bar_top, LV_OBJ_FLAG_SCROLLABLE);

    // 状态指示圆点
    lv_obj_t *dot = lv_obj_create(bar_top);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 12, 0);

    label_status = lv_label_create(bar_top);
    lv_obj_add_style(label_status, &style_cn, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0x80CBC4), 0);
    lv_obj_align(label_status, LV_ALIGN_LEFT_MID, 28, 0);
    lv_label_set_text(label_status, "连接中...");

    // ============= 中部数据卡片 =============
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 300, 140);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C2B3A), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A4A5F), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // 商品行
    label_item = lv_label_create(card);
    lv_obj_add_style(label_item, &style_cn, 0);
    lv_obj_set_style_text_color(label_item, lv_color_white(), 0);
    lv_obj_align(label_item, LV_ALIGN_TOP_LEFT, 16, 14);
    lv_label_set_text(label_item, "商品: --");

    // 分割线1
    lv_obj_t *line1 = lv_obj_create(card);
    lv_obj_set_size(line1, 268, 1);
    lv_obj_set_style_bg_color(line1, lv_color_hex(0x2A4A5F), 0);
    lv_obj_set_style_border_width(line1, 0, 0);
    lv_obj_set_style_radius(line1, 0, 0);
    lv_obj_align(line1, LV_ALIGN_TOP_MID, 0, 42);

    // 重量行
    label_weight = lv_label_create(card);
    lv_obj_add_style(label_weight, &style_cn, 0);
    lv_obj_align(label_weight, LV_ALIGN_TOP_LEFT, 16, 52);
    lv_label_set_text(label_weight, "重量: 0 g");

    // 分割线2
    lv_obj_t *line2 = lv_obj_create(card);
    lv_obj_set_size(line2, 268, 1);
    lv_obj_set_style_bg_color(line2, lv_color_hex(0x2A4A5F), 0);
    lv_obj_set_style_border_width(line2, 0, 0);
    lv_obj_set_style_radius(line2, 0, 0);
    lv_obj_align(line2, LV_ALIGN_TOP_MID, 0, 82);

    // 单价行
    label_price = lv_label_create(card);
    lv_obj_add_style(label_price, &style_cn, 0);
    lv_obj_align(label_price, LV_ALIGN_TOP_LEFT, 16, 92);
    lv_label_set_text(label_price, "单价: 0.00 元/kg");

    // ============= 底部总价高亮卡片 =============
    lv_obj_t *card_total = lv_obj_create(scr);
    lv_obj_set_size(card_total, 300, 46);
    lv_obj_align(card_total, LV_ALIGN_TOP_MID, 0, 188);
    lv_obj_set_style_bg_color(card_total, lv_color_hex(0x0D47A1), 0);
    lv_obj_set_style_bg_opa(card_total, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card_total, 0, 0);
    lv_obj_set_style_radius(card_total, 10, 0);
    lv_obj_set_style_pad_all(card_total, 0, 0);
    lv_obj_clear_flag(card_total, LV_OBJ_FLAG_SCROLLABLE);

    label_total = lv_label_create(card_total);
    lv_obj_add_style(label_total, &style_cn, 0);
    lv_obj_set_style_text_color(label_total, lv_color_hex(0xFFD54F), 0);
    lv_obj_align(label_total, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(label_total, "总价: 0.00 元");
}

// =================================================================
// ==                        开机动画                              ==
// =================================================================
static lv_obj_t *boot_label_text = NULL;
static const char boot_text[] = "基于RTOS的视觉称重系统";
static int boot_char_index = 0;

// 开机结束 → 切换到主界面
void boot_transition_cb(lv_timer_t * timer) {
    lv_timer_del(timer);
    lv_obj_clean(lv_scr_act());
    create_ui();
    lv_timer_create(blink_timer_cb, 500, NULL);
}

// 逐字打出标题文字
void typewriter_cb(lv_timer_t * timer) {
    int text_len = strlen(boot_text);
    if (boot_char_index >= text_len) {
        lv_timer_del(timer);
        lv_timer_create(boot_transition_cb, 1500, NULL);
        return;
    }
    uint8_t c = (uint8_t)boot_text[boot_char_index];
    int char_bytes = 1;
    if (c >= 0xF0) char_bytes = 4;
    else if (c >= 0xE0) char_bytes = 3;
    else if (c >= 0xC0) char_bytes = 2;
    boot_char_index += char_bytes;

    char buf[64];
    memcpy(buf, boot_text, boot_char_index);
    buf[boot_char_index] = '\0';
    lv_label_set_text(boot_label_text, buf);
}

// 显示开机画面
void show_boot_animation() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);

    // 居中显示 Logo
    lv_obj_t *img = lv_img_create(scr);
    lv_img_set_src(img, &boot_logo);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -30);

    // 底部逐字打出标题
    static lv_style_t style_boot_text;
    lv_style_init(&style_boot_text);
    lv_style_set_text_font(&style_boot_text, &font_123);
    lv_style_set_text_color(&style_boot_text, lv_color_hex(0x0D5C9E));

    boot_label_text = lv_label_create(scr);
    lv_obj_add_style(boot_label_text, &style_boot_text, 0);
    lv_obj_align(boot_label_text, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_label_set_text(boot_label_text, "");

    boot_char_index = 0;
    lv_timer_create(typewriter_cb, 150, NULL);
}

void take_and_send_picture() {
  if (lvgl_lock(-1)) {
    lv_label_set_text(label_status, "状态: 抓拍发送中...");
    lv_refr_now(NULL); 
    lvgl_unlock();
  }
  
  camera_fb_t *pic = esp_camera_fb_get();
  if (!pic) return;

  HostSerial.write(START_MARKER, sizeof(START_MARKER));
  uint32_t image_size = pic->len;
  HostSerial.write((uint8_t*)&image_size, sizeof(image_size));
  HostSerial.write(pic->buf, pic->len);
  HostSerial.write(END_MARKER, sizeof(END_MARKER));
  esp_camera_fb_return(pic);

  if (lvgl_lock(-1)) {
    lv_label_set_text(label_status, "状态: 等待AI识别...");
    lvgl_unlock();
  }
}

// =================================================================
// ==            Receive and Display Data from Main Board         ==
// =================================================================
void data_receive_task(void *param) {
  String inputString = "";
  inputString.reserve(512);

  while (1) {
    while (HostSerial.available()) {
      char inChar = (char)HostSerial.read();
      if (inChar == '\n') {
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, inputString) == DeserializationError::Ok) {
            String status = doc["status"] | "就绪";
            String itemName = doc["item"] | "N/A";
            String inputStr = doc["input"] | ""; // 提取主板发来的键盘输入
            float weight = doc["weight"] | 0.0;
            float price = doc["price"] | 0.0;
            float total = doc["total"] | 0.0;
            
            String weightStr(weight, 0);
            String priceStr(price, 2);
            String totalStr(total, 2);

            if (lvgl_lock(-1)) {
                // 保存到全局变量，供闪烁定时器使用
                current_item = itemName;
                current_input = inputStr;

                lv_label_set_text(label_status, status.c_str());
                lv_label_set_text_fmt(label_weight, "重量: %s g", weightStr.c_str());
                lv_label_set_text_fmt(label_price, "单价: %s 元/kg", priceStr.c_str());
                lv_label_set_text_fmt(label_total, "总价: %s 元", totalStr.c_str());

                // 数据到达时立刻强制更新一次光标，保证打字跟手无延迟
                if (current_input.length() > 0) {
                    lv_label_set_text_fmt(label_item, "输入单价: %s_", current_input.c_str());
                    cursor_state = true;
                } else {
                    lv_label_set_text_fmt(label_item, "商品: %s", current_item.c_str());
                }
                
                lvgl_unlock();
            }
        }
        inputString = "";
      } else {
        inputString += inChar;
        if (inputString.length() > 500) inputString = "";
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// =================================================================
// ==                            SETUP                            ==
// =================================================================
void setup() {
  Serial.begin(115200);
  HostSerial.begin(921600, SERIAL_8N1, HOST_RX_PIN, HOST_TX_PIN);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  lvgl_api_mux = xSemaphoreCreateRecursiveMutex();

  // Camera Init
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_1; config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM; config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM; config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM; config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM; config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM; config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM; config.xclk_freq_hz = 20000000; config.frame_size = FRAMESIZE_HVGA; config.pixel_format = PIXFORMAT_JPEG; config.grab_mode = CAMERA_GRAB_WHEN_EMPTY; config.fb_location = CAMERA_FB_IN_PSRAM; config.jpeg_quality = 12; config.fb_count = 1;
  esp_camera_init(&config);

  // GFX and LVGL Init
  gfx->begin();
  gfx->fillScreen(BLACK);
  ledcAttach(EXAMPLE_PIN_NUM_LCD_BL, 5000, 10);
  ledcWrite(EXAMPLE_PIN_NUM_LCD_BL, 1023 * 0.8);
  lv_init();
  screenWidth = gfx->width(); screenHeight = gfx->height();
  disp_draw_buf = (lv_color_t *)heap_caps_malloc(screenWidth * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
  lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, screenWidth * 20);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth; disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  show_boot_animation();

  xTaskCreatePinnedToCore(data_receive_task, "data_recv", 4096, NULL, 1, NULL, 0);
}

bool last_button_state = HIGH;
void loop() {
  if (lvgl_lock(-1)) {
    lv_timer_handler(); 
    lvgl_unlock();
  }

  bool current_button_state = digitalRead(BUTTON_PIN);
  if (current_button_state == LOW && last_button_state == HIGH) {
    delay(30); 
    if (digitalRead(BUTTON_PIN) == LOW) {
      take_and_send_picture();
    }
  }
  last_button_state = current_button_state;
  vTaskDelay(pdMS_TO_TICKS(5));
}