# Thread Border Router + BLE Peripheral Example

This example demonstrates running a **Thread Border Router** alongside a **BLE Peripheral** on an ESP32-P4, using an external RCP that supports both **OpenThread RCP** and **ESP-Hosted** (WiFi + BLE).

## Overview

The example combines two functionalities:

1. **Thread Border Router**: Provides connectivity between Thread mesh network and IP network (WiFi/Ethernet)
2. **BLE Peripheral**: Advertises as a BLE device with a custom GATT service, allowing connections from BLE centrals

Both functionalities communicate with the RCP:
- **Thread**: Via UART spinel protocol to the ot-rcp firmware
- **BLE**: Via ESP-Hosted VHCI transport to the BLE controller on the RCP

## Hardware Requirements

- **Host**: ESP32-P4 (or other chipset without native 802.15.4 radio)
- **RCP**: ESP32-C6 or ESP32-H2 running custom firmware that includes:
  - OpenThread RCP (ot-rcp)
  - ESP-Hosted coprocessor (for WiFi and/or BLE)

### Wiring

Connect the ESP32-P4 to the RCP via UART:

| ESP32-P4 | RCP (ESP32-C6/H2) |
|----------|-------------------|
| GPIO 4   | TX                |
| GPIO 5   | RX                |

> **Note**: Adjust `CONFIG_PIN_TO_RCP_TX` and `CONFIG_PIN_TO_RCP_RX` in sdkconfig if using different pins.

## RCP Firmware

Your RCP must run firmware that supports both protocols. This typically involves building a combined ot-rcp + esp-hosted coprocessor firmware for your RCP chip.

## Build and Flash

### Configure

```bash
cd examples/thread_border_router_ble

# Set target to ESP32-P4
idf.py set-target esp32p4
```

### Build

```bash
idf.py build
```

### Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## BLE Peripheral Details

The BLE peripheral:
- **Device Name**: `esp-ot-br-ble`
- **Advertises**: Alert Notification Service UUID (0x1811)
- **Custom Service**: UUID `59462f12-9543-9999-12c8-58b459a2712d`
  - **Characteristic**: Read/Write/Notify/Indicate
  - **Descriptor**: Read-only

You can connect to this device using any BLE central app (e.g., nRF Connect, LightBlue).

## OpenThread CLI

The OpenThread CLI is available via the serial console. Example commands:

```
> state
leader

> ipaddr
fd5b:281f:1ef6:923c:0:ff:fe00:fc00
fd5b:281f:1ef6:923c:0:ff:fe00:8000
fd5b:281f:1ef6:923c:8cc:b8cd:7e50:bba0
fe80:0:0:0:acb5:4f57:5f0d:6f6f

> dataset active
Active Timestamp: 1
Channel: 20
Channel Mask: 0x07fff800
...
```

## Configuration Options

### BLE Security (Optional)

Enable in `sdkconfig`:
```
CONFIG_EXAMPLE_BONDING=y      # Enable bonding
CONFIG_EXAMPLE_MITM=y         # Enable MITM protection
CONFIG_EXAMPLE_USE_SC=y       # Enable Secure Connections
```

### RCP UART Pins

Adjust in `sdkconfig.defaults.esp32p4`:
```
CONFIG_PIN_TO_RCP_TX=4
CONFIG_PIN_TO_RCP_RX=5
```

### WiFi Configuration

If using WiFi as the backbone interface, configure:
```
CONFIG_EXAMPLE_CONNECT_WIFI=y
CONFIG_EXAMPLE_WIFI_SSID="your_ssid"
CONFIG_EXAMPLE_WIFI_PASSWORD="your_password"
```

## Troubleshooting

### BLE not advertising
- Check that ESP-Hosted connection to RCP succeeded (look for "ESP-Hosted RCP FW Version" in logs)
- Verify RCP firmware includes BLE support
- Check `CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y` is set

### Thread not starting
- Verify UART pins are correctly connected
- Check RCP firmware includes ot-rcp support
- Look for spinel communication errors in logs

### Both BLE and Thread failing
- The RCP firmware may not support both protocols simultaneously
- Check RCP has enough resources (RAM, flash) for combined firmware

## License

This example is provided under the Apache-2.0 license. See the LICENSE file for details.
