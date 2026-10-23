#pragma once
// ESP32-2432S028R ("Cheap Yellow Display"), single-USB ILI9341 variant.
// Every board pin lives here. This header is force-included into every
// translation unit (see platformio.ini), so TFT_eSPI is configured from it
// without editing the library's User_Setup.h. To support the dual-USB /
// ST7789 variant later, change only this file.

// ---- Display: ILI9341 on HSPI ----
#define USER_SETUP_LOADED 1
#define ILI9341_2_DRIVER 1
#define TFT_WIDTH 240
#define TFT_HEIGHT 320
#define USE_HSPI_PORT 1
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST -1
#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH
#define SPI_FREQUENCY 55000000
#define SPI_READ_FREQUENCY 20000000
#define LOAD_GLCD 1
#define LOAD_FONT2 1
#define LOAD_FONT4 1
#define LOAD_GFXFF 1
#define SMOOTH_FONT 1
// Touch uses XPT2046_Touchscreen on its own bus, so TFT_eSPI touch support (TOUCH_CS) stays off.
#define DISABLE_ALL_LIBRARY_WARNINGS 1

// ---- Touch: XPT2046 on its own SPI bus (VSPI) ----
#define PIN_TOUCH_CLK 25
#define PIN_TOUCH_MOSI 32
#define PIN_TOUCH_MISO 39
#define PIN_TOUCH_CS 33
#define PIN_TOUCH_IRQ 36

// ---- RGB LED (active LOW) ----
#define PIN_LED_R 4
#define PIN_LED_G 16
#define PIN_LED_B 17

// ---- Other I/O ----
#define PIN_LDR 34      // light sensor, ADC1
// Some CYD revisions leave the LDR unpopulated (true of the first test unit).
// With 0, sim mode "ldr" falls back to a static value and warns.
#define HAS_LDR 0
#define PIN_BOOT 0      // BOOT button, active LOW, usable at runtime
#define PIN_SPEAKER 26  // optional, unused

// ---- SD card (VSPI default pins; shares nothing with touch at runtime) ----
#define PIN_SD_SCK 18
#define PIN_SD_MISO 19
#define PIN_SD_MOSI 23
#define PIN_SD_CS 5

// ---- Screen geometry (landscape, rotation 1) ----
#define SCREEN_W 320
#define SCREEN_H 240
