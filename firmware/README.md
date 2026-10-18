# PhotoFrame firmware for the Waveshare ESP32-S3-PhotoPainter

A fully local replacement for the vendor firmware. The frame runs its own web UI, keeps settings and photos on the TF card, and makes no cloud calls. There is no AI image generation, voice, or weather.

## How it works

| Event | What happens |
|---|---|
| Power on, or **KEY** pressed while asleep | Joins your Wi-Fi (or opens its own access point), serves the web UI, and sleeps after `awake_min` minutes without UI activity. Red LED = awake. |
| Slideshow timer (`interval_min`) or **BOOT** pressed while asleep | Shows the next photo and goes straight back to deep sleep. Wi-Fi never starts, which saves battery. |
| **BOOT** while awake | Next photo |
| **PWR** short press while awake | Sleep now |
| **PWR** held 4 s | Power off (handled by the AXP2101) |

Green LED = the panel is refreshing (about 20 s).

### Image processing (in the browser)

Uploading a photo opens an editor in the browser. All processing happens there, so the ESP32 only stores and streams finished frames:

1. Crop/fit to 800×480 (or 480×800 portrait), with zoom, drag-to-pan, brightness, contrast, and saturation.
2. **Color remapping**: each pixel is matched to the closest of the panel's six colors in OKLab (perceptual) space. Matching uses the colors the panel *actually* shows (`PALETTE` in `main/www/index.html`, approximate measurements) rather than pure RGB.
3. **Floyd–Steinberg dithering** (serpentine), with error diffused in linear light so the average of the dots matches the original brightness.
4. **Compression**: indices are packed to the panel's native 4 bits per pixel, so a photo takes 192,000 bytes versus 1.15 MB for the vendor's 24-bit BMPs. The device can stream it to the panel without decoding.

The preview shows the dithered result in panel colors, and the gallery thumbnail is a downscaled copy of that preview.

## TF card layout

```
/config.txt          settings (created with defaults on first boot; editable by hand)
/photos/<id>.epd     panel frame, 800x480 @ 4bpp, viewer orientation, left pixel in high nibble
/photos/<id>.jpg     thumbnail for the web gallery
```

Use a FAT32 card. The firmware never formats the card, and it ignores the vendor's `01_sys_init_img` … `06_user_Foundation_img` folders, which you can delete.

`config.txt` keys:

| key | default | meaning |
|---|---|---|
| `wifi_ssid` / `wifi_pass` | empty | Network to join. Empty, or failing to connect within 20 s, falls back to the access point. |
| `ap_ssid` / `ap_pass` | `PhotoFrame` / `photoframe` | Fallback access point (8-63 char password, or empty for an open network). UI at http://192.168.4.1. An invalid password in a hand-edited file starts an open AP so you can fix it. |
| `hostname` | `photoframe` | DHCP and mDNS name: http://photoframe.local |
| `interval_min` | `60` | Slideshow interval; `0` = only change on request |
| `awake_min` | `10` | Idle minutes before deep sleep when the UI is up; `0` = never sleep |
| `shuffle` | `0` | Random order instead of upload order |
| `rotate180` | `1` | The stock frame mounts the panel upside down |

Wi-Fi changes take effect after a reboot (there's a button for it in the UI).

## Build and flash

Requires ESP-IDF 5.5 (built and tested with v5.5.1).

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

`espressif/mdns` is fetched by the component manager at build time. At runtime there are no external dependencies.

To get back to the vendor firmware, flash `03 Firmware/ESP32-S3-PhotoPainter-factory.bin` at offset 0x0.

## HTTP API

| Method | Path | Notes |
|---|---|---|
| GET | `/` | Web UI (embedded in the firmware) |
| GET | `/api/status` | Battery, TF space, photo count, current photo, busy flag, IP. Polling it does not keep the frame awake. |
| GET | `/api/photos` | `{"photos":[ids…]}` sorted oldest first |
| GET | `/photos/<id>.jpg` / `.epd` | Stored files |
| POST | `/api/upload?id=<id>&type=jpg\|epd[&show=1]` | Raw body. `epd` must be exactly 192000 bytes. Upload `jpg` first, since a photo only appears once its `.epd` exists. |
| POST | `/api/show?id=<id>`, `/api/next` | Queue a refresh |
| POST | `/api/delete?id=<id>` | |
| GET/POST | `/api/settings` | POST is form-urlencoded; only fields present are changed; passwords are write-only |
| POST | `/api/sleep`, `/api/reboot` | |

Ids are `[A-Za-z0-9_-]{1,32}`; the UI uses base-36 timestamps so that name order is upload order.

Every POST must include an `X-PF: 1` header. This forces a CORS preflight, which the server never answers, so other web pages can't drive the API. There's no authentication beyond that. Anyone on the same network (or on the access point) can manage photos, so use an access point password and a trusted LAN.

## Code map

| File | Purpose |
|---|---|
| `main/main.c` | Boot/wake flow, buttons, deep sleep |
| `main/epd.c` | Spectra 6 panel driver (init sequence from the Waveshare reference) |
| `main/display.c` | Slideshow selection, refresh task/queue |
| `main/storage.c` | TF card mount, `config.txt`, photo files |
| `main/wifi.c` | STA with SoftAP fallback, mDNS |
| `main/web.c` | HTTP server and API |
| `main/pmic.cpp` | AXP2101 rails, battery status (via vendored `components/XPowersLib`, MIT) |
| `main/www/index.html` | UI and the image pipeline (the `<script id="core">` block is DOM-free and unit-testable) |
