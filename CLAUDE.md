# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PolyPlug is an open-source smart plug firmware for ESP32-C5, built on ESP-IDF v6.0. It receives commands from a [PolyCast5](https://github.com/RoboticWorx/PolyCast5) remote control over LoRa, ESP-NOW, or MQTT to toggle a 15A relay, set schedules/modes, and drive a 5-bit GPIO bus for custom builds. Written in C.

## Build Commands

```bash
idf.py build                # Build
idf.py -p PORT flash        # Flash to device
idf.py -p PORT monitor      # Serial monitor
idf.py fullclean            # Full clean (required after ESP-IDF patches or sdkconfig changes)
idf.py menuconfig           # Interactive config editor
```

There is no test framework; validation is done by flashing to hardware.

## Hardware & Memory

- **Target**: ESP32-C5, 8MB flash (DIO/80MHz), 8MB PSRAM (quad/80MHz)
- **Partition layout** (`partitions.csv`): NVS (6KB), OTA data (8KB), PHY (4KB), two 4.13MB OTA slots
- **Memory strategy**: PSRAM enabled via `CONFIG_SPIRAM_USE_MALLOC`, with BSS/NOINIT/stack external memory allowed; 32KB internal reserved (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`)
- **Key peripherals**: SPI2 (SX1262 LoRa), ADC channel 4 (battery), direct GPIO (relay, RGB LED, 5-bit bus, buttons)

## Architecture

The firmware is task-based. `main/main.c` initializes hardware (NVS, Wi-Fi driver, SPI2 bus, GPIOs) then spawns four independent FreeRTOS tasks:

| Task | Stack | Pri | Component | Purpose |
|------|-------|-----|-----------|---------|
| gpio_task | 2KB | 1 | `components/gpio` | Relay control, RGB LED, 5-bit GPIO bus output, plan/mode scheduling, battery ADC |
| lora_task | 2KB | 1 | `components/lora` | SX1262 init, arms RX mode; spawns `lora_event_handler_task` (3KB, pri 3) for DIO1 ISR events |
| espnow_task | 3KB | 1 | `components/espnow` | Receives ESP-NOW data from PolyCast5: LoRa encryption keys and MQTT network config |
| wifi_task | 5KB | 1 | `components/wifi` | Wi-Fi STA connection, MQTT client (subscribe `polycast5/{key}/cmd`, publish `polycast5/{key}/ack`), OTA updates |

**Dynamic tasks** spawned at runtime:
- `plan_mode` tasks (up to 10 via `plan_tasks[]`) — scheduled relay timers
- `ota_check_task` (6KB) — polls GitHub for firmware updates
- `ota_task` (8KB) — downloads OTA binary via HTTPS

### LoRa Protocol (PCP — Poly Cipher Protocol)

Encrypted LoRa messaging between PolyCast5 and PolyPlug:
- **Encryption**: AES-128-CCM with 4-byte MIC, 13-byte random nonce
- **Replay protection**: per-message counter stored in NVS
- **Message types**: Command (38B plaintext) and ACK (5B plaintext)
- **On-air format**: Nonce (13B) + Ciphertext (up to 38B) + MIC (4B) = 55B max
- **Radio settings**: SF7, BW125, CR4/5, preamble 12 symbols, explicit header

### Synchronization

**Mutex**: `xPcpMutex` — protects AES-CCM encryption key and replay counter

**Semaphores**: `xLoraEventSemaphore` (DIO1 ISR → lora_task), `xTXDoneSemaphore` (TX complete), `xWifiReconnectSemaphore` (ESP-NOW → wifi_task)

**Queues**: `xRelayToggleQueue` (relay commands), `xEspReceivedEncKeyQueue` (LoRa key from ESP-NOW), `xWifiConnectQueue` (MQTT network config)

## Pin Assignments

| Pin | Function | Direction |
|-----|----------|-----------|
| 25 | Relay (15A AC) | Output |
| 8, 9, 10 | RGB LED (R/G/B) | Output |
| 1, 0, 3, 2, 26 | 5-bit GPIO bus | Output |
| 6, 7 | Pairing buttons | Input |
| 24, 12, 23 | SPI2 (MOSI/SCLK/MISO) | — |
| 11 | SX1262 CS | Output |
| 27, 4, 5 | SX1262 (RST/BUSY/DIO1) | Mixed |
| ADC CH4 | Battery voltage | Input |

## Key Shared Headers

- `components/common/include/polyplug_macros.h` — Debug flag (`POLYPLUG_DEBUG`)
- `components/gpio/include/gpio_funcs.h` — All pin definitions, `relay_t` struct, GPIO/SPI/ADC helpers
- `components/lora/include/lora_pcp.h` — PCP protocol constants and encryption API

## Code Style

- Preserve existing comment style (`/** */` vs `//`) and formatting in surrounding code
- Keep functions separated with blank lines consistent with adjacent code
- Wrap lines consistently with the file's existing style
