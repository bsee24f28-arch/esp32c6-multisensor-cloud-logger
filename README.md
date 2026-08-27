# ESP32-C6 Multi-Sensor Cloud Logger

A complete embedded systems project built on the **ESP32-C6**, combining ultrasonic distance sensing, IMU motion sensing, local SD card logging, and real-time cloud visualization — all in one working pipeline.

## Overview

This project reads live sensor data from an ultrasonic distance sensor and a 6-axis IMU, logs every reading locally to an SD card, and periodically streams an aggregated summary to a cloud dashboard (ThingSpeak) over WiFi — built using FreeRTOS on ESP-IDF.

## Features

- 📏 **Real-time distance sensing** via HC-SR04 ultrasonic sensor (GPIO-based timing)
- 📐 **6-axis motion sensing** via MPU6050 IMU (accelerometer + gyroscope over I2C)
- 💾 **Full-resolution local logging** to an SD card over SPI (CSV format)
- ☁️ **Cloud dashboard integration** via WiFi + HTTP to ThingSpeak
- ⚙️ **Edge aggregation** — averages readings locally before upload, respecting cloud API rate limits
- 🧵 Built on **FreeRTOS** task/delay model for reliable real-time behavior

## Hardware Components

| Component | Role |
|---|---|
| ESP32-C6 | Main controller |
| HC-SR04 | Ultrasonic distance sensor |
| MPU6050 | 6-axis IMU (accelerometer + gyroscope) |
| SD card module | Local data storage |

## Wiring / Pinout

| Component | Pin | Connects to |
|---|---|---|
| HC-SR04 | VCC | 5V |
| | GND | GND |
| | TRIG | GPIO22 |
| | ECHO | GPIO23  |
| MPU6050 | VCC | 3V3 |
| | GND | GND |
| | SDA | GPIO6 |
| | SCL | GPIO7 |
| SD card reader | VCC | 5V |
| | GND | GND |
| | MOSI | GPIO18 |
| | MISO | GPIO19 |
| | SCK | GPIO20 |
| | CS | GPIO21 |

## Protocols Used

- **GPIO** — trigger/echo timing for HC-SR04
- **I2C** — communication with MPU6050
- **SPI** — communication with SD card module
- **WiFi + HTTP** — uploading aggregated data to ThingSpeak

## Software Stack

- ESP-IDF (FreeRTOS-based)
- C
- ThingSpeak cloud API

## Getting Started

### Prerequisites
- ESP-IDF installed and configured
- ESP32-C6 development board
- Hardware wired as per the table above

### Cloud Setup
1. Create a free [ThingSpeak](https://thingspeak.com) account
2. Create a new channel with 7 fields (distance, accel X/Y/Z, gyro X/Y/Z)
3. Copy your channel's **Write API Key** into the WiFi/upload section of `main.c`
4. Update the WiFi SSID/password in the code
5. Flash the firmware — data will begin appearing on your ThingSpeak dashboard within a few upload cycles

## Data Flow

1. Sensors are read every ~1 second
2. Every reading is immediately logged to `sensor_log.csv` on the SD card (full resolution)
3. Readings are also accumulated in memory
4. Every ~15–20 seconds, the averaged values for all 7 fields are sent in a single HTTP request to ThingSpeak



## Acknowledgements

This project was carried out at **CAID – Namal**, under the guidance of **Dr. Tassaduq Hussain**, Director of CAID, with technical mentorship from **Ms. Mushafia Sadia**.

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
