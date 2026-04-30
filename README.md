# BLE Acoustic Turntable — ECE 428

Bluetooth-controlled stepper motor turntable for rotating a ReSpeaker 6-Mic Circular Array at precise angular increments for acoustic localization research.

## Hardware

| Component | Part | Purpose |
|---|---|---|
| Microcontroller | ESP32 Dev Board | BLE communication + stepper control |
| Stepper driver | DRV8825 | Motor current control |
| Stepper motor | FLSUN 42SHDC40472-23B | Rotation (1.8°/step) |
| Microphone array | ReSpeaker 6-Mic Circular | Acoustic data capture |
| Power (motor) | 3S LiPo 11.1V 5200mAh | Motor supply |
| Power (logic) | USB power bank 5V | ESP32 supply |
| Capacitor | 100µF 35V electrolytic | VMOT spike suppression |
| Boost/buck | MT3608 or equivalent | Voltage regulation if needed |

## Wiring

```
ESP32 GPIO18  →  DRV8825 STEP
ESP32 GPIO19  →  DRV8825 DIR
ESP32 GPIO21  →  DRV8825 EN
ESP32 GPIO22  →  DRV8825 MODE0
ESP32 GPIO23  →  DRV8825 MODE1
ESP32 GPIO5   →  DRV8825 MODE2
ESP32 3.3V    →  DRV8825 VDD, RST, SLP
ESP32 GND     →  Shared GND rail
LiPo 11.1V    →  DRV8825 VMOT
GND           →  DRV8825 GND (motor side)
100µF cap     →  Across VMOT and GND (at DRV8825 pins)
DRV8825 1A/1B →  Motor Coil 1
DRV8825 2A/2B →  Motor Coil 2
```

Microstepping: MODE0=LOW, MODE1=HIGH, MODE2=LOW = 1/4 step
Resolution: 800 steps/rev = 0.45°/step

## Software Setup

### Requirements
- PlatformIO
- ESP32 board package
- ESP32 BLE Arduino library

### platformio.ini
```ini
[env:esp32dev]
platform  = espressif32
board     = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps =
  ESP32 BLE Arduino
```

### Build and Flash
```bash
pio run -t upload
pio device monitor
```

## BLE Commands

Connect to device named **Turntable** using any Bluetooth-terminal for ios/android/pc

### Movement
| Command | Description |
|---|---|
| `STEP` | Advance one increment (default 2°) |
| `STEP:N` | Set increment to N° and advance |
| `BACK` | Reverse one increment |
| `BACK:N` | Reverse N degrees |
| `GOTO:N` | Move to absolute angle 0–360° |
| `RESET` | Return to 0° via shortest path |
| `ZERO` | Set current position as software 0° |

### Sweep / Scan
| Command | Description |
|---|---|
| `SWEEP` | Timer-based auto sweep 0→360° |
| `SCAN:N` | Synchronized scan in N° steps |
| `STOP` | Halt any active sweep or scan |
| `ACK` | Advance to next scan position (SCAN mode only) |

### Settings
| Command | Description |
|---|---|
| `SPEED:N` | Step delay in µs (100–10000, default 800) |
| `DWELL:N` | Sweep pause per position in ms (100–60000, default 1000) |
| `SETSTEP:N` | Set default increment without moving |
| `DIR:CW` | Force clockwise approach (reduces backlash) |
| `DIR:CCW` | Force counter-clockwise approach |
| `DIR:AUTO` | Shortest path (default) |
| `HOLD:ON` | Keep motor energized between moves (default) |
| `HOLD:OFF` | Disable motor between moves (runs cooler) |

### Info
| Command | Description |
|---|---|
| `STATUS` | Returns angle, step, speed, dwell, run, mode, dir, hold |
| `INFO` | Returns firmware version and motor config |

## SCAN Mode — Data Collection Flow

SCAN mode is the primary mode for acoustic data collection. The ESP32 and host stay synchronized — the motor never moves while the mic is recording.

```
Host  →  ESP32:  SCAN:2
ESP32 →  Host:   READY:0.00,TS:12345,STEP:2.00,RUN:1
[Host starts recording]
Host  →  ESP32:  ACK
ESP32 →  Host:   READY:2.00,TS:14201,STEP:2.00,RUN:1
[Host starts recording]
Host  →  ESP32:  ACK
... (180 positions for full 360° at 2° steps)
ESP32 →  Host:   SCAN_DONE,RUN:1
```

Each READY packet contains:
- Current angle in degrees
- Timestamp in milliseconds (millis() since boot)
- Step size
- Run number (auto-increments each SCAN or SWEEP)

## Additional Resources
- [[https://lastminuteengineers.com/drv8825-stepper-motor-driver-arduino-tutorial/]]
- [[https://www.monolithicpower.com/en/learning/resources/stepper-motors-basics-types-uses]]

## Advisor
Dr. Anthony Choi — Mercer University, Department of ECE

## Acknowledgements
This project was supported by NASA.
