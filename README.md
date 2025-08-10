# ESP32 Hurricane Tracking Display

This project creates a simple hurricane tracking display for ESP32S3-8048S070C (7") display board. It connects to WiFi, downloads the latest hurricane forecast map from the National Hurricane Center (NHC), and displays it on the 800x480 LCD panel. Additionally, it will scan the latest xml feed to download cone/warning images of any active storms.


## Prerequisites

- ESP-IDF v5.5 or later
- ESP32-S3 with 7" RGB LCD panel (touch, or w/o touch)

## Hardware

- ESP32-S3-8048S070C display board
- WiFi connectivity

## Build and Flash

1. Configure the project with your WiFi credentials and other settings:

```bash
idf.py menuconfig
```

Navigate to "Hurricane Tracking Configuration" and configure the following options:

### Available Configuration Options

- **WiFi SSID**: Network name for the device to connect to
- **WiFi Password**: WiFi password (WPA or WPA2) for network access
- **Enable Touchscreen Support**: Enable/disable touchscreen initialization and touch-based backlight control. Disable this for hardware versions that lack a touchscreen (default: enabled)
  - **Enable PIR Sensor**: Rather than keep the touch-less version backlight on all the time, a PIR (AS312) sensor can be used to turn on the backlight.
- **Time Synchronization Method**: Choose between two methods for time synchronization:
  - **SNTP (Network Time Protocol)**: Standard network time synchronization (requires port 123 access) - Default
  - **WorldTimeAPI (HTTP)**: HTTP-based time synchronization via WorldTimeAPI.org (useful when port 123/NTP is blocked)
- **Conversion API URL**: URL of the image conversion API (configure privately, not committed to repository).  See https://github.com/gonesurfing/lv_img_conv_api for a deployable api example.

2. Build the project:

```bash
idf.py build
```

3. Flash the project:

```bash
idf.py flash
```

4. Monitor serial output:

```bash
idf.py monitor
```

## Implementation Notes

The current implementation relies on an external API to do image scaling and conversion.

## Future TODO

- Code consolidation, cleanup. main.c is too large.
- Better way of starting tasks.
- There's definitely more configurable options to break out to menuconfig or app_config.h
- Download all images from RSS xml, rather than static.
- Test/Implement smaller CYD screens