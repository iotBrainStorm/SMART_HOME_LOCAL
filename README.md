# SMART_HOME_LOCAL

![ESP32](https://img.shields.io/badge/ESP32-Controller-ff6f00?style=for-the-badge&logo=espressif&logoColor=white)
![Web Dashboard](https://img.shields.io/badge/Web-Dashboard-0288d1?style=for-the-badge&logo=googlechrome&logoColor=white)
![Automation](https://img.shields.io/badge/Automation-Timer%20%7C%20Schedule%20%7C%20Sensor-2e7d32?style=for-the-badge)
![Firebase Ready](https://img.shields.io/badge/Firebase-Ready-f57c00?style=for-the-badge&logo=firebase&logoColor=white)
![License](https://img.shields.io/badge/License-Custom-d81b60?style=for-the-badge)

A modern ESP32 Smart Home controller with a full web dashboard, rich automation engine, user/admin management, WiFi networking tools, and Firebase integration.

![Dashboard](screenshots/controlDashboard.png)

## What This Project Delivers

- 4-channel switch control with physical input + web control
- Real-time web dashboard with feature status indicators
- Automation suite: Timer, Schedule, Future Schedule, Temperature, Humidity, Sunrise/Sunset
- Automation priority engine (resolve conflicts by your chosen order)
- Full user management with admin verification
- WiFi setup + status + forget + DHCP/static IP configuration
- Firebase URL/token/rules/auth user management
- Admin utilities: device name, restart schedule, location setup, reset workflows
- Strong reset UX with BOOT-hold protection and checklist progress

## Menu and Submenu Guide

The settings page is organized into tabs with clear submenu panels.

### 1) Switches Tab

| Menu        | Description                                           | Screenshot                                        |
| ----------- | ----------------------------------------------------- | ------------------------------------------------- |
| Switch Name | Rename each output channel for human-friendly control | ![Switch Names](screenshots/switchName.png)       |
| Switch Icon | Assign icon per switch for visual clarity             | ![Switch Icons](screenshots/switchiconChange.png) |
| Relay State | Configure startup behavior (off/on/remember)          | ![Relay State](screenshots/switchState.png)       |

### 2) Automation Tab

| Menu / Submenu                      | Description                                              | Screenshot                                                     |
| ----------------------------------- | -------------------------------------------------------- | -------------------------------------------------------------- |
| Add Timer                           | Add per-switch countdown actions, live remaining seconds | ![Timer](screenshots/timer.png)                                |
| Set Schedule                        | Create recurring time-window schedules                   | ![Add Schedule](screenshots/addSchedule.png)                   |
| Show Schedules                      | View/edit/remove recurring schedules                     | ![Show Schedules](screenshots/showSchedules.png)               |
| Future Schedule                     | One-time date/time-window automation                     | ![Add Future](screenshots/addFutureSchedules.png)              |
| Show Future Schedules               | View/edit/remove one-time schedules                      | ![Show Future](screenshots/showFutureSchedules.png)            |
| Sensor Control > Temperature        | Trigger actions from temperature conditions              | ![Temp Automation](screenshots/addTemperatureAutomation.png)   |
| Sensor Control > Humidity           | Trigger actions from humidity conditions                 | ![Sensor List](screenshots/showSensorAutomations.png)          |
| Sensor Control > Sunset and Sunrise | Trigger actions by solar events + offsets                | ![Sun Automation](screenshots/addSunriseSunsetAutomations.png) |
| Sensor Automation List              | Review configured sensor automations                     | ![Show Sensor](screenshots/showSensorAutomations.png)          |

### 3) WiFi Tab

| Menu                 | Description                                    | Screenshot                                           |
| -------------------- | ---------------------------------------------- | ---------------------------------------------------- |
| Connect WiFi         | Save and connect to a router (admin protected) | ![Connect WiFi](screenshots/connectWifi.png)         |
| Network Status       | Check SSID/IP/subnet/gateway/DNS/RSSI          | ![Network Status](screenshots/wifiNetworkStatus.png) |
| Forget WiFi          | Remove saved credentials safely                | ![Forget WiFi](screenshots/forgetWifi.png)           |
| IP Settings (DHCP)   | Use router-assigned addressing                 | ![DHCP IP](screenshots/dhcpIP.png)                   |
| IP Settings (Static) | Configure static IP, gateway, DNS              | ![Static IP](screenshots/staticIP.png)               |

### 4) Firebase Tab

| Menu / Submenu              | Description                                         | Screenshot                                         |
| --------------------------- | --------------------------------------------------- | -------------------------------------------------- |
| On / Off                    | Enable/disable cloud sync mode                      | ![Firebase Toggle](screenshots/firebaseUrl.png)    |
| Database URL                | Save and validate RTDB URL                          | ![Firebase URL](screenshots/firebaseUrl.png)       |
| Auth Token                  | Securely store DB token (masked/reveal workflow)    | ![Firebase Secret](screenshots/firebaseSecret.png) |
| DB Rules                    | View/update Firebase security rules JSON            | ![Database Rules](screenshots/databaseRules.png)   |
| Authentication > Add User   | Add local Firebase auth users for provisioning flow | ![Add Firebase User](screenshots/addFBUsers.png)   |
| Authentication > Show Users | Review/remove saved Firebase auth users             | ![Firebase Users](screenshots/firebaseUsers.png)   |

### 5) User Tab

| Menu        | Description                       | Screenshot                                         |
| ----------- | --------------------------------- | -------------------------------------------------- |
| Show Users  | List all registered ESP users     | ![Show ESP Users](screenshots/showESPUsers.png)    |
| Add User    | Add normal user accounts          | ![Add ESP User](screenshots/addESPUsers.png)       |
| Remove User | Remove normal user accounts       | ![Remove ESP User](screenshots/removeESPUsers.png) |
| Edit Admin  | Change admin ID/password securely | ![Edit Admin](screenshots/editAdmin.png)           |

### 6) Admin Tab

| Menu                | Description                                                             | Screenshot                                             |
| ------------------- | ----------------------------------------------------------------------- | ------------------------------------------------------ |
| Device Name         | Set hostname/device identity                                            | ![Device Name](screenshots/changeDeviceName.png)       |
| Schedule Priority   | Choose automation conflict priority                                     | ![Schedule Priority](screenshots/schedulePriority.png) |
| Time Setup          | Configure NTP server and timezone                                       | ![Time Setup](screenshots/timeSetup.png)               |
| Location Setup      | Set latitude/longitude and view sunrise/sunset                          | ![Location Setup](screenshots/locationSetup.png)       |
| Restart             | Configure weekly restart + manual restart                               | ![Restart Setup](screenshots/restartSetup.png)         |
| Restart Progress UI | Live restart countdown/overlay flow                                     | ![Restart UI](screenshots/restartUI.png)               |
| Reset Storage       | Clear names/icons/schedules/sensor automation/priority                  | ![Storage Reset](screenshots/storageReset.png)         |
| Reset Settings      | Clear relay startup, firebase state, time settings, static IP, location | ![Settings Reset](screenshots/settingsReset.png)       |
| Factory Reset       | Full wipe (storage + settings + users + wifi + firebase)                | ![Factory Reset](screenshots/hardReset.png)            |
| Reset Progress UI   | Step-by-step reset progress with safety timing                          | ![Reset UI](screenshots/resetUI.png)                   |

## Dashboard Overview

- Login screen with remember-me and protected admin reset flow
- 4 smart cards for device control
- Per-card feature indicators: timer, schedule, future schedule, sensor automation
- All ON / All OFF controls

![Login](screenshots/login.png)

## Core Technical Features

- ESPAsyncWebServer based API and static web app delivery
- Preferences (NVS) storage across namespaces: sw, wfcfg, fb, admin, users, sched, fsched, sensor
- Sunrise/sunset calculations using Dusk2Dawn + geolocation + timezone
- AHT10 integration for temperature/humidity automation
- Safe admin actions protected by credential checks and BOOT-hold flow
- Rich reset pipeline with timed checkpoints and restart orchestration

## Hardware and Wiring

### Required Hardware

- ESP32 dev board
- 4-channel relay module
- 4 physical switches/buttons for local input
- AHT10 sensor module (I2C)
- Status LED and buzzer (optional but supported)
- Stable 5V/3.3V power supply with common ground

### GPIO Mapping

| ESP32 Pin | Function                              |
| --------- | ------------------------------------- |
| GPIO18    | Relay Output 1                        |
| GPIO19    | Relay Output 2                        |
| GPIO23    | Relay Output 3                        |
| GPIO5     | Relay Output 4                        |
| GPIO34    | Physical Input Switch 1               |
| GPIO35    | Physical Input Switch 2               |
| GPIO36    | Physical Input Switch 3               |
| GPIO39    | Physical Input Switch 4               |
| GPIO16    | Status LED                            |
| GPIO17    | Buzzer                                |
| GPIO0     | BOOT hold button (admin reset safety) |
| GPIO21    | I2C SDA (AHT10, default)              |
| GPIO22    | I2C SCL (AHT10, default)              |

### Circuit Diagram (Logical)

```mermaid
graph TD
	ESP32[ESP32 Dev Board]
	R1[Relay CH1]
	R2[Relay CH2]
	R3[Relay CH3]
	R4[Relay CH4]
	S1[Wall Switch 1]
	S2[Wall Switch 2]
	S3[Wall Switch 3]
	S4[Wall Switch 4]
	AHT[AHT10 Sensor]
	LED[Status LED]
	BZ[Buzzer]

	ESP32 -->|GPIO18| R1
	ESP32 -->|GPIO19| R2
	ESP32 -->|GPIO23| R3
	ESP32 -->|GPIO5| R4

	S1 -->|GPIO34| ESP32
	S2 -->|GPIO35| ESP32
	S3 -->|GPIO36| ESP32
	S4 -->|GPIO39| ESP32

	ESP32 -->|GPIO21 SDA| AHT
	ESP32 -->|GPIO22 SCL| AHT

	ESP32 -->|GPIO16| LED
	ESP32 -->|GPIO17| BZ
```

Note: If your relay board is active-low, adjust wiring logic or relay module settings accordingly.

## Software Setup

### Prerequisites

- Arduino IDE 2.x
- ESP32 board package installed in Arduino IDE
- Data upload tool for ESP32 (for data folder upload)

### Required Libraries

| Library                      | Download Link                                                                    | Notes                           |
| ---------------------------- | -------------------------------------------------------------------------------- | ------------------------------- |
| WiFiManager                  | https://github.com/tzapu/WiFiManager                                             | WiFi provisioning portal        |
| ESPAsyncWebServer            | https://github.com/ESP32Async/ESPAsyncWebServer                                  | Async web server for ESP32      |
| AsyncTCP                     | https://github.com/ESP32Async/AsyncTCP                                           | Required by ESPAsyncWebServer   |
| ArduinoJson                  | https://github.com/bblanchon/ArduinoJson                                         | JSON parsing and serialization  |
| Preferences (ESP32 Core)     | https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html | Built into ESP32 Arduino core   |
| Adafruit AHTX0 (AHT10/AHT20) | https://github.com/adafruit/Adafruit_AHTX0                                       | Sensor support for AHT10        |
| Dusk2Dawn                    | https://github.com/dmkishi/Dusk2Dawn                                             | Sunrise and sunset calculations |
| FirebaseClient (Mobizt)      | https://github.com/mobizt/FirebaseClient                                         | Firebase integration            |

### Build and Flash Steps

1. Open SMART_HOME.ino in Arduino IDE.
2. Select board: ESP32 Dev Module (or your exact ESP32 board).
3. Confirm correct COM port.
4. Compile and upload firmware.
5. Upload web assets from data folder (index.html, config.html, icons, svg assets) to SPIFFS/LittleFS.
6. Restart device.
7. Connect to the same network and open the device IP (192.168.4.1) in browser.

## First Login

- Default admin user: esp
- Default admin password: 456456

Change admin credentials immediately from User -> Edit Admin.

## Project Structure

```text
SMART_HOME/
|- SMART_HOME.ino         # Main firmware and API routes
|- data/
|  |- index.html          # Dashboard/login
|  |- config.html         # Full settings UI
|  |- index.svg
|  |- settings.svg
|- screenshots/           # README screenshots
|- README.md
|- LICENSE
```

## YouTube Demo

Add your demo link here:

- https://www.youtube.com/watch?v=YOUR_VIDEO_ID

## Security Notes

- Sensitive actions require admin verification.
- BOOT hold requirement protects critical reset operations.
- Use strong admin credentials in production.
- Review Firebase rules before cloud deployment.

## Troubleshooting

- If the web UI looks outdated, hard refresh browser and re-upload data files.
- If sunrise/sunset does not appear, verify timezone + location and ensure device has valid time.
- If WiFi reconnect is unstable, verify DHCP/static configuration and gateway/DNS values.
- If automations conflict, tune order in Schedule Priority.

## License

This project uses a custom owner license.

See LICENSE for terms.

## Author

- Owner: iotBrainStorm
