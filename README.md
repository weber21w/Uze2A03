# Uze2A03 — NES APU (2A03) VGM Player for Uzebox

A lightweight 2A03/NES APU player for **Uzebox**, capable of streaming **.VGM** files directly from SD while mixing audio in real time. DMC sample playback is supported with a dedicated cache to keep SPI seeks low.

> **Status:** Working VGM player. **VGZ (gzip) is not yet implemented** (planned).

---

## Features

- 2A03/NES APU emulation: 2× pulse, triangle, noise, and **DMC**.
- Real‑time VGM stream parser (44.1 kHz wait conversion → 15.72 kHz mix).
- **Dual SPI‑RAM caches** (separate VGM/DMC) to minimize SD/PSRAM seeks.
- File browser UI, pause/play/FF, on‑screen timer, skin color cycling.

## Requirements

- **Uzebox** console with **128 KiB SPI RAM** minimum (supports up to 8MB).
- SD card (FAT) readable via Petit FatFs.
- AVR‑GCC toolchain compatible with Uzebox builds.

## Building

1. Make sure your Uzebox SDK/toolchain is installed.
2. Drop the sources into a new project directory.
3. Build as usual (e.g., `make`) to produce the ROM.

## Usage

1. Copy **`.VGM`** files to your SD card. (VGZ is **not** supported yet.)
2. Boot Uzebox, open the file selector, and pick a track.
3. Controls: pause/play/FF, volume, skin. Timer shows MM:SS:ff.

## Technical Notes

- **Mixer rate:** `262 * 60 = 15720 Hz` (Uzebox line rate).  
  CPU cycles per sample ≈ `1789773 / 15720` with remainder carry.
- **VGM timing:** 44.1 kHz “wait” ticks are converted to mixer samples with fractional carry to preserve timing over long runs.
- **DMC:** Implements proper buffer fetch / bit‑shifter behavior, start/loop semantics, and immediate fetch on $4015 enable when the buffer is empty. Uses a **256‑byte** local cache for glitch‑free playback.
- **Frame counter:** 4‑step/5‑step sequencing; envelopes, length, sweep, and triangle linear counter tick at the correct phases.
- **Dual caches:** Separate read‑through caches for VGM stream and DMC payloads so the player rarely seeks back and forth on the medium.

## Roadmap

- **VGZ** (gzip) decompression for compressed tracks.
- Optional filename/title metadata improvements.
- More UI polish / playlist support.

## Credits

- Initial APU structure and variable naming inspired by the ESP32 **“cartridge”** 2A03 project by **Connor Nishijima**.  
  GitHub: https://github.com/connornishijima  
  (Add a direct repo link here if/when confirmed.)
- Uzebox, Petit FatFs, and community resources for platform support.

## License

This repository is licensed under **GPL‑3.0**.  
Please retain the credit above to the original author whose ideas and structure inspired this port. If you copy code from any third‑party repository, ensure you also comply with that project’s license.
