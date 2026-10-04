# CR-10 Wi-Fi Print Server (ESP32-S3, ESP-IDF, bare C)

Custom firmware that turns an ESP32-S3 into a Wi-Fi print server for an original
Creality CR-10 (Melzi / ATmega1284P / Marlin / CH340). There's no Arduino,
ESP3D or OctoPrint involved; it uses only ESP-IDF components.

- Upload G-code from a browser (drag & drop) straight to an SD card on the ESP32.
- The ESP32 streams the file to Marlin over USB (it is the USB host). It uses
  line numbers and checksums, handles resends and recovers lost `ok`s, and
  detects printer resets.
- Live status, temperatures, pause/resume/stop, a G-code console, preheat buttons.
- Wi-Fi STA with a fallback setup access point, `http://cr10.local`, and OTA updates.
- Dry-run mode with a simulated Marlin, for testing without the printer.

---

## 1. Hardware & wiring

```
                 ESP32-S3 dev board (separate 5 V supply)
             ┌──────────────────────────────────────────────┐
  PC  ═══════╡ USB-C "UART" (USB-UART bridge)               │  flashing + logs
             │                                              │
             │ GPIO19 (USB D−) ─────────┐                   │
             │ GPIO20 (USB D+) ───────┐ │                   │
             │ GND ─────────────────┐ │ │                   │
             │                      │ │ │                   │
             │ GPIO10 ── CS   ┐     │ │ │                   │
             │ GPIO11 ── MOSI │ SD  │ │ │                   │
             │ GPIO12 ── SCK  │ card│ │ │                   │
             │ GPIO13 ── MISO │ SPI │ │ │                   │
             │ 3V3/5V ── VCC  │     │ │ │                   │
             │ GND ───── GND  ┘     │ │ │                   │
             └──────────────────────┼─┼─┼───────────────────┘
                                    │ │ │
                    USB-A female:   │ │ └── pin 2  D−  (white)
                                    │ └──── pin 3  D+  (green)
                                    └────── pin 4  GND (black)
                                            pin 1  VBUS (red)  NOT CONNECTED
                                     │
                         standard USB A–B cable
                                     │
                         CR-10 control box USB (CH340 → ATmega1284P)
```

- **Native USB (GPIO19/20) runs in USB host mode.** It cannot be used for
  flashing or logs. Always flash through the **UART** USB-C port.
- **VBUS is intentionally left unconnected.** The printer board powers its own
  CH340, so with the printer off, no device is seen. GND must be shared (it is,
  through the cable).
- **SD card:** FAT32, SPI or SDMMC 1-bit. All pins are set in `menuconfig`
  under *CR-10 Print Server → SD card*. Modules with an on-board regulator or
  level shifter want 5 V on VCC; bare 3.3 V adapters want 3V3. Avoid GPIO19/20
  (USB), 43/44 (UART0), the strapping pins 0/3/45/46, and 35–37 on boards with
  octal PSRAM or flash.
- Default SDMMC 1-bit pins: CLK=12, CMD=11, D0=13.

## 2. Build & flash (terminal, Docker)

You only need **Docker Desktop** (Windows/macOS: with the WSL2 backend) and git.
ESP-IDF runs inside the official `espressif/idf:v5.5.1` image.

### Windows (PowerShell)

```powershell
.\idf.ps1 build                # first run builds the Docker image (~3 GB download)
.\idf.ps1 ports                # find the COM port of the board's UART USB-C port
.\idf.ps1 flash COM5           # flash (esptool.exe is downloaded automatically)
.\idf.ps1 monitor COM5         # serial log, Ctrl+] to quit
.\idf.ps1 flash-monitor COM5
.\idf.ps1 menuconfig           # SD pins, timeouts, temperatures, AP password ...
```

If scripts are blocked: `powershell -ExecutionPolicy Bypass -File .\idf.ps1 build`,
or run `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` once.

How flashing works on Windows: Docker Desktop cannot see COM ports. So the
firmware is **built** in Docker and **flashed** by the standalone
`esptool.exe` (v5.4.0, kept in `tools/esptool/`; no Python needed). The
**monitor** runs `idf.py monitor` in Docker, connected to the board through
`esp_rfc2217_server.exe` on `localhost:4000`. That keeps backtrace decoding.
Windows Firewall may ask about the bridge the first time.

### Linux / macOS

```bash
chmod +x idf.sh
./idf.sh build
./idf.sh flash-monitor /dev/ttyUSB0      # Linux: port passed with --device
./idf.sh flash-monitor /dev/cu.usbserial-XXXX   # macOS: needs `pip install esptool`
```

### VS Code

Open the folder in VS Code with the **Dev Containers** extension installed. VS
Code offers **"Reopen in Container"** (from `.devcontainer/devcontainer.json`).
The container has ESP-IDF, the Espressif extension and C/C++ IntelliSense from
`build/compile_commands.json`. Use *Terminal → Run Task → IDF: build*.

To flash or monitor from inside the container on Windows, first start the
bridge in a normal PowerShell window: `.\idf.ps1 serve COM5`. Then run the task
*IDF: flash + monitor (via host RFC2217 server)*. Running `.\idf.ps1 flash COM5`
from a host terminal also works and is faster.

### Board won't enter download mode?

Hold **BOOT**, tap **RESET**, release BOOT, then flash again.

## 3. First start

1. With no Wi-Fi stored, the ESP32 opens the AP **`CR10-Setup`** (password
   `cr10setup`, set in menuconfig).
2. Connect to it and open **http://192.168.4.1/setup**. Enter your Wi-Fi details
   and save; it reboots.
3. Open **http://cr10.local/**. If your OS lacks mDNS, use the IP printed in the
   serial log.
4. If the stored network can't be joined within 20 s, the setup AP comes back
   up and STA keeps retrying in the background.

## 4. Dry run (no printer)

```powershell
.\idf.ps1 build-dry
.\idf.ps1 flash COM5 -Dry
```

The overlay `sdkconfig.dryrun` replaces the USB transport with a simulated
Marlin. The simulator:

- prints `start` and a banner;
- validates `N…*cs` exactly like Marlin (Error / `Resend:` / `ok`);
- answers M105/M114/M115;
- auto-reports temperatures, models heating, and blocks on M109/M190/G28;
- injects a checksum error every 200 lines, to exercise the resend path.

The web UI shows a DRY RUN banner. An SD card is still needed for uploads.

## 5. How streaming works

| Piece | Behaviour |
|---|---|
| Connect | The CH34x is opened **once** per USB attach. 115200 8N1 is set, and DTR/RTS are set explicitly. Asserting DTR resets the ATmega once (menuconfig option). The firmware waits for Marlin's `start` banner (max 10 s), then sends `M110 N0`, `M115`, `M155 S2`. |
| Lines | `N<n> <cmd>*<xor>`. Comments and whitespace are stripped. Lines over 80 chars have spaces squeezed out (`G1X10Y20`); still too long fails the job. |
| Window | `GS_WINDOW` in `gcode_stream.c` (default **1** line in flight; Marlin's BUFSIZE is 4). |
| `Resend: N` | Rewinds to N and replays from the 32-line history. The `ok`s owed for the rejected lines are swallowed, and the duplicate Resends ignored. |
| `echo:busy:` | Extends the timeout. |
| No `ok` for 10 s | Printer totally silent → reset or power loss assumed → job **failed**, checkpoint kept. Printer still talking (temperature reports) → the oldest line is re-sent once per period; Marlin either runs it or rejects the duplicate. Long commands (M109/M190/G28/G29/M400…) are just waited for. |
| `start` mid-print | Treated as a printer reset → job **failed**, never continued blindly. |
| USB disconnect | Job failed. Re-handshake on reconnect. |
| `Error:` | Logged and shown in the UI. Halting errors (MINTEMP, thermal runaway, `kill()`) fail the job. |
| Temperatures | `M155 S2` auto-report, or M105 polling if the firmware lacks it. |
| Pause | Stops feeding → `M400`, `M114` (position snapshot) → `G91`, retract 2 mm, Z +5 mm, `G90`. Heaters stay on. While paused, jogging, extruding and fans are allowed. |
| Resume | `G90`, back to the snapshot XY, then Z → un-retract → `G92 E<snapshot>` → restores M82/M83, feedrate, G90/G91 → continues with the next line. |
| Stop | `M108` (unnumbered, breaks heat-up waits) → `M104 S0`, `M140 S0`, `M107`, Z +10 mm, `M84`. |
| Checkpoint | `/sdcard/.checkpoint` holds file, acknowledged byte offset and line, and state. It's written at start, every 30 s, on pause, and at the end or on failure. This is the only SD write while printing. |
| Watchdog | The comm/print task is registered with the task watchdog (10 s, panics). |

Tasks: `usb_lib` and `usb_conn` (USB host and connection; core 0), `gs_rx`
(parser) and `gs_comm` (print task; core 1), `httpd` and `ws_push` (core 0).

## 6. REST API

| Method & path | Notes |
|---|---|
| `GET /api/status` | Same JSON as the WebSocket `status` |
| `GET /api/files[?space=1]` | List of files (and card free space; slow on big cards) |
| `GET /api/log` | Last 64 console lines |
| `POST /api/upload?name=&strip=0\|1&ts=<ms>` | Raw body = file. Streamed to SD in 4 KB chunks. 409 while printing |
| `POST /api/delete?name=` | 409 while printing |
| `POST /api/print?name=` | |
| `POST /api/pause` · `/api/resume` · `/api/stop` | |
| `POST /api/gcode` | Body: G-code line(s). During a print only M105/M114/M115/M119/M27/M31/M503/M220/M221 |
| `POST /api/preheat?m=pla\|petg\|cool` | |
| `POST /api/wifi` | Form body `ssid=&pass=` (empty ssid = forget). Reboots |
| `POST /api/ota` | Raw body = `cr10_print_server.bin`. Reboots. Rollback protected |
| `POST /api/reboot` | |
| `GET /ws` | WebSocket push: `{"t":"push","status":{…}\|null,"log":[…]}` |

OTA: upload `build/cr10_print_server.bin` from the System card.

## 7. Project layout

```
CMakeLists.txt  sdkconfig.defaults  sdkconfig.dryrun  partitions.csv (8 MB)  partitions_4mb.csv
Dockerfile  idf.ps1  idf.sh  .devcontainer/  .vscode/tasks.json
main/                 app_main, Kconfig.projbuild (all options)
components/
  storage/            SD mount (SPI/SDMMC, retry), file list/delete, checkpoint
  usb_printer/        USB host + CH34x VCP (usb_printer_ch34x.c) | simulator (usb_printer_sim.c)
  gcode_stream/       print/comm task + protocol (gcode_stream.c), RX parser (gcode_rx.c)
  net/                Wi-Fi STA/AP, NVS credentials, mDNS, SNTP
  web/                HTTP server, REST, WebSocket, upload, OTA, www/index.html, www/setup.html
```

**4 MB flash:** set `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` and
`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_4mb.csv"` in
`sdkconfig.defaults`, then `.\idf.ps1 clean` and rebuild. The app is about
0.95 MB, which fits the 1.25 MB slots.

## 8. Components & versions

| | Version |
|---|---|
| ESP-IDF | v5.5.1 (Docker image `espressif/idf:v5.5.1`); code targets ≥ 5.3 |
| `espressif/usb_host_ch34x_vcp` | 2.2.1 (C API `ch34x_vcp_open()`, PID auto-detect, DTR/RTS polarity fix) |
| `espressif/usb_host_cdc_acm` | 2.4.1 (pulled in by the above) |
| `espressif/usb` | 1.x managed USB host stack (pulled in by cdc_acm, replaces IDF's built-in `usb`) |
| `espressif/mdns` | 1.14.0 |
| esptool (host, Windows) | v5.4.0 standalone |

The exact versions are pinned in `dependencies.lock` after the first build.

## 9. Known limitations

- **Not yet tested on real hardware.** Both variants compile cleanly (no
  warnings) against the versions above, but nothing has run on a board or
  against a real CR-10. Test with the dry run first, then with a short print.
- **Stop during M109/M190:** breaking the wait needs Marlin's
  `EMERGENCY_PARSER`. Stock CR-10 firmware may not have it; then the stop runs
  after heating finishes.
- **The ESP32 rebooting mid-print** (power loss, crash, OTA) re-opens the port.
  With DTR enabled that resets Marlin and the print is lost. That's the same as
  any USB host. The checkpoint shows where it stopped. There is deliberately no
  automatic resume from a checkpoint.
- **Window = 1 line.** That's safe; raise `GS_WINDOW` to 2–3 if very
  segment-dense files stutter. The Melzi itself is usually the limit.
- **Single-threaded HTTP server.** Live updates pause during an upload or OTA,
  and other requests wait. Upload speed is limited by SD over SPI, typically
  300–800 KB/s.
- **Files:** root folder only, names up to 64 characters, no subfolders. Card
  removal while mounted isn't detected; a missing card at boot is retried every
  5 s.
- **File dates** need SNTP (internet) or the browser's clock, which is sent
  with each upload.
- **No authentication** on the web UI or API. Keep it on a trusted network and
  change the setup AP password.
- **No captive portal** in AP mode; browse to `192.168.4.1/setup` yourself.
- While the setup AP fallback is active, STA retries (with backoff up to 60 s)
  can briefly disturb AP clients.
- The upload "strip" option removes only comment-only and blank lines. Inline
  comments are always stripped at print time.
- Fatal-error detection matches Marlin message text (MINTEMP, Thermal, kill…);
  unusual firmware wording may only be logged.
