# GGS Bridge Firmware v2 — Relay Chain

Three-ESP relay chain for streaming Spider Farmer GGS AC5 telemetry to Cultivar
when the AC5 is too far from WiFi for a single bridge to work.

```
[AC5 sensor in shed] --BLE--> [SHED esp]
                                  |
                            ESP-NOW (~25-30 ft)
                                  v
                            [GARAGE esp]
                                  |
                            ESP-NOW (~25-30 ft)
                                  v
                            [HOUSE esp] --WiFi--> Supabase --> Cultivar
```

## Layout

```
.
├── platformio.ini          # 3 build envs: shed, garage, house
├── src/
│   ├── main.cpp            # all 3 roles, selected by #ifdef
│   ├── role_config.h       # per-role compile-time flags
│   ├── espnow_link.h       # ESP-NOW API
│   └── espnow_link.cpp     # ESP-NOW pairing, channel sync, packet relay
├── .github/workflows/
│   └── build.yml           # builds all 3 firmwares per push
└── netlify/                # NOT part of this GitHub repo — drop in Netlify site
    ├── flash-bridge.html
    └── bridge-manifest-{shed,garage,house}.json
```

## Deployment

1. **Push these files to GitHub.** GitHub Actions auto-builds 3 firmwares.
2. **Download the artifact** from the Actions run. Inside you'll find:
   - `ggs-bridge-bootloader.bin`
   - `ggs-bridge-partitions.bin`
   - `ggs-bridge-boot_app0.bin`
   - `ggs-bridge-shed-firmware.bin`
   - `ggs-bridge-garage-firmware.bin`
   - `ggs-bridge-house-firmware.bin`
3. **Drag all 6 .bin files + the netlify/ contents** into your Netlify site root
   alongside `index.html` (replacing the old single firmware.bin and flash-bridge.html).
4. **Visit your Netlify flash page**, click each role's install button.

## Flash order

1. **HOUSE** first — sits by your router, gets the wifi credentials, anchors the chain
2. **GARAGE** second — auto-pairs to HOUSE
3. **SHED** last — auto-pairs to GARAGE, talks BLE to your AC5

## Configuration

- **HOUSE:** WiFiManager captive portal (wifi creds + Supabase URL + Supabase key + fallback room label)
- **GARAGE:** none — pure plug-and-play
- **SHED:** lightweight captive portal (BLE MAC + room label)

To re-configure, hold the BOOT button while applying power; firmware will detect this
and re-enter config mode. (Or just visit the AP it broadcasts on power-up.)
