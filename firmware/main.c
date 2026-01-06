/**
 * Single File Spectrum Analyzer
 * Hardware: nrf52832, ST7565 LCD or SH1106 (CH1116) OLED, Radio, no PMIC
 * Updated: Added DFU Bootloader entry
 **/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "nrf.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "nrf_saadc.h"
#include "boards.h"

#pragma region Configuration & Pin Definitions
// --------------------------------------------------------------------------
// Configuration & Pin Definitions
// --------------------------------------------------------------------------

// Select Display Type (Uncomment if SH1106 is used instead of ST7565)
// This is set by the makefile
// #define OLED_TYPE_SH1106

#ifdef LCD_TYPE_DEFAULT
#undef OLED_TYPE_SH1106
#endif

// debugging only: show FPS indicator
// #define SHOW_FPS_INDICATOR

// LCD Pins
#define PIN_LCD_SCL 26
#define PIN_LCD_MOSI 25
#define PIN_LCD_CS 27
#define PIN_LCD_DC 28
#define PIN_LCD_RST 29
#define PIN_LCD_BL 30

// Button & Input Pins
#define PIN_BTN_LEFT 5
#define PIN_BTN_MID 6
#define PIN_BTN_RIGHT 7
#define PIN_ADC_INPUT 2
#define PIN_CHRG_STAT 3

// Spectrum Analyzer Settings
#define SCAN_BASE_FREQ 2400 // in MHz
#define SCAN_START_FREQ 0
#define SCAN_END_FREQ 87
#define BANDWIDTH (SCAN_END_FREQ - SCAN_START_FREQ + 1)

// Display Settings
#define DISP_W 128
#define DISP_H 64
#define SPECTRUM_H 32

// DFU Magic Number (Standard Nordic SDK value)
#define BOOTLOADER_DFU_START 0xB1

// Hardware Offset Definition:
#ifdef OLED_TYPE_SH1106
#define LCD_START_COL 2
#else
#define LCD_START_COL 0
#endif

#pragma endregion Configuration & Pin Definitions

#pragma region Global Variables & Fonts
// --------------------------------------------------------------------------
// Global Variables & Fonts
// --------------------------------------------------------------------------

// The screen buffer
static uint8_t m_frame_buffer[1024];

// array with RSSI values for each frequency in the scan range, normally 20..90 (meaning -20dBm .. -90dBm)
static uint8_t m_rssi_current[BANDWIDTH];
// array with peak RSSI values for each frequency in the scan range. Both will use gradual falloff.
static uint8_t m_rssi_peak[BANDWIDTH];
static float m_rssi_floating[BANDWIDTH];

// array with waterfall data
static uint8_t m_waterfall_data[DISP_W][4];

#ifdef SHOW_FPS_INDICATOR
static uint32_t m_frame_count = 0;
static uint32_t m_fps = 0;
static uint32_t m_last_time = 0;
#endif

typedef struct
{
    uint8_t level; // 0..8
    float voltage;
    bool is_charging;
    int16_t raw_adc;
} bat_status_t;

bat_status_t current_bat_status = {0};
const float lipo_voltage_map[] = {4.1, 3.98, 3.92, 3.87, 3.8, 3.7, 3.6, 3.5, 3.4};

typedef enum
{
    STATE_SCANNER = 0,
    STATE_MENU,
    STATE_INFO,
    STATE_SETTINGS
} app_state_t;

app_state_t current_state = STATE_SCANNER;

typedef enum {
    MENU_BACK = 0,
    MENU_ABOUT,
    MENU_SLEEP,
    MENU_FIRMWARE_UPDATE,
    MENU_SETTINGS,
    NR_MENU_ITEMS // is not a menu entry, serves as maximum marker
} menu_item_t;

int menu_selection = 0; // int and not enum because I iterate

static uint8_t m_contrast_level = 32;

const char *menu_item_names[NR_MENU_ITEMS] =
{
    "Back",
    "About",
    "Sleep",
    "Firmware Update",
    "Settings"
};

typedef enum
{
    SCAN_MODE_FREQUENCY = 0,
    SCAN_MODE_802_15_4,
    SCAN_MODE_802_11,
    SCAN_MODE_BLE,
    NR_SCANNER_MODES // is not a scanner mode, serves as maximum marker
} scanner_mode_t;

int scanner_selection = 0; // int and not enum because I iterate
const char *scanner_mode_names[NR_SCANNER_MODES] =
{
    "Freq",
    "802.15.4",
    "802.11b/g/n",
    "BLE",
};

#define FONT_START 32
#define FONT_END 127
// Font 5x7
const char font5x7[FONT_END - FONT_START + 1][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50}, // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '''
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00}, // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // '6'
    {0x01, 0x01, 0x71, 0x09, 0x07}, // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00}, // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ';'
    {0x08, 0x14, 0x22, 0x41, 0x00}, // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C'
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x3A}, // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // '['
    {0x02, 0x04, 0x08, 0x10, 0x20}, // '\'
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04}, // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40}, // '_'
    {0x00, 0x01, 0x02, 0x04, 0x00}, // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // 'f'
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00}, // '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00}, // '}'
    {0x10, 0x08, 0x08, 0x10, 0x08}, // '~'
    {0x00, 0x7F, 0x3E, 0x1C, 0x08}  // '►', as 0x7F
};
// I repeat the FONT_END - FONT_START in the definition, as that makes the compiler check the size


#pragma endregion Global Variables & Fonts

#pragma region Low-Level Display Driver
// --------------------------------------------------------------------------
// Low-Level Display Driver (OLED and LCD SPI communication)
// --------------------------------------------------------------------------

static void lcd_spi_byte(uint8_t data)
{
    for (int i = 7; i >= 0; i--)
    {
        if (data & (1 << i))
            nrf_gpio_pin_set(PIN_LCD_MOSI);
        else
            nrf_gpio_pin_clear(PIN_LCD_MOSI);

        nrf_gpio_pin_set(PIN_LCD_SCL);
        nrf_gpio_pin_clear(PIN_LCD_SCL);
    }
}

void lcd_write_cmd(uint8_t cmd)
{
    nrf_gpio_pin_clear(PIN_LCD_CS);
    nrf_gpio_pin_clear(PIN_LCD_DC);
    lcd_spi_byte(cmd);
    nrf_gpio_pin_set(PIN_LCD_CS);
}

void lcd_write_data_block(const uint8_t *data, int len)
{
    nrf_gpio_pin_clear(PIN_LCD_CS);
    nrf_gpio_pin_set(PIN_LCD_DC);
    for (int i = 0; i < len; i++)
    {
        lcd_spi_byte(data[i]);
    }
    nrf_gpio_pin_set(PIN_LCD_CS);
}

void lcd_set_contrast(uint8_t contrast); // forward declaration

void lcd_init(void)
{
    nrf_gpio_cfg_output(PIN_LCD_SCL);
    nrf_gpio_cfg_output(PIN_LCD_MOSI);
    nrf_gpio_cfg_output(PIN_LCD_CS);
    nrf_gpio_cfg_output(PIN_LCD_DC);
    nrf_gpio_cfg_output(PIN_LCD_RST);
    nrf_gpio_cfg_output(PIN_LCD_BL);

    nrf_gpio_pin_set(PIN_LCD_CS);
    nrf_gpio_pin_set(PIN_LCD_BL);

    nrf_gpio_pin_clear(PIN_LCD_RST);
    nrf_delay_ms(100);
    nrf_gpio_pin_set(PIN_LCD_RST);
    nrf_delay_ms(100);

#ifdef OLED_TYPE_SH1106
    lcd_write_cmd(0xAE);  // Display off
    lcd_write_cmd(0x00 | (LCD_START_COL & 0x0F));
    lcd_write_cmd(0x10 | (LCD_START_COL >> 4));
    lcd_write_cmd(0x40); // Set display start line to 0
    lcd_write_cmd(0xB0); // Set page address to 0
    lcd_set_contrast(m_contrast_level);
    lcd_write_cmd(0xA1); // Set segment re-map (A0/A1)
    lcd_write_cmd(0xA6); // Set display mode (normal/inverse)
    lcd_write_cmd(0xA8); // Set multiplex ratio
    lcd_write_cmd(0x3F); // Multiplex ratio value
    lcd_write_cmd(0xAD); // Set master configuration
    lcd_write_cmd(0x8B); // Master configuration value
    lcd_write_cmd(0x33); // Set internal VPP regulator
    lcd_write_cmd(0xC8); // Set COM output scan direction
    lcd_write_cmd(0xD3); // Set display offset
    lcd_write_cmd(0x00); // Display offset value
    lcd_write_cmd(0xD5); // Set display clock divide ratio/oscillator frequency
    lcd_write_cmd(0x80); // Display clock divide ratio/oscillator frequency value
    lcd_write_cmd(0xD9); // Set pre-charge period
    lcd_write_cmd(0x1F); // Pre-charge period value
    lcd_write_cmd(0xDA); // Set COM pins hardware configuration
    lcd_write_cmd(0x12); // COM pins hardware configuration value
    lcd_write_cmd(0xDB); // Set VCOMH deselect level
    lcd_write_cmd(0x40); // VCOMH deselect level value
    lcd_write_cmd(0xAF); // Display off
#else
    lcd_write_cmd(0xE2); // 1 1 1 0 1 1 1 0, Reset
    nrf_delay_ms(10);    // sleep 10 ms
    lcd_write_cmd(0xA2); // 1 0 1 0 0 0 1 0, LCD Bias Set: 1/9 bias ratio
    lcd_write_cmd(0xA0); // 1 0 1 0 0 0 0 0, ADC Select (Segment Driver Direction Select): normal
    lcd_write_cmd(0xC8); // 1 1 0 0 1 0 0 0, Common Output Mode Select: reverse
    lcd_write_cmd(0x23); // 0 0 1 0 0 0 1 1, Voltage Regulator Resistor Ratio Set: 3 (0..7)
    lcd_set_contrast(m_contrast_level);
    lcd_write_cmd(0x2F); // 0 0 1 0 1 1 1 1, Power Control Set: all on
    lcd_write_cmd(0xB0); // 1 0 1 1 0 0 0 0, Set Page Address: page 0
    lcd_write_cmd(0xA6); // 1 0 1 0 0 1 1 0, Display Normal / Inverse: normal
    lcd_write_cmd(0xAF); // 1 0 1 0 1 1 1 1, Display ON
#endif
}

void lcd_uninit(void)
{
#ifdef OLED_TYPE_SH1106
    lcd_write_cmd(0xAE);
#endif
    nrf_gpio_pin_clear(PIN_LCD_BL);
    nrf_delay_ms(100);
    nrf_gpio_cfg_default(PIN_LCD_SCL);
    nrf_gpio_cfg_default(PIN_LCD_MOSI);
    nrf_gpio_cfg_default(PIN_LCD_CS);
    nrf_gpio_cfg_default(PIN_LCD_DC);
    nrf_gpio_cfg_default(PIN_LCD_RST);
    nrf_gpio_cfg_default(PIN_LCD_BL);
}

void lcd_flush(void)
{
    for (uint8_t page = 0; page < 8; page++)
    {
        lcd_write_cmd(0xB0 + page);
        lcd_write_cmd(0x00 | (LCD_START_COL & 0x0F));
        lcd_write_cmd(0x10 | ((LCD_START_COL >> 4) & 0x0F));
        lcd_write_data_block(&m_frame_buffer[page * DISP_W], DISP_W);
    }
}

#ifdef OLED_TYPE_SH1106
void lcd_set_contrast(uint8_t contrast)
{
    lcd_write_cmd(0x81);  // Set contrast
    lcd_write_cmd((contrast & 0x3F) << 2);  // Contrast value (0..255, I remap it from 0..63)
}
#else
void lcd_set_contrast(uint8_t contrast)
{
    lcd_write_cmd(0x81);  // Electronic Volume Mode Set (Contrast setting)
    lcd_write_cmd(contrast & 0x3F);  // Electronic Volume Register Set (Contrast value): (0..63)
}
#endif

#pragma endregion Low-Level Display Driver

#pragma region Settings
// --------------------------------------------------------------------------
// Settings
// Settings are stored in UICR CUSTOMER register 0 (0x10001080 .. 0x100010FC)
// This is a poor man's persistance mechanism, as UICR can only be written/erased in 32-bit words
// --------------------------------------------------------------------------

typedef struct {
    uint8_t lcd_contrast : 6; // 0..63, used for both OLED and LCD
    int spare: 22; // spare bits to make it 32 bits total
    uint8_t check: 4; // simple check
} settings_data_t; // please keep this exactly 32 bits. Bigger is possible, but requires more complex handling below.

_Static_assert(sizeof(settings_data_t) == 4, "settings_data_t must be exactly 32 bits (4 bytes)");

static settings_data_t m_settings_data;

#define MAX_CONTRAST 63

#ifdef OLED_TYPE_SH1106
#define DEFAULT_CONTRAST 32
#define MIN_CONTRAST 0
#else
#define DEFAULT_CONTRAST 32
// 16 is too low, you'll see nothing on ST7565
#define MIN_CONTRAST 16
#endif

static const settings_data_t def_settings_data = {
    .lcd_contrast = DEFAULT_CONTRAST,
    .spare = 0,
    .check = 0x0A
};

int32_t settings_init();
int32_t settings_save();

static void validate_settings() {
    if (m_settings_data.lcd_contrast > MAX_CONTRAST) {
        m_settings_data.lcd_contrast = MAX_CONTRAST;
    }
    if (m_settings_data.lcd_contrast < MIN_CONTRAST) {
        m_settings_data.lcd_contrast = MIN_CONTRAST; 
    }
    if (m_settings_data.check != 0x0A) {
        // invalid data, reset to defaults
        memcpy(&m_settings_data, &def_settings_data, sizeof(settings_data_t));
    }
}

int32_t settings_init() {
    // Try to read from UICR customer registers
    // UICR CUSTOMER registers start at address 0x10001080
    uint32_t *uicr_settings = (uint32_t *)0x10001080;
    
    // Check if UICR contains valid data (not 0xFFFFFFFF which is erased state)
    if (*uicr_settings != 0xFFFFFFFF) {
        memcpy(&m_settings_data, uicr_settings, sizeof(settings_data_t)); // This handles multi-byte copy correctly
        validate_settings();
    } else {
        // No valid data in UICR, use defaults
        memcpy(&m_settings_data, &def_settings_data, sizeof(settings_data_t));
    }

    m_contrast_level = m_settings_data.lcd_contrast;

    return 0;
}

int32_t settings_save() {
    bool do_save = false;

    if (m_contrast_level != m_settings_data.lcd_contrast)
    {
        // save new contrast setting
        m_settings_data.lcd_contrast = m_contrast_level;
        do_save = true;                 
    }

    if (!do_save)
        return 0; // nothing to save

    // Enable UICR erase
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {}

    // Erase UICR
    NRF_NVMC->ERASEUICR = 1;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {}
    
    NRF_NVMC->ERASEUICR = 0; // TODO don't know if this is needed
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {}

    // Enable flash writes
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {}    
    
    // Write settings to UICR CUSTOMER[0]
    uint32_t *uicr_addr = (uint32_t *)0x10001080;
    uint32_t data_to_write; // TODO: this is a single 32 bit write. This needs to be changed if settings_data_t becomes larger than 32 bits.
    memcpy(&data_to_write, &m_settings_data, sizeof(uint32_t));
    *uicr_addr = data_to_write;
    
    // Wait for write to complete
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {}
    
    // Disable flash writes
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {}

    // read back and validate
    settings_init();
    
    return 0;
}

#pragma endregion Settings

#pragma region Graphic Primitives
// --------------------------------------------------------------------------
// Graphic Primitives
// --------------------------------------------------------------------------

void draw_pixel(int x, int y, bool on)
{
    if (x >= 0 && x < DISP_W && y >= 0 && y < DISP_H)
    {
        if (on)
            m_frame_buffer[x + (y / 8) * DISP_W] |= (1 << (y % 8));
        else
            m_frame_buffer[x + (y / 8) * DISP_W] &= ~(1 << (y % 8));
    }
}

void draw_vline(int x, int y1, int y2)
{
    if (y1 > y2)
    {
        int t = y1;
        y1 = y2;
        y2 = t;
    }
    for (int y = y1; y <= y2; y++)
        draw_pixel(x, y, true);
}

void draw_box(int x, int y, int w, int h, bool fill, bool color)
{
    for (int i = x; i < x + w; i++)
    {
        for (int j = y; j < y + h; j++)
        {
            if (i == x || i == x + w - 1 || j == y || j == y + h - 1 || fill)
            {
                draw_pixel(i, j, color);
            }
        }
    }
}


/**
 * @brief Draw a bounding box with given offset from the edges
 * 
 * @param offset offset from edge (0...DISP_W/2)
 **/
void draw_boundingbox(int offset)
{
    if (offset < 0) offset = 0;
    if (offset > DISP_W / 2) offset = DISP_W / 2;
    draw_box(offset, offset, DISP_W - 2 * offset, DISP_H - 2 * offset, false, true);
}


/**
 * @brief Draw a filled bar
 * 
 * @param x left of bounding box. If < 0, it is centered on screen
 * @param y top of bounding box
 * @param w width of bounding box
 * @param h height of bounding box
 * @param fill_level fill level
 * @param fill_max maximum fill level
 * @param spacer if true, add 1 pixel empty space between border and fill
 **/
void draw_filled_bar(int x, int y, int w, int h, int fill_level, int fill_max, bool spacer)
{
    if (x < 0)
        x = (DISP_W - w) / 2;
    draw_box(x, y, w, h, false, true);
    
    // inside fill
    if (fill_level > 0)
    {
        if (fill_level > fill_max)
            fill_level = fill_max;
        int offset = 1;
        if (spacer)
            offset = 2;

        int wf = (fill_level * (w - 2 * offset)) / fill_max;
        draw_box(x + offset, y + offset, wf, h - 2 * offset, true, true);
    }
}


/**
 * @brief Draw a character on screen
 * 
 * @param x x position (left of the character)
 * @param y y position (top of the character)
 * @param c the character. Must be within the defined set of the font (see at font definition)
 **/
void draw_char_buf(int x, int y,  char c)
{
    if (c < FONT_START || c > FONT_END)
        c = FONT_START;
    c -= FONT_START;
    for (int i = 0; i < 5; i++)
    {
        uint8_t line = font5x7[(int)c][i];
        for (int j = 0; j < 8; j++)
        {
            if (line & (1 << j))
                draw_pixel(x + i, y + j, true);
        }
    }
}


/**
 * @brief Draw a string on screen
 * 
 * @param x x position (left of the string)
 * @param y y position (top of the string)
 * @param str the string
 **/
void draw_text_buf(int x, int y, const char *str)
{
    while (*str)
    {
        draw_char_buf(x, y, *str++);
        x += 6;
    }
}


/** 
 * @brief Draw a number vertically
 * 
 * @param x x position (center of characters)
 * @param y y position (top of first character)
 * @param nr number to draw. Only numbers 0..99 will be drawn
 **/
void draw_nr_vertical(int x, int y, int nr)
{
    if (nr < 0) return;
    if (nr > 99) nr = 99;
    x -= 2; // center of character (well, approx)
    char ch;    
    if (nr >= 10)
    {
        ch = '0' + (nr / 10);
        draw_char_buf(x, y, ch);
        y += 8;
    }
    ch = '0' + (nr % 10);
    draw_char_buf(x, y, ch);
}

/**
 * @brief draw a string centered on screeen
 * 
 * @param y y position (top of the string)
 * @param str the string
 **/
void draw_text_buf_centered(int y, const char *str)
{
    int len = strlen(str);
    if (len == 0) return;
    int total_width = len * 6; // 5 pixels + 1 pixel space
    int start_x = (DISP_W - total_width) / 2;
    draw_text_buf(start_x, y, str);
}

/**
 * @brief draw a string left aligned on screeen
 * 
 * @param y y position (top of the string)
 * @param str the string
 **/
void draw_text_buf_left(int y, const char *str)
{
    draw_text_buf(0, y, str);
}

/**
 * @brief draw a string right aligned on screeen
 * 
 * @param y y position (top of the string)
 * @param str the string
 **/
void draw_text_buf_right(int y, const char *str)
{
    int len = strlen(str);
    int total_width = (len * 6) - 1; // 5 pixels + 1 pixel space
    int start_x = (DISP_W - total_width);
    draw_text_buf(start_x, y, str);
}

#pragma endregion Graphic Primitives

#pragma region Buttons
// --------------------------------------------------------------------------
// Buttons
// --------------------------------------------------------------------------
// forward declaration
uint32_t get_time_ms(void);

/** 
 * @brief Detect if the mid button is pressed.
 * If `detection_mode` is 0, it will only return True on the transition from not pressed to pressed (button press event).
 * If `detection_mode` is 1, it will return True if the button has been held down (but do not detect length).
 * If `detection_mode` is 2, it will return True if the button has been held down for more than 5 seconds.
 * If `detection_mode` < 0: reset internal counters
 * All use debouncing.
 * 
 * @param detection_mode 0 to get debounced leading edge only, 1 to get the raw button state, 2 to see if the button is held since a long time (5 seconds)
 * @return True if button was pressed, False otherwise
 **/
bool btn_mid(int detection_mode) 
{
    static bool previous_state = false;
    static bool edge_detected = false;
    static uint32_t last_press = 0;

    bool raw_state = (nrf_gpio_pin_read(PIN_BTN_MID) == 0); // active low

    if (detection_mode < 0)
    {
        // reset internal variables
        previous_state = raw_state; // this makes sure that next call will not detect edge
        edge_detected = false;
        last_press = 0;
        return false;
    }

    // debounce logic
    if (raw_state != previous_state)
    {        
        nrf_delay_ms(10); // Debounce delay
        raw_state = (nrf_gpio_pin_read(PIN_BTN_MID) == 0); // re-read after debounce delay
        if (raw_state != previous_state)
        {
            previous_state = raw_state;
            if (raw_state && (detection_mode != 1)) // skip edge detection if in raw mode
            {
                // button just pressed
                last_press = get_time_ms(); // store edge time
                edge_detected = true;
            }
        }
    }
    if (raw_state) // pressed
    {
        if ((detection_mode == 0) && edge_detected) 
        {
            edge_detected = false; // reset edge detection
            return true; // Button was just pressed
        }
        uint32_t now = get_time_ms();
        if (last_press == 0)
            last_press = now; // should not happen, but just in case        
        if (detection_mode == 2)
        {
            if ((now - last_press) >= 5000) // 5 seconds hold
            {
                last_press = now; // prevent multiple triggers
                return true; // Long press detected
            }
        }
    }
    if (detection_mode == 1) 
        return raw_state;
    else 
        return false; // Button not pressed or still pressed, but not long enough to provoke long press
}


/**  
 * @brief Detect if the left button is pressed.
 * @param raw set to True to get the raw button state, False to get auto repeating (3 per sec), and debounced leading edge only
 * @return True if button was pressed, False otherwise
 **/
bool btn_left(bool raw) 
{ 
    static bool previous_state = false;
    static uint32_t last_press = 0;

    bool raw_state = (nrf_gpio_pin_read(PIN_BTN_LEFT) == 0); // active low
    if (raw)
    {
        previous_state = raw_state; // keep previous state for next call, that may need debounce
        return raw_state;
    }
    if (raw_state != previous_state)
    {        
        nrf_delay_ms(10); // Debounce delay
        raw_state = (nrf_gpio_pin_read(PIN_BTN_LEFT) == 0); // re-read after debounce delay
        if (raw_state != previous_state)
        {
            previous_state = raw_state;
            if (raw_state)
            {
                last_press = get_time_ms();
                return true; // Button was just pressed
            }
        }
    }
    else if (raw_state) // still pressed
    {
        if (last_press == 0)
            last_press = get_time_ms(); // should not happen, but just in case        
        uint32_t now = get_time_ms();
        if ((now - last_press) >= 333) // 333ms delay for auto repeat
        {
            last_press = now;
            return true; // Auto repeat press
        }
    }
    return false; // Button not pressed or still pressed (but not long enough to provoke auto repeat)
}

/**  
 * @brief Detect if the right button is pressed.
 * @param raw set to True to get the raw button state, False to get auto repeating (3 per sec), and debounced leading edge only
 * @return True if button was pressed, False otherwise
 **/
bool btn_right(bool raw) 
{ 
    static bool previous_state = false;
    static uint32_t last_press = 0;

    bool raw_state = (nrf_gpio_pin_read(PIN_BTN_RIGHT) == 0); // active low
    if (raw)
    {
        previous_state = raw_state; // keep previous state for next call, that may need debounce
        return raw_state;
    }
    if (raw_state != previous_state)
    {        
        nrf_delay_ms(10); // Debounce delay
        raw_state = (nrf_gpio_pin_read(PIN_BTN_RIGHT) == 0); // re-read after debounce delay
        if (raw_state != previous_state)
        {
            previous_state = raw_state;
            if (raw_state)
            {
                last_press = get_time_ms();
                return true; // Button was just pressed
            }
        }
    }
    else if (raw_state) // still pressed
    {
        if (last_press == 0)
            last_press = get_time_ms(); // should not happen, but just in case
        uint32_t now = get_time_ms();
        if ((now - last_press) >= 333) // 333ms delay for auto repeat
        {
            last_press = now;
            return true; // Auto repeat press
        }
    }
    return false; // Button not pressed or still pressed (but not long enough to provoke auto repeat)
}

void buttons_init(void)
{
    nrf_gpio_cfg_input(PIN_BTN_LEFT, NRF_GPIO_PIN_PULLUP);
    nrf_gpio_cfg_input(PIN_BTN_MID, NRF_GPIO_PIN_PULLUP);
    nrf_gpio_cfg_input(PIN_BTN_RIGHT, NRF_GPIO_PIN_PULLUP);
    nrf_gpio_cfg_input(PIN_CHRG_STAT, NRF_GPIO_PIN_PULLUP);
    // and init their local variables
    btn_mid(-1); // initialize previous state
    btn_left(true); // initialize previous state
    btn_right(true); // initialize previous state
}

#pragma endregion Buttons

#pragma region Power Management and Timer
// --------------------------------------------------------------------------
// Power Management and Timer
// --------------------------------------------------------------------------


/**
 * @brief Enter deep sleep mode.
 * This function will turn off the LCD, disable the radio, and configure the mid button to wake up the device.
 * This function does not return.
 **/
void enter_deep_sleep(void)
{
    memset(m_frame_buffer, 0, 1024);
    lcd_flush();
    lcd_uninit();

    NRF_RADIO->TASKS_DISABLE = 1;
    while (!NRF_RADIO->EVENTS_DISABLED)
        ;

    nrf_gpio_cfg_sense_input(PIN_BTN_MID, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);

    NRF_POWER->SYSTEMOFF = 1;
    while (1)
        ;
}

void timer_init(void)
{
    NRF_TIMER0->TASKS_STOP = 1;
    NRF_TIMER0->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER0->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER0->PRESCALER = 4; // 1MHz resolution
    NRF_TIMER0->TASKS_CLEAR = 1;
    NRF_TIMER0->TASKS_START = 1;
}

uint32_t get_time_ms(void)
{
    NRF_TIMER0->TASKS_CAPTURE[0] = 1;
    return (NRF_TIMER0->CC[0]) / 1000;
}

#pragma endregion Power Management and Timer

#pragma region SAADC (Battery Measurement)
// --------------------------------------------------------------------------
// SAADC (Battery Measurement)
// --------------------------------------------------------------------------

void saadc_init_simple(void)
{
    nrf_gpio_cfg_input(PIN_ADC_INPUT, NRF_GPIO_PIN_NOPULL);
    NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_10bit;
    NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;
    NRF_SAADC->CH[0].CONFIG = (SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) |
                              (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos) |
                              (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
                              (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
                              (SAADC_CH_CONFIG_TACQ_40us << SAADC_CH_CONFIG_TACQ_Pos);
    NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_AnalogInput0 << SAADC_CH_PSELP_PSELP_Pos;
    NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC << SAADC_CH_PSELN_PSELN_Pos;
    NRF_SAADC->ENABLE = 1;
}

void saadc_calibrate(void)
{
    saadc_init_simple();
    NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
    while (NRF_SAADC->EVENTS_CALIBRATEDONE == 0)
        ;
    NRF_SAADC->EVENTS_CALIBRATEDONE = 0;
    NRF_SAADC->ENABLE = 0;
}

void bat_measure_update(void)
{
    volatile int16_t adc_val = 0;
    saadc_init_simple();
    NRF_SAADC->RESULT.PTR = (uint32_t)&adc_val;
    NRF_SAADC->RESULT.MAXCNT = 1;
    NRF_SAADC->EVENTS_STARTED = 0;
    NRF_SAADC->EVENTS_END = 0;
    NRF_SAADC->EVENTS_STOPPED = 0;
    NRF_SAADC->TASKS_START = 1;
    while (NRF_SAADC->EVENTS_STARTED == 0)
        ;
    NRF_SAADC->EVENTS_STARTED = 0;
    NRF_SAADC->TASKS_SAMPLE = 1;
    while (NRF_SAADC->EVENTS_END == 0)
        ;
    NRF_SAADC->EVENTS_END = 0;
    NRF_SAADC->TASKS_STOP = 1;
    while (NRF_SAADC->EVENTS_STOPPED == 0)
        ;
    NRF_SAADC->EVENTS_STOPPED = 0;
    NRF_SAADC->ENABLE = 0;

    if (adc_val < 0)
        adc_val = 0;
    current_bat_status.raw_adc = adc_val;

    float voltage = (3.6f / 1024.0f) * (float)adc_val * 1.451f;
    uint8_t level = 0;
    for (uint32_t i = 0; i < 9; i++)
    {
        if (voltage >= lipo_voltage_map[i])
        {
            level = 9 - i - 1;
            break;
        }
    }
    current_bat_status.voltage = voltage;
    current_bat_status.level = level;
    current_bat_status.is_charging = (nrf_gpio_pin_read(PIN_CHRG_STAT) == 0);
}
#pragma endregion SAADC (Battery Measurement)

#pragma region Radio Scanner
// --------------------------------------------------------------------------
// Radio Scanner
// --------------------------------------------------------------------------


/**
 * @brief Initialize the radio for scanning
 **/
void radio_init_scanner(void)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0)
        ;
    NRF_RADIO->POWER = 1;
    NRF_RADIO->MODE = (RADIO_MODE_MODE_Ble_1Mbit << RADIO_MODE_MODE_Pos);
}


/**
 * @brief scans the radio.
 * This function fills the `m_rssi_current[]` array with RSSI values for each frequency in the scan range.
 **/
void scan_band(void)
{
    if ((NRF_CLOCK->HFCLKSTAT & (CLOCK_HFCLKSTAT_SRC_Msk | CLOCK_HFCLKSTAT_STATE_Msk)) !=
        (CLOCK_HFCLKSTAT_SRC_Xtal << CLOCK_HFCLKSTAT_SRC_Pos | CLOCK_HFCLKSTAT_STATE_Running << CLOCK_HFCLKSTAT_STATE_Pos))
    {
        NRF_CLOCK->TASKS_HFCLKSTART = 1;
        while ((NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_STATE_Msk) == 0)
            ;
    }
    for (uint32_t freq = SCAN_START_FREQ; freq <= SCAN_END_FREQ; freq++)
    {
        NRF_RADIO->FREQUENCY = freq;
        NRF_RADIO->DATAWHITEIV = 0x40;
        NRF_RADIO->EVENTS_READY = 0;
        NRF_RADIO->TASKS_RXEN = 1;
        while (NRF_RADIO->EVENTS_READY == 0)
            ;
        NRF_RADIO->EVENTS_RSSIEND = 0;
        NRF_RADIO->TASKS_RSSISTART = 1;
        while (NRF_RADIO->EVENTS_RSSIEND == 0)
            ;
        uint8_t val = NRF_RADIO->RSSISAMPLE;
        if ((freq - SCAN_START_FREQ) < BANDWIDTH)
            m_rssi_current[freq - SCAN_START_FREQ] = val;
        NRF_RADIO->EVENTS_DISABLED = 0;
        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0)
            ;
    }
}

// Frequency mapping table to IEEE 802.15.4 channel, WiFi channel, and BLE channel
// this is not memory optimal, but CPU optimal, as the index is the frequency.
// index = frequency - SCAN_BASE_FREQ
// -1 means: not a center frequency for that standard
// columns: 802.15.4, 802.11b/g/n WiFi, BLE
// These columns must be aligned with the scanner_mode_names, offset by 1.
const int8_t freq_to_channel_map[BANDWIDTH][3] = {
    { -1, -1, -1}, // 2400
    { -1, -1, -1}, // 2401
    { -1, -1, 37}, // 2402
    { -1, -1, -1}, // 2403
    { -1, -1,  0}, // 2404
    { 11, -1, -1}, // 2405
    { -1, -1,  1}, // 2406
    { -1, -1, -1}, // 2407
    { -1, -1,  2}, // 2408
    { -1, -1, -1}, // 2409
    { 12, -1,  3}, // 2410
    { -1, -1, -1}, // 2411
    { -1,  1,  4}, // 2412
    { -1, -1, -1}, // 2413
    { -1, -1,  5}, // 2414
    { 13, -1, -1}, // 2415
    { -1, -1,  6}, // 2416
    { -1,  2, -1}, // 2417
    { -1, -1,  7}, // 2418
    { -1, -1, -1}, // 2419
    { 14, -1,  8}, // 2420
    { -1, -1, -1}, // 2421
    { -1,  3,  9}, // 2422
    { -1, -1, -1}, // 2423
    { -1, -1, 10}, // 2424
    { 15, -1, -1}, // 2425
    { -1, -1, 38}, // 2426
    { -1,  4, -1}, // 2427
    { -1, -1, 11}, // 2428
    { -1, -1, -1}, // 2429
    { 16, -1, 12}, // 2430
    { -1, -1, -1}, // 2431
    { -1,  5, 13}, // 2432
    { -1, -1, -1}, // 2433
    { -1, -1, 14}, // 2434
    { 17, -1, -1}, // 2435
    { -1, -1, 15}, // 2436
    { -1,  6, -1}, // 2437
    { -1, -1, 16}, // 2438
    { -1, -1, -1}, // 2439
    { 18, -1, 17}, // 2440
    { -1, -1, -1}, // 2441
    { -1,  7, 18}, // 2442
    { -1, -1, -1}, // 2443
    { -1, -1, 19}, // 2444
    { 19, -1, -1}, // 2445
    { -1, -1, 20}, // 2446
    { -1,  8, -1}, // 2447
    { -1, -1, 21}, // 2448
    { -1, -1, -1}, // 2449
    { 20, -1, 22}, // 2450
    { -1, -1, -1}, // 2451
    { -1,  9, 23}, // 2452
    { -1, -1, -1}, // 2453
    { -1, -1, 24}, // 2454
    { 21, -1, -1}, // 2455
    { -1, -1, 25}, // 2456
    { -1, 10, -1}, // 2457
    { -1, -1, 26}, // 2458
    { -1, -1, -1}, // 2459
    { 22, -1, 27}, // 2460
    { -1, -1, -1}, // 2461
    { -1, 11, 28}, // 2462
    { -1, -1, -1}, // 2463
    { -1, -1, 29}, // 2464
    { 23, -1, -1}, // 2465
    { -1, -1, 30}, // 2466
    { -1, 12, -1}, // 2467
    { -1, -1, 31}, // 2468
    { -1, -1, -1}, // 2469
    { 24, -1, 32}, // 2470
    { -1, -1, -1}, // 2471
    { -1, 13, 33}, // 2472
    { -1, -1, -1}, // 2473
    { -1, -1, 34}, // 2474
    { 25, -1, -1}, // 2475
    { -1, -1, 35}, // 2476
    { -1, -1, -1}, // 2477
    { -1, -1, 36}, // 2478
    { -1, -1, -1}, // 2479
    { 26, -1, 39}, // 2480
    { -1, -1, -1}, // 2481
    { -1, -1, -1}, // 2482
    { -1, -1, -1}, // 2483
    { -1, 14, -1}, // 2484
    { -1, -1, -1}, // 2485
    { -1, -1, -1}, // 2486
    { -1, -1, -1}  // 2487
};

/**
 * @brief Convert frequency to channel number for given mode
 * 
 * @param freq Frequency in MHz, already offset by SCAN_BASE_FREQ
 * @param mode SCAN_MODE_802_15_4, SCAN_MODE_802_11, SCAN_MODE_BLE
 * @return int Channel number, or -1 if not a valid channel for that mode
 **/
int freq_to_channel(int freq, int mode)
{
    // translate SCAN_MODE_802_15_4, SCAN_MODE_802_11, SCAN_MODE_BLE to 0..2
    mode -= 1;
    if (mode < 0 || mode > 2)
        return -1;
    if (freq < SCAN_START_FREQ || freq > SCAN_END_FREQ)
        return -1;
    return freq_to_channel_map[freq - SCAN_START_FREQ][mode];
}

#pragma endregion Radio Scanner

#pragma region UI Functions - rendering
// --------------------------------------------------------------------------
// UI Functions - rendering
// --------------------------------------------------------------------------

/**
 * @brief Show the boot "splash screen"
 **/
void show_boot_screen(void)
{
    memset(m_frame_buffer, 0, 1024);

    draw_boundingbox(2);
    draw_boundingbox(4);

    draw_text_buf_centered(15, "2.4GHz");
    draw_text_buf_centered(25, "SPECTRUM");
    draw_text_buf_centered(35, "ANALYZER");
    draw_text_buf_centered(45, "ATC1441");

    lcd_flush();
    nrf_delay_ms(1500);
}


/**
 * @brief Startup screen sequence
 **/
void check_power_on_sequence(void)
{
	// Sometimes not working so only activate in development or so
    /*uint32_t reset_reason = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = 0xFFFFFFFF;

    // If not a Power-Off Reset, start normally
    if ((reset_reason & POWER_RESETREAS_OFF_Msk) == 0)
    {
        return;
    }*/

    // If from Deep Sleep: Check if button is held
    for (int i = 0; i < 20; i++)
    {
        if (!btn_mid(1)) // this is the only use of raw button read: need to see if it is still pressed.
        {
            memset(m_frame_buffer, 0, 1024);
            lcd_flush();
            enter_deep_sleep();
        }

        memset(m_frame_buffer, 0, 1024);
        draw_text_buf_centered(25, "HOLD TO START");

        draw_filled_bar(-1, 38, 100, 8, i, 19, true);

        lcd_flush();
        nrf_delay_ms(50);
    }
}

/**
 * @brief Render the battery icon at the specified position.
 * 
 * @param x The x-coordinate for the battery icon. Use -1 for right aligned.
 * @param y The y-coordinate for the battery icon.
 */
void render_battery_icon(int x, int y)
{
    int h = 7;
    // the battery
    int wm = (2 * 8) + 2; // 2 pixels per level + 2 pixels border
    if (x < 0)
        x = DISP_W - (wm + 4); // right aligned
    draw_filled_bar(x, y, wm, h, current_bat_status.level, 8, false);
    // tip of the battery
    draw_box(x + wm, y+2, 2, h-4, false, true);
}


/**
 * @brief Measure the battery and render the battery status, top right on the screen. 
 */
void render_battery(void)
{
    char buf[10];
    // Battery Update (rarely)
    static int batt_ctr = 0;
    if (batt_ctr == 0)
    {
        bat_measure_update();
    }
    if (batt_ctr++ > 60)
    {
        batt_ctr = 0;
    }

    if (current_bat_status.is_charging)
    {
        draw_text_buf_right(0, "CHRG");
    }
    else
    {
        // text
        // int pct = (current_bat_status.level * 100) / 8;
        // sprintf(buf, "%d%%", pct);
        // draw_text_buf_right(0, buf);

        // icon
        render_battery_icon(-1, 0);
    }
}


/**
 * @brief calculates the frequence (offset to SCAN_BASE_FREQ) for a column on screen
 * 
 * @param column 
 * @return int he offset to SCAN_BASE_FREQ for a column on screen
 **/
int column_to_freq(int column)
{
    if (column < 0)
        column = 0;
    if (column >= DISP_W)
        column = DISP_W - 1;
    int freq_idx = (column * BANDWIDTH) / DISP_W;
    return SCAN_START_FREQ + freq_idx;
}


/**
 * @brief Process the waterfall data in the scanner view, by shifting down and adding new line.
 * 
 * To be called only by `render_scanner()`
 **/
void process_scanner_waterfall(void)
{
#define WATERFALL_MINIMUM_VALUE 88

    for (int x = 0; x < DISP_W; x++)
    {
        int freq_idx = column_to_freq(x);
        uint8_t val = m_rssi_current[freq_idx];
        bool pixel_on = (val < WATERFALL_MINIMUM_VALUE); // this is a threshold for turning the pixel on
        uint32_t col = (m_waterfall_data[x][3] << 24) | (m_waterfall_data[x][2] << 16) | (m_waterfall_data[x][1] << 8) | m_waterfall_data[x][0];
        col <<= 1;
        if (pixel_on)
            col |= 1;
        m_waterfall_data[x][0] = (col & 0xFF);
        m_waterfall_data[x][1] = (col >> 8) & 0xFF;
        m_waterfall_data[x][2] = (col >> 16) & 0xFF;
        m_waterfall_data[x][3] = (col >> 24) & 0xFF;
    }
}

/**
 * @brief Render only the waterfall part of the scanner screen
 * 
 * To be called only by `render_scanner()`
 **/
void render_scanner_waterfall(void)
{
    // Draw Waterfall (Bottom half)
    int start_page = SPECTRUM_H / 8;
    for (int x = 0; x < DISP_W; x++)
    {
        for (int p = 0; p < 4; p++)
        {
            if (start_page + p < 8)
                m_frame_buffer[x + (start_page + p) * DISP_W] = m_waterfall_data[x][p];
        }
    }
}

/**
 * @brief Render the channel markers
 * 
 * To be called only by `render_scanner()`
 * @param mode SCAN_MODE_802_15_4, SCAN_MODE_802_11, SCAN_MODE_BLE
 **/
void render_scanner_channels(int mode) 
{
// channel numbers are 16 pixes high 
#define CHANNEL_TOP_TEXT (DISP_H - 16)
    // Draw Waterfall with channel markers (Bottom half)
    int start_page = SPECTRUM_H / 8;
    int last_channel = -1;
    for (int x = 0; x < DISP_W; x++)
    {
        int freq_idx = column_to_freq(x);
        int channel = freq_to_channel(freq_idx, mode);
        if (channel != -1)
        {
            if (channel == last_channel)
                continue; // already drawn
            last_channel = channel;
            bool draw_channel_nr = true;
            enum line_type_enum { LINE_NORMAL = 0, LINE_SHORT, LINE_DOTTED };
            enum line_type_enum line_type = LINE_NORMAL; // normal line. 
            if (mode == 3)
            {
                // for BLE
                if (channel == 37 || channel == 38 || channel == 39)
                {
                    // special case for advertising channels
                    line_type = LINE_DOTTED;
                    draw_channel_nr = false; // would be nice, but messes up
                } 
                else
                {
                    // too many channels, only draw every 5th channel number
                    if (channel % 5 != 0)
                    {
                        draw_channel_nr = false;
                        line_type = LINE_SHORT;
                    }
                }
            }
            // draw line
            switch (line_type)
            {
                case LINE_NORMAL:
                    draw_vline(x, SPECTRUM_H + 2, CHANNEL_TOP_TEXT - 2);
                    break;
                case LINE_SHORT:
                    draw_vline(x, SPECTRUM_H + 2, CHANNEL_TOP_TEXT - 4);
                    break;
                case LINE_DOTTED:
                    for (int y = SPECTRUM_H + 2; y <= CHANNEL_TOP_TEXT - 2; y += 2)
                    {
                        draw_pixel(x, y, true);
                    }
                    break;
            }

            // draw channel number vertically
            if (draw_channel_nr)
                draw_nr_vertical(x, CHANNEL_TOP_TEXT, channel);
        }
    }
}

/**
 * @brief Render the main scanner screen
 **/
void render_scanner(void)
{
    memset(m_frame_buffer, 0, 1024);

    for (int x = 0; x < DISP_W; x++)
    {
        int freq_idx = column_to_freq(x);
        if (freq_idx >= BANDWIDTH)
            freq_idx = BANDWIDTH - 1;
        uint8_t val = m_rssi_current[freq_idx];
        int height = 0;
        if (val < 95)
            height = 95 - val;
        if (height > SPECTRUM_H)
            height = SPECTRUM_H;
        if (height < 0)
            height = 0;

        // Peak Hold Logic with gradual falloff
        if (height >= m_rssi_peak[freq_idx])
            m_rssi_peak[freq_idx] = height;
        else if (m_rssi_peak[freq_idx] > 0)
            m_rssi_peak[freq_idx]--;

        int bar_h = m_rssi_peak[freq_idx];
        if (bar_h > 0)
            draw_vline(x, SPECTRUM_H - bar_h, SPECTRUM_H - 1);

        // Floating Dot, even slower falloff
        float fh = (float)height;
        if (fh >= m_rssi_floating[freq_idx])
            m_rssi_floating[freq_idx] = fh;
        else
        {
            m_rssi_floating[freq_idx] -= 0.5f;
            if (m_rssi_floating[freq_idx] < 0)
                m_rssi_floating[freq_idx] = 0;
        }

        int dot_y;
        if ((int)m_rssi_floating[freq_idx] <= 0)
            dot_y = SPECTRUM_H - 1 ; // base line
        else 
            dot_y = SPECTRUM_H - (int)m_rssi_floating[freq_idx] - 2; // 2 offset
        if (dot_y >= 0)
            draw_pixel(x, dot_y, true);
    }

    process_scanner_waterfall();

    switch (scanner_selection)
    {
        case SCAN_MODE_FREQUENCY:
            render_scanner_waterfall();
            break;
        case SCAN_MODE_802_15_4:
        case SCAN_MODE_802_11:
        case SCAN_MODE_BLE:
            render_scanner_channels(scanner_selection);
            break;
        default:
            draw_text_buf_centered(0, "UNKNOWN MODE");
            break;
    }

    // Info Text
    char buf[40];
#ifdef SHOW_FPS_INDICATOR
    // show the FPS for debugging
    sprintf(buf, "%luhz", m_fps);
    draw_text_buf_left( 0, buf);
#else
    // scanner modes
    sprintf(buf, "%s", scanner_mode_names[scanner_selection]);
    draw_text_buf_left( 0, buf);
#endif
    // Frequency Labels
    if (scanner_selection == 0)
    {
        sprintf(buf, "%d", SCAN_BASE_FREQ + column_to_freq(0));
        draw_text_buf_left( 56, buf);
        sprintf(buf, "%d", SCAN_BASE_FREQ + column_to_freq(DISP_W - 1));
        draw_text_buf_right(56, buf);
    }

    // Battery Status
    render_battery();

    lcd_flush();
}

/**
 * @brief Render the info screen
 **/
void render_info(void)
{
    memset(m_frame_buffer, 0, 1024);

    // Border
    draw_boundingbox(0);

    draw_text_buf(4, 4, "Made by ATC1441");

    draw_text_buf(4, 13, "github.com");
    draw_text_buf(4, 21, "/atc1441");
    draw_text_buf(4, 29, "/pixlAnalyzer");

    draw_text_buf(4, 44, "Credit Codebase:");
    draw_text_buf(4, 52, "solosky/pixl.js");

    lcd_flush();
}

/**
 * @brief Render the settings screen.
 * For now, only the LCD contrast
 **/
void render_settings(void)
{
    memset(m_frame_buffer, 0, 1024);

    char buf[30];
    sprintf(buf, "Contrast: %d", m_contrast_level);
    draw_text_buf_centered(20, buf);

    draw_filled_bar(-1, 30, MAX_CONTRAST + 2, 8, m_contrast_level, MAX_CONTRAST, false);

    lcd_flush();
}

// The left alignment column for menu items
#define MENU_LEFT_X 25


/**
 * @brief Render a single menu item line.
 * 
 * @param line Line number to render. -1 for header.
 **/
void render_menu_item(int line)
{
    if (line < 0) 
    {
        // this is the header
        draw_text_buf_centered(3, "MENU");
        return;
    }

    if (line > NR_MENU_ITEMS - 1)
        return; // out of range
    
    int x = MENU_LEFT_X;
    int y = (line * 8) + 21;
    if (line == menu_selection)
    {
        draw_char_buf(x - 10, y, 0x7F); // '►' character
    }
    const char *text = menu_item_names[line];
    draw_text_buf(x, y, text);
}


/**
 * @brief Render the menu screen.
 * It does not act on buttons, just renders the current state.
 **/
void render_menu(void)
{
    bat_measure_update();
    memset(m_frame_buffer, 0, 1024);

    draw_boundingbox(0);

    render_menu_item(-1); // header

    // Battery Status
    char buf[30];
    int v_int = (int)current_bat_status.voltage;
    int v_dec = (int)((current_bat_status.voltage - v_int) * 100);
    int y = 11;
    sprintf(buf, "Batt: %d.%02dV", v_int, v_dec);
    draw_text_buf(MENU_LEFT_X, y, buf);

    int x = MENU_LEFT_X + ((strlen(buf) + 1) * 6); // N chars width + 2 chars space
    render_battery_icon(x, y);

    // Menu Items
    for (int line = 0; line < NR_MENU_ITEMS; line++)
    {
        render_menu_item(line);
    }

    lcd_flush();
}


/**
 * @brief Tell the user that the device goes to sleep, and then go to sleep.
 * This function does not return
 **/
void render_goto_sleep(void)
{
    memset(m_frame_buffer, 0, 1024);
    draw_text_buf_centered(30, "GOODBYE");
    lcd_flush();
    nrf_delay_ms(1000);
    enter_deep_sleep(); // This function does not return
}


/**
 * @brief Tell the user the device goes to DFU mode, and then reset to the DFU bootloader.
 * This function does not return.
 **/
void render_goto_dfu(void)
{
    memset(m_frame_buffer, 0, 1024);
    draw_text_buf_centered(30, "ENTERING DFU...");
    lcd_flush();
    nrf_delay_ms(1000);

    // Signal to bootloader to enter DFU mode
    NRF_POWER->GPREGRET = BOOTLOADER_DFU_START;
    NVIC_SystemReset(); // This function does not return
}

#pragma endregion UI Functions - rendering

#pragma region UI Functions - interacting
// --------------------------------------------------------------------------
// UI Functions - interacting
// --------------------------------------------------------------------------


/**
 * @brief handles the scanner interactions
 */
void handle_scanner(void)
{
    scan_band();
    render_scanner();

    // Interaction
    if (btn_mid(0))
    {
        current_state = STATE_MENU;
        menu_selection = MENU_BACK; // Reset selection on entry
    }
    if (btn_left(false)) scanner_selection--;
    if (btn_right(false)) scanner_selection++;
    // wrap around scanner mode selections
    if (scanner_selection < 0)
        scanner_selection = NR_SCANNER_MODES - 1;
    if (scanner_selection > NR_SCANNER_MODES - 1)
        scanner_selection = 0;

#ifdef SHOW_FPS_INDICATOR
    // FPS Counter
    m_frame_count++;
    uint32_t now = get_time_ms();
    if (now - m_last_time >= 1000)
    {
        m_fps = m_frame_count;
        m_frame_count = 0;
        m_last_time = now;
    }
#endif         
}

/**
 * @brief Handle the menu interactions
 **/
void handle_menu(void)
{
    render_menu();

    if (btn_left(false)) menu_selection--;
    if (btn_right(false)) menu_selection++;
    // wrap around menu selection
    if (menu_selection < 0)
        menu_selection = NR_MENU_ITEMS - 1;
    if (menu_selection > NR_MENU_ITEMS - 1)
        menu_selection = 0;

    if (btn_mid(0))
    {
        switch (menu_selection)
        {
            case MENU_BACK:
                current_state = STATE_SCANNER;
                break;
            case MENU_ABOUT:
                current_state = STATE_INFO;
                break;
            case MENU_SLEEP:
                render_goto_sleep(); // this halts the program (in a deep sleep mode)
                break;
            case MENU_FIRMWARE_UPDATE:
                render_goto_dfu(); // this exits to a bootloader
                break;
            case MENU_SETTINGS:
                current_state = STATE_SETTINGS;
                break;
            default:
                // do nothing
                break;
        }
    }
    nrf_delay_ms(150);
}


/**
 * @brief Handle the "About" screen interactions
 */
void handle_info(void)
{
    render_info();
    // Press any key to go back
    if (btn_mid(0) || btn_left(false) || btn_right(false))
    {
        current_state = STATE_MENU;
        nrf_delay_ms(150);
    }
}

/**
 * @brief Handles the Settings interactions
 */
void handle_settings(void)
{
    render_settings();

    if (btn_left(false))
    {
        if (m_contrast_level > MIN_CONTRAST)
            m_contrast_level--;
        lcd_set_contrast(m_contrast_level);
    }
    if (btn_right(false))
    {
        if (m_contrast_level < MAX_CONTRAST)
            m_contrast_level++;
        lcd_set_contrast(m_contrast_level);
    }
    if (btn_mid(0))
    {
        current_state = STATE_MENU;
        settings_save(); // will check if changes are needed to be persisted
        nrf_delay_ms(150);
    }
}

#pragma endregion UI Functions - interacting

#pragma region Main Application
// --------------------------------------------------------------------------
// Main Application
// --------------------------------------------------------------------------

int main(void)
{
    // Init Hardware
    settings_init();
    timer_init();
    buttons_init();
    lcd_init();

    // Clear Screen
    memset(m_frame_buffer, 0, 1024);
    lcd_flush();

    // Boot Sequence
    check_power_on_sequence();
    show_boot_screen();

    // Calibration & Init Radio
    saadc_calibrate();
    bat_measure_update();
    radio_init_scanner();

    // Clear Data
    memset(m_rssi_peak, 0, sizeof(m_rssi_peak));
    memset(m_rssi_floating, 0, sizeof(m_rssi_floating));
    memset(m_waterfall_data, 0, sizeof(m_waterfall_data));
#ifdef SHOW_FPS_INDICATOR
    m_last_time = get_time_ms();
#endif

    buttons_init(); // again, to kill any long press state from boot

    while (1)
    {
        if (btn_mid(2))
        {
            // Long press to enter deep sleep from any state
            render_goto_sleep();
        }
        switch (current_state)
        {
            case STATE_SCANNER:
                handle_scanner();
                break;
            case STATE_MENU:
                handle_menu();
                break;
            case STATE_INFO:
                handle_info();
                break;
            case STATE_SETTINGS:
                handle_settings();
                break;
            default:
                current_state = STATE_SCANNER;
                break;
        }
    }
}

#pragma endregion Main Application