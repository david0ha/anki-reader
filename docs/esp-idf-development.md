# ESP-IDF Development Guide

The workflow from environment activation through build/flash/debug to the peripheral drivers.

## 1. Environment activation

ESP-IDF is installed under `~/esp/`. Run this **every time you open a new terminal** so `idf.py`
lands on PATH:

```bash
. ~/esp/v5.4.3/esp-idf/export.sh
```

Verify:

```bash
idf.py --version      # ESP-IDF v5.4.3
```

> Older notes in this repository referenced a `~/.espressif/tools/activate_idf_v6.0.1.sh` script and
> ESP-IDF v6.0.1. That script does not exist on this machine; v5.4.1 and v5.4.3 are what is
> installed, and v5.4.3 is what the firmware is verified against. If you move to v6, note that it
> removed cJSON from core — this project already vendors it at `third_party/cJSON`, so that
> particular migration is a non-issue.

## 2. Creating a project

```bash
# Copy-an-example approach
cp -r $IDF_PATH/examples/get-started/hello_world my_app
cd my_app

# Or an empty project
idf.py create-project my_app && cd my_app
```

## 3. Setting the target (once per project)

```bash
idf.py set-target esp32s3
```

## 4. Board configuration (menuconfig)

```bash
idf.py menuconfig
```

Items you must verify for this board:

- **Serial flasher config → Flash size** → `16 MB`
- **Component config → ESP PSRAM**
  - Enable `Support for external, SPI-connected RAM`
  - `SPI RAM config → Mode` → **Octal Mode PSRAM**
  - Speed 80MHz (if needed)
- **Component config → ESP System Settings → Channel for console output**
  - When using the direct USB-C connection, you can select `USB Serial/JTAG Controller`
- Partition Table: to make use of the 16MB flash, a custom `partitions.csv` is recommended
  (app + SPIFFS/FAT + OTA, etc.).

> Writing the settings above into `sdkconfig.defaults` makes them reproducible across the team/CI. Example:
> ```
> CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
> CONFIG_SPIRAM=y
> CONFIG_SPIRAM_MODE_OCT=y
> CONFIG_SPIRAM_SPEED_80M=y
> ```

## 5. Build / flash / monitor

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

- Exit the serial monitor: `Ctrl + ]`
- Finding `<PORT>` (macOS):
  ```bash
  ls /dev/cu.*
  ```
  - Native USB Serial/JTAG: `/dev/cu.usbmodem*`
  - UART bridge (CH34x/CP210x, etc.): `/dev/cu.usbserial-*` / `/dev/cu.wchusbserial*`
- If entering flash mode fails: **hold the BOOT button while clicking RESET → release BOOT**, then retry.
- Individual steps: `idf.py build`, `idf.py flash`, `idf.py monitor`, `idf.py app-flash`, etc.

## 6. Debugging (JTAG / USB Serial/JTAG)

The ESP32-S3 has a built-in native USB Serial/JTAG, so OpenOCD/GDB debugging is possible without an extra adapter:

```bash
idf.py openocd            # Start the OpenOCD server (in a separate terminal)
idf.py gdb                # Connect with GDB
# Or all at once
idf.py openocd gdbgui
```

- It uses the Espressif-patched OpenOCD build included with ESP-IDF (no separate installation needed).
- In VS Code, you can configure debugging with the "Espressif IDF" extension + `idf.py`.

## 7. Peripheral drivers (ESP-IDF components)

See [pinout.md](pinout.md) for the pin map. Components are fetched from the ESP Component Registry
(`idf.py add-dependency "<name>"`).

### Display (UC8179 e-Paper, SPI, 648×480 monochrome)

Written in-house at `components/port_bsp/epd_panel.c`. There is no official `esp_lcd` component for
the UC8179 — do not go looking for one. It uses `esp_lcd`'s SPI panel IO for transport and owns its
own 38,880-byte framebuffer and refresh policy.

Full rationale, the command sequence's provenance, and the refresh rules are in
[epaper-5in83.md](epaper-5in83.md). Read that before touching the driver, and note the one thing
most likely to waste your afternoon: **BUSY is active LOW on this controller**, the inverse of the
SSD1680 this code started as, and getting it backwards fails silently rather than loudly.

### Not present on this build

No touch controller, audio codec, SD card or RTC. There is also **no I2C bus**: on the EE04 the two
pins the previous carrier routed to an I2C header are KEY2 and the battery divider's enable. Adding
an I2C device means finding two free pins first — see [pinout.md](pinout.md).

## 8. Summary of frequently used commands

```bash
. ~/esp/v5.4.3/esp-idf/export.sh   # environment
idf.py set-target esp32s3        # target
idf.py menuconfig                # configuration
idf.py build                     # build
idf.py -p <PORT> flash monitor   # flash + monitor
idf.py fullclean                 # clean the build cache
idf.py size                      # memory usage
idf.py add-dependency "<comp>"   # add a component
```

Before claiming a change works, also run the host tests and the simulator — see
[the verification section of CLAUDE.md](../CLAUDE.md#verify-before-claiming-anything-works).

## References

- [references.md](references.md) — the e-Paper driver's upstream source, the EE04's pin routing as
  published by Seeed, and the font licence.
