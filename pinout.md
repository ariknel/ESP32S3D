# Wiring

## SD card module (SPI, default)

| SD module | ESP32-S3 |
|-----------|----------|
| CS        | GPIO10   |
| MOSI (DI) | GPIO11   |
| SCK (CLK) | GPIO12   |
| MISO (DO) | GPIO13   |
| VCC       | 5V if the module has its own regulator, else 3V3 |
| GND       | GND      |

Card must be FAT32.

## Printer USB (USB-A female socket)

| USB-A pin       | ESP32-S3          |
|-----------------|-------------------|
| 1 VBUS (red)    | **not connected** |
| 2 D− (white)    | GPIO19            |
| 3 D+ (green)    | GPIO20            |
| 4 GND (black)   | GND               |

Connect a normal USB A-to-B cable from this socket to the CR-10.

**Easier option, no wiring:** plug a USB-C (male) → USB-A (female) OTG adapter into the board's
native/JTAG USB-C port, which is GPIO19/20. Then connect the printer cable to the adapter.
First check VBUS:
1. Power the ESP32 with the printer cable unplugged.
2. Measure between pin 1 and pin 4 of the adapter's USB-A socket.
3. If you read about 5 V, tape over the VBUS contact of the printer cable's USB-A plug (or use a
   data-only adapter). Otherwise the ESP32 back-powers the printer board.

## Board USB-C ports

- **UART port:** goes to the PC, for flashing and logs.
- **Native/JTAG USB port** (GPIO19/20): this is the printer connection, either through an OTG
  adapter or wired to a USB-A socket. It can no longer be used for flashing.

## Power

Give the ESP32 its own 5 V supply. The printer powers itself.

Pins can be changed with `.\idf.ps1 menuconfig` → *CR-10 Print Server*.

Pins to avoid: 0, 3, 45, 46 (boot-mode pins), 19/20 (printer USB), 43/44 (log port), and 35–37 on boards with octal PSRAM.
