// ============================================================
// User_Setup.h  -- for Bodmer's TFT_eSPI library
// Board  : ESP32-S3-N16R8 (UICPAI dev board, dual USB-C)
// Display: 3.5" SPI TFT, driver ILI9488, 480x320, no touch, SKU MSP3521
//
// HOW TO USE:
// 1. Find your Arduino libraries folder, e.g.
//      Windows: Documents\Arduino\libraries\TFT_eSPI
//      Mac/Linux: ~/Arduino/libraries/TFT_eSPI
// 2. Replace the existing "User_Setup.h" file inside that folder
//    with THIS file (make a backup of the original first).
// ============================================================

#define USER_SETUP_INFO "ESP32S3_ILI9488"

// ---------- Driver ----------
#define ILI9488_DRIVER

// ---------- Resolution (portrait orientation by default) ----------
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// ---------- SPI pins (match the wiring diagram) ----------
#define TFT_MOSI 13   // SDI / MOSI
#define TFT_MISO 9    // SDO / MISO
#define TFT_SCLK 14   // SCK
#define TFT_CS   10   // Chip select
#define TFT_DC   11   // DC / RS
#define TFT_RST  12   // RESET
#define TFT_BL   8    // Backlight (LED pin)
#define TFT_BACKLIGHT_ON HIGH

// ---------- Fonts ----------
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// ---------- SPI frequency ----------
#define SPI_FREQUENCY       27000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

// ---------- Use HSPI/FSPI explicitly on ESP32-S3 ----------
#define USE_HSPI_PORT