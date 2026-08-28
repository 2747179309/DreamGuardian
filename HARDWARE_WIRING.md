# DreamGuardian 2 Hardware Wiring

This wiring is frozen for the ESP32-S3-BOX-3 + ESP32-S3-BOX-3-DOCK prototype.
The connector called PMOD2 below is J5 in the official DOCK schematic.

## 1. Frozen GPIO allocation

| Function | ESP32-S3 GPIO | DOCK PMOD2 | J5 pin | Direction |
|---|---:|---|---:|---|
| LD6002 UART TX | GPIO43 | PCIe/expansion GPIO43 | - | Optional BOX -> radar RX0; unused by current firmware |
| LD6002 UART RX | GPIO44 | PCIe/expansion GPIO44 | - | radar TX0 -> BOX |
| Stereo I2S BCLK | GPIO13 | IO1 | 1 | BOX -> both amplifiers |
| Stereo I2S LRCLK/WS | GPIO10 | IO5 | 7 | BOX -> both amplifiers |
| Stereo I2S DOUT | GPIO14 | IO6 | 8 | BOX -> both amplifiers |
| Signal ground | GND | GND | 5 or 11 | Common ground |

The firmware uses UART1 through the GPIO matrix on GPIO43/GPIO44. Console logs and the test command prompt use USB Serial/JTAG on COM12, so radar traffic does not mix with logs.

## 2. LD6002 wiring

| LD6002 pin | Connect to |
|---|---|
| Pin 1, 3V3 | Dedicated low-ripple 3.3 V regulator output |
| Pin 2, GND | Common ground |
| Pin 3, P19/BOOT1 | GND; must be low before radar power-up |
| Pin 7, TX0 | GPIO44 / BOX UART RX |
| Pin 8, RX0 | GPIO43 / BOX UART TX, optional and unused by receive-only firmware |

Electrical requirements from the local V1.1 module specification:

- Supply target: 3.3 V, keep it within 3.2-3.4 V for vital-sign measurement.
- Regulator capacity: at least 1 A; module current can reach 600 mA.
- Ripple: no more than 50 mV. If a DC/DC converter is used, switching frequency should
  be at least 2 MHz.
- UART: 115200 baud, 8 data bits, no parity, 1 stop bit, 3.3 V logic.
- The module connector's Pin 3 (P19/BOOT1) must already be low when the radar
  powers up. Connect Pin 3 directly to common GND for normal internal-Flash boot.
  The startup table's Pin8/Pin12 names refer to the radar IC package, not the
  module connector's UART RX0 Pin 8.
- Do not power LD6002 from a DOCK PMOD 3.3 V pin. Use the dedicated regulator and join
  grounds at the power distribution point.


## 3. RGB status light

Current bring-up hardware uses a common-ground three-wire RGB LED. Firmware drives it with LEDC PWM:

| RGB LED signal | Connect to |
|---|---|
| R | GPIO39, through a current-limiting resistor if the LED board has none |
| G | GPIO40, through a current-limiting resistor if the LED board has none |
| B | GPIO41, through a current-limiting resistor if the LED board has none |
| GND / common cathode | Common GND |

The current firmware assumes active-high common-ground wiring: GPIO high turns the corresponding color on. If a later LED module is common-anode or active-low, set `DG_RGB_LED_ACTIVE_LOW` to `1` in `main/app_config.h`.

Planned later hardware may switch to a WS2812/SK6812 one-wire RGB LED. That will require changing the lighting driver back from LEDC PWM to RMT one-wire output and assigning a single data GPIO.

## 4. Two MAX98357A amplifiers

Both amplifier boards share the three I2S signals:

| DOCK signal | Left MAX98357A | Right MAX98357A |
|---|---|---|
| GPIO13 / BCLK | BCLK | BCLK |
| GPIO10 / LRCLK | LRC/LRCLK | LRC/LRCLK |
| GPIO14 / DOUT | DIN | DIN |
| Common GND | GND | GND |
| External 5 V | VIN/VDD | VIN/VDD |

Channel selection on a reference MAX98357A circuit:

- Left amplifier: pull `SD_MODE` directly high to 3.3 V. A 2 kOhm series resistor is
  acceptable.
- Right amplifier: pull `SD_MODE` to 3.3 V through 210 kOhm (1%).
- If the purchased breakout has L/R/MONO solder pads or onboard SD resistors, configure
  those pads according to that board's schematic instead of adding a second resistor network.
- The current firmware outputs standard I2S, stereo, signed 16-bit, 16 kHz. MAX98357A
  does not need MCLK and supports 16 kHz.

Power and speaker rules:

- Feed the two amplifiers from a separate 5 V rail rated for at least 2 A for prototype
  headroom. Add 10 uF + 0.1 uF close to each amplifier board.
- Use two independent 4-8 Ohm speakers. MAX98357A outputs are bridge-tied: never connect
  either speaker negative terminal to ground and never join the two negative terminals.
- Keep speaker wires away from the LD6002 antenna and its 3.3 V supply wiring.
- Start testing at the firmware safety limits (10-16% volume), then confirm temperature
  and acoustic level before increasing it.

## 5. Power tree

Use one qualified 5 V USB supply, 3 A minimum, then split it as follows:

```text
5 V input
+-- ESP32-S3-BOX-3-DOCK power input
+-- left MAX98357A VIN
+-- right MAX98357A VIN
+-- low-noise 3.3 V / >=1 A regulator -> LD6002 3V3

All grounds connected at the distribution point.
```

Do not connect the output of the dedicated 3.3 V regulator back to a DOCK 3.3 V pin.

## 6. Known resource conflicts

- PMOD2 is reserved by external stereo audio in this build; LD6002 uses GPIO43/GPIO44 on the expansion connector.
- The official SDMMC mapping also uses PMOD2 GPIO9-GPIO14/GPIO43/GPIO44. Therefore an
  SD card cannot be connected through this PMOD while radar and stereo are active.
- ESP32-S3-BOX-3-SENSOR and ESP32-S3-BOX-3-DOCK are alternate bottom accessories. This
  prototype uses the DOCK; the SENSOR board's basic presence radar is not part of the
  final vital-sign signal path.
- The board's internal codec/I2S pins remain untouched, leaving the onboard microphones
  and speaker available for later BSP integration.

- The BOX-3 LCD is enabled through the BSP/LVGL display path. Touch input is intentionally not initialized while the prototype uses GPIO40/GPIO41 for the common-ground RGB LED. RGB PWM uses LEDC timer 1, channels 3/4/5 to avoid display backlight LEDC resources.

## 6. Bring-up order

1. Verify the LD6002 rail is 3.2-3.4 V with ripple under 50 mV before inserting the radar.
2. Connect only LD6002 and confirm valid UART frames.
3. Connect one amplifier in left mode and run a low-volume channel test.
4. Add the right amplifier, verify left/right separation, then connect both speakers.
5. Run radar and audio together and check UART frame loss, regulator temperature and radar
   vital-sign stability for at least four hours.

## 7. Sources

- Local DOCK schematic: `ESP32-S3-BOX-3/esp-box/hardware/SCH_ESP32-S3-BOX-3_V1.0/SCH_ESP32-S3-BOX-3-DOCK_V1.0_20230629.pdf`
- Local main-board schematic: `ESP32-S3-BOX-3/esp-box/hardware/SCH_ESP32-S3-BOX-3_V1.0/SCH_ESP32-S3-BOX-3-MB_V1.1_20230808.pdf`
- Local LD6002 specification: `LD6002/HLK-LD6002-呼吸心率检测雷达模组规格书V1.1 .pdf`
- MAX98357A data sheet: https://www.analog.com/media/en/technical-documentation/data-sheets/MAX98357A-MAX98357B.pdf
