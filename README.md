# Remote Master v2

ESP32-C3 remote controller — second revision with web-based UI.

## What's New

- Web frontend for remote configuration (Vue 3 + Vite)
- REST API for device management
- Updated BLE HID implementation

## Hardware

- ESP32-C3-MINI-1
- Bluetooth Low Energy HID

## Getting Started

### Firmware

1. Open `ESP32-C3-Remote.ino` in Arduino IDE or PlatformIO
2. Select board: ESP32-C3 DEV Module
3. Flash to your board

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
