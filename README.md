# Remote Master v2

ESP32-C3 remote controller — second revision with web-based UI.

## What's New

- Web frontend for remote configuration (Vue 3 + Vite)
- REST API for device management
- Updated BLE HID implementation

## Hardware

- ESP32-C3-MINI-1
- Bluetooth Low Energy HID
- IR receiver on GPIO5; IR transmitter on GPIO4

## IR Transmission

Select the GPIO4 drive mode in Settings > IR and save:

| `irTxMode` | Output |
| --- | --- |
| `off` (default) | High impedance, no internal pull-up/down |
| `push-pull` | Active high carrier, idle low |
| `sink` | Active low open-drain carrier, idle released |

Use a current-limiting resistor and keep GPIO current within the ESP32-C3
ratings. Use a transistor driver for higher LED currents. GPIO4 is not
5V tolerant; sink-mode circuits must not pull it above 3.3V.

The Send button beside Delete on each learned-key card replays its saved
capture. IR mappings and IR-only sequences (replay/wait) use the same sender.
Missing captures must be learned again. Sending does not require a BLE host.
Reception is paused during an IR action, including sequence waits, and resumes
after a 20 ms settling interval. Learning remains active when sending from a card.
Saving a drive mode, starting/stopping learning, stopping actions, or resetting
the system cancels any running IR action.

RMT generates a 33% duty carrier and replays one captured frame with receiver
mark-excess correction. A saved `carrierHz` takes precedence; otherwise Sony
uses 40 kHz, RC5/RC6 uses 36 kHz, Panasonic/Kaseikyo uses 37 kHz, Bang & Olufsen
uses 455 kHz, and other/unknown protocols use 38 kHz. The receiver records only
the envelope, not the carrier. Automatic protocol repeats, toggle-bit updates,
and multi-frame reconstruction are not performed.

### WebSocket Commands

Requests use `{id, op, args}`; replies echo `id` with `ok` and optional `error`.

- `settings.save`, `args: {irTxMode: "off" | "push-pull" | "sink"}`:
  persist and immediately apply the mode after a successful save.
- `keys.send`, `args: {id: "<learned-key-id>"}`: start one frame asynchronously.
  A successful reply means accepted, not confirmation from the target appliance.
- `mappings.test` / `macros.test`: also support IR-only actions.
- `hid.stop`: cancel either BLE or IR output.
- State events expose `config.settings.irTxMode` and `status.irTxRunning`.
  An asynchronous transmission failure emits `{event: "ir.error", error: "..."}`.

Captures must use version 1 `alternating-mark-space-ticks`, start with a mark,
have no overflow, and contain 3-749 positive ticks. Files are limited to 16 KiB,
pulses to 100 ms, and frames to 1 second; IR sequences are limited to 60 seconds.

## Getting Started

### Configuration Capacity

Learned keys and mappings have no fixed entry-count limit. They share a 32 KiB
configuration JSON budget with settings and other collections; raw IR captures
are stored separately. Available RAM and filesystem space also limit capacity.
The device supports up to 12 remotes, 8 BLE hosts, and 50 macros.

Older firmware enforced a global 50-key limit across all remotes. Upgrade the
firmware to learn more keys; no reset or relearning of existing keys is needed.
Do not downgrade to firmware with the old limit after storing more than 50 keys:
its startup validation can reject the configuration and format the filesystem.

### Firmware

1. Open `remote-master.ino` in Arduino IDE
2. Select board: ESP32-C3 DEV Module
3. Select Partition Scheme: No OTA (2MB APP/2MB SPIFFS)
4. Flash to your board

The default 1.2 MB application partition is too small. Back up configuration
and captures before changing an existing device's partition layout.

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=no_ota .
```

### Web UI

```bash
cd web/
npm install
npm run dev     # development
npm run build   # production build
```

## Project Structure

- `*.ino`, `*.cpp`, `*.h` — firmware source
- `web/` — Vue 3 web frontend
- `data/` — web assets served by ESP32
- `deploy.mjs` — build & deploy script

## License

MIT
