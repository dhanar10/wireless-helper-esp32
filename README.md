# ESP32 Wireless Helper

PlatformIO + ESP-IDF firmware for **ESP32-WROOM-32E**. Replaces Wireless Helper’s phone/tablet-hotspot modes: join WiFi, listen for a TCP knock on **5289**, then trigger Android Auto on a paired phone over Bluetooth RFCOMM (no Gearhead intent).

## Flow

1. Classic Bluetooth: discoverable, SSP auto-accept pairing
2. WiFi STA joins the AP from Kconfig (SSID/key)
3. On GOT_IP: listen TCP **5289**; headunit subnet-scans and knocks
4. On knock: record source IP, one-shot HFP/HSP poke to all bonded phones
5. Phone opens AA RFCOMM (SCN 8) → `WifiStartRequest(ip:5288)` → `WifiInfoResponse(SSID/key/BSSID)`
6. Phone joins the AP and connects TCP to the headunit

Knocks are ignored while a wake poke or handshake is in progress. Retries are the headunit’s job (re-knock).

## Build / flash

```bash
pio run
pio run -t upload
pio device monitor
```

Menuconfig (WiFi credentials, BT name, ports):

```bash
pio run -t menuconfig
```

## Configure (Kconfig only)

| Setting | Default |
|---------|---------|
| BT name | `ESP32 Wireless Helper` |
| WiFi SSID / key | `AndroidAuto` / `password123` |
| AA TCP port | `5288` |
| Knock port | `5289` |
| Wake hold | `15000` ms |

BSSID is learned from the associated AP after STA connect (not a Kconfig setting).
Knocks and `WifiInfoResponse` require a BSSID from STA — Gearhead matches SSID+BSSID.

No NVS overlay or UART commands — change via menuconfig / `sdkconfig.defaults`, rebuild, flash. UART is logging only.

## Use

1. Set SSID/key to the AP shared with the headunit; flash
2. Pair the phone once with **ESP32 Wireless Helper**
3. Put ESP and headunit on the same LAN
4. Headunit scans the subnet for open **5289** and knocks
5. Serial log should show knock → poke → `WifiStartRequest` → `WifiInfoResponse` → `WifiConnectStatus`

## Out of scope

- SoftAP / AA projection proxy on the ESP
- mDNS discovery (HU subnet-scans 5289)
- ESP-side wake/handshake retries
- BLE
