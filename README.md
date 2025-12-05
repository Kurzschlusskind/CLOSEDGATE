# CLOSEDGATE

ESP32 YubiKey NFC Access Controller - Secure, cryptographically validated access control using YubiKey 5 NFC with server-side OTP verification against the Yubico Cloud API.

## Features

- **YubiKey OTP Validation**: Cryptographically secure one-time passwords
- **NFC Communication**: PN532 NFC reader via I2C for contactless authentication
- **Cloud Verification**: Server-side OTP validation against Yubico Cloud API
- **Automatic Reconnection**: WiFi reconnection with exponential backoff
- **Configurable Hardware**: Flexible GPIO configuration via menuconfig
- **Robust Error Handling**: Comprehensive logging and retry mechanisms

## Hardware Requirements

### Components

| Component | Description | Quantity |
|-----------|-------------|----------|
| ESP32 | Development board (e.g., ESP32-DevKitC) | 1 |
| PN532 | NFC/RFID module with I2C interface | 1 |
| Relay Module | 5V relay module (Active-High) | 1 |
| YubiKey 5 NFC | YubiKey with NFC capability | 1 |
| Power Supply | 5V power supply | 1 |

### Wiring Diagram

```
ESP32                    PN532 NFC Module
┌─────────┐              ┌─────────────┐
│         │              │             │
│   GPIO21├──────────────┤SDA          │
│   GPIO22├──────────────┤SCL          │
│     3.3V├──────────────┤VCC          │
│      GND├──────────────┤GND          │
│         │              │             │
└─────────┘              └─────────────┘

ESP32                    Relay Module
┌─────────┐              ┌─────────────┐
│         │              │             │
│   GPIO26├──────────────┤IN           │
│      5V ├──────────────┤VCC          │
│      GND├──────────────┤GND          │
│         │              │             │
└─────────┘              └─────────────┘
```

### Default GPIO Configuration

| Function | GPIO | Description |
|----------|------|-------------|
| I2C SDA | GPIO21 | NFC module data line |
| I2C SCL | GPIO22 | NFC module clock line |
| Relay | GPIO26 | Relay control (Active-High) |

## Software Requirements

- **ESP-IDF v5.x** (tested with v5.1+)
- **Python 3.8+** (for ESP-IDF tools)
- **Git** (for cloning repository)

## Installation

### 1. Install ESP-IDF

Follow the official ESP-IDF installation guide:
- [ESP-IDF Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32/get-started/)

Quick setup on Linux/macOS:
```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32
source export.sh
```

### 2. Clone CLOSEDGATE Repository

```bash
git clone https://github.com/yourusername/CLOSEDGATE.git
cd CLOSEDGATE
```

### 3. Configure the Project

```bash
idf.py menuconfig
```

Navigate to **CLOSEDGATE Configuration** and set:

- **WiFi SSID**: Your WiFi network name
- **WiFi Password**: Your WiFi password
- **Yubico Client ID**: Your Yubico API client ID (see below)
- **Relay GPIO**: GPIO pin for relay (default: 26)
- **Relay Pulse Duration**: Door unlock duration in ms (default: 3000)
- **NFC I2C SDA GPIO**: I2C data pin (default: 21)
- **NFC I2C SCL GPIO**: I2C clock pin (default: 22)

### 4. Build the Project

```bash
idf.py build
```

### 5. Flash to ESP32

```bash
idf.py -p /dev/ttyUSB0 flash
```

Replace `/dev/ttyUSB0` with your ESP32's serial port.

### 6. Monitor Output

```bash
idf.py -p /dev/ttyUSB0 monitor
```

Press `Ctrl+]` to exit monitor.

## Yubico API Key Registration

To use the Yubico Cloud API for OTP verification, you need to register for a free API key:

1. Visit [Yubico API Key Registration](https://upgrade.yubico.com/getapikey/)
2. Enter your email address and a YubiKey OTP (touch your YubiKey while focused on the OTP field)
3. Complete the CAPTCHA and submit
4. You will receive:
   - **Client ID**: A numeric identifier (use this in menuconfig)
   - **Secret Key**: For HMAC signature verification (optional, not used in basic mode)

5. Configure the Client ID in menuconfig:
   ```
   idf.py menuconfig
   ```
   Navigate to: CLOSEDGATE Configuration → Yubico Client ID

## Usage

1. Power on the ESP32
2. Wait for WiFi connection (check serial monitor)
3. Tap your YubiKey 5 NFC on the PN532 reader
4. On successful OTP verification, the relay activates for 3 seconds
5. Use the relay to control your door lock, gate, or other access mechanism

### LED Indicators (if available on your ESP32 board)

| State | Description |
|-------|-------------|
| Booting | System initialization |
| WiFi Connecting | Attempting WiFi connection |
| Ready | Waiting for YubiKey tap |
| Verifying | OTP sent to Yubico API |
| Access Granted | Relay activated |
| Access Denied | Invalid or replayed OTP |

## Configuration Options

### Kconfig Options

| Option | Default | Description |
|--------|---------|-------------|
| `CLOSEDGATE_WIFI_SSID` | YourWiFiSSID | WiFi network name |
| `CLOSEDGATE_WIFI_PASSWORD` | YourWiFiPassword | WiFi password |
| `CLOSEDGATE_YUBICO_CLIENT_ID` | (empty) | Yubico API client ID |
| `CLOSEDGATE_RELAY_GPIO` | 26 | Relay GPIO pin |
| `CLOSEDGATE_RELAY_PULSE_MS` | 3000 | Relay pulse duration (ms) |
| `CLOSEDGATE_NFC_SDA_GPIO` | 21 | I2C SDA pin |
| `CLOSEDGATE_NFC_SCL_GPIO` | 22 | I2C SCL pin |
| `CLOSEDGATE_NFC_POLL_INTERVAL_MS` | 500 | NFC polling interval |
| `CLOSEDGATE_HTTP_RETRY_COUNT` | 3 | HTTP request retries |
| `CLOSEDGATE_WIFI_RECONNECT_MAX_RETRY` | 10 | Max WiFi reconnect attempts |

## Troubleshooting

### Common Issues

#### NFC Module Not Detected

```
E (1234) CLOSEDGATE: Failed to get PN532 firmware version
```

**Solutions:**
1. Check I2C wiring (SDA to GPIO21, SCL to GPIO22)
2. Verify PN532 is set to I2C mode (check DIP switches)
3. Ensure proper power supply to PN532 (3.3V)
4. Try reducing I2C speed in code (100kHz default)

#### WiFi Connection Failed

```
E (5678) CLOSEDGATE: Max reconnection attempts reached
```

**Solutions:**
1. Verify WiFi SSID and password in menuconfig
2. Check WiFi signal strength
3. Ensure router allows new device connections
4. Try moving ESP32 closer to router

#### OTP Verification Failed

```
W (9012) CLOSEDGATE: === ACCESS DENIED: NO_SUCH_CLIENT ===
```

**Solutions:**
1. Verify Yubico Client ID in menuconfig
2. Ensure you registered for a Yubico API key
3. Check internet connectivity

#### Replayed OTP

```
W (3456) CLOSEDGATE: === ACCESS DENIED: Replayed OTP ===
```

**Explanation:** Each YubiKey OTP can only be used once. This error occurs when:
1. The same OTP is presented twice
2. The YubiKey's internal counter has reset
3. Network issues caused a duplicate request

### Debug Logging

Enable verbose logging by changing log level in `sdkconfig`:

```
CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
CONFIG_LOG_DEFAULT_LEVEL=4
```

Or via menuconfig:
```
idf.py menuconfig
```
Navigate to: Component config → Log output → Default log verbosity → Debug

## Security Considerations

1. **Physical Security**: Secure the ESP32 and relay module from tampering
2. **WiFi Security**: Use WPA2/WPA3 encryption
3. **API Keys**: Keep your Yubico API secret key confidential
4. **Firmware Updates**: Regularly update ESP-IDF for security patches
5. **Certificate Validation**: The firmware uses ESP-IDF's certificate bundle for TLS verification

## Project Structure

```
CLOSEDGATE/
├── CMakeLists.txt              # Main CMake configuration
├── Kconfig.projbuild           # menuconfig options
├── sdkconfig.defaults          # Default configuration
├── main/
│   ├── CMakeLists.txt          # Component CMake
│   ├── Kconfig.projbuild       # Component-specific config
│   ├── closedgate_main.c       # Main application
│   ├── nfc_handler.c           # PN532 NFC driver
│   ├── nfc_handler.h
│   ├── yubikey_verify.c        # Yubico API client
│   ├── yubikey_verify.h
│   ├── wifi_manager.c          # WiFi with reconnect
│   ├── wifi_manager.h
│   ├── relay_control.c         # Relay control
│   └── relay_control.h
└── README.md                   # This file
```

## License

This project is provided as-is for educational and personal use.

## Contributing

Contributions are welcome! Please submit issues and pull requests on GitHub.

## Acknowledgments

- [Espressif Systems](https://www.espressif.com/) for ESP-IDF
- [Yubico](https://www.yubico.com/) for YubiKey and OTP validation API
- [Adafruit](https://www.adafruit.com/) for PN532 documentation