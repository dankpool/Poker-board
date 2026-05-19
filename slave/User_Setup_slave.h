// ============================================================
//  User_Setup.h  –  SLAVE  (ILI9341 2.4" + XPT2046)
//  Copy this file to:  Arduino/libraries/TFT_eSPI/User_Setup.h
//  Do this BEFORE compiling the slave sketch.
// ============================================================

#define USER_SETUP_ID 2

// ── Driver ──────────────────────────────────────────────────
#define ILI9341_DRIVER

// ── SPI Pins (ESP32 DevKit V1 standard) ─────────────────────
#define TFT_MISO  19
#define TFT_MOSI  23
#define TFT_SCLK  18
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST    4
#define TFT_BL    21
#define TFT_BACKLIGHT_ON  HIGH

// ── Touch controller ────────────────────────────────────────
#define TOUCH_CS   5
// IRQ → GPIO 27  (wired in sketch)

// ── Fonts ────────────────────────────────────────────────────
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

// ── SPI speeds ───────────────────────────────────────────────
#define SPI_FREQUENCY        40000000
#define SPI_READ_FREQUENCY   20000000
#define SPI_TOUCH_FREQUENCY   2500000
