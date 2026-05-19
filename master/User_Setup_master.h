// ============================================================
//  User_Setup.h  –  MASTER  (ILI9488 3.5" + XPT2046)
//  Copy this file to:  Arduino/libraries/TFT_eSPI/User_Setup.h
//  Do this BEFORE compiling the master sketch.
// ============================================================

#define USER_SETUP_ID 1

// ── Driver ──────────────────────────────────────────────────
#define ILI9488_DRIVER

// ── SPI Pins (ESP32 DevKit V1 standard) ─────────────────────
#define TFT_MISO  19
#define TFT_MOSI  23
#define TFT_SCLK  18
#define TFT_CS    15    // TFT chip select
#define TFT_DC     2    // Data / Command
#define TFT_RST    4    // Reset
#define TFT_BL    21    // Backlight
#define TFT_BACKLIGHT_ON  HIGH

// ── Touch controller (XPT2046 shares SPI bus) ───────────────
#define TOUCH_CS   5    // Touch chip select
// IRQ → GPIO 27  (wired in sketch, not here)

// ── Fonts ────────────────────────────────────────────────────
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

// ── SPI speeds ───────────────────────────────────────────────
#define SPI_FREQUENCY        27000000
#define SPI_READ_FREQUENCY   20000000
#define SPI_TOUCH_FREQUENCY   2500000
