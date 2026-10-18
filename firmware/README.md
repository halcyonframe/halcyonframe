# PhotoFrame firmware for the Waveshare ESP32-S3-PhotoPainter

A fully local replacement for the vendor firmware. The frame runs its own web UI, keeps settings and photos on the TF card, and makes no cloud calls. There is no AI image generation, voice, or weather.

## How it works

| Event | What happens |
|---|---|
| Power on, or **KEY** pressed while asleep | Joins your Wi-Fi (or opens its own access point), serves the web UI, and sleeps after `awake_min` minutes without UI activity. Red LED = awake. |
| Slideshow timer (`interval_min`) or **BOOT** pressed while asleep | Shows the next photo and goes straight back to deep sleep. Wi-Fi never starts, which saves battery. |
| **BOOT** while awake | Next photo |
| **KEY** while awake | Shows "How to reach me" on the panel: address, QR code, battery, photo count |
| **PWR** short press while awake | Sleep now |
| **PWR** held 4 s | Power off (handled by the AXP2101) |

Green LED = the panel is refreshing (about 20 s). Two quick red blinks after pressing BOOT = nothing to show.

### What the screen shows when there's no photo

The frame never sits blank or stuck on an old image. When something needs a person, it draws a status screen (`main/screens.c`) in large type with QR codes. A phone can join the hotspot and open the page without typing anything.

| Situation | Screen |
|---|---|
| No photos yet, on its own hotspot | "Hi! Let's add photos": join *PhotoFrame* (QR), then open 192.168.4.1 (QR) |
| Home Wi-Fi set but joining failed | "Wi-Fi problem": the reason (wrong password / network not found), plus how to reach the hotspot to fix it |
| No photos yet, on home Wi-Fi | "Ready for photos!": `photoframe.local`, the IP address and a QR code |
| After any reset (power-on, restart, crash) | For the first minute, "How to reach me": network, `http://` address with QR code, battery, photos on the card, free space. Then the photo returns. |
| TF card missing or unreadable | What to do about it |
| Battery at 2% or less | "Please charge me". It's drawn while there's still power, so it stays up after the battery dies. At 15% or less, photos get a small "Charge me" corner badge instead. |

Screens that only matter while you're interacting ("Wi-Fi problem", "How to reach me") are replaced by the photo when the frame goes to sleep. Quiet wakes (timer, BOOT) never show a problem screen over your photos. The panel is only refreshed when its content would actually change.

To preview the layouts on a computer, run `tools/render_screens.sh <dir>`. It writes PNGs of every screen in every orientation, and checks that long network names still fit.

### Image processing (in the browser)

Uploading a photo opens an editor in the browser. All processing happens there, so the ESP32 only stores and streams finished frames:

1. Shape it to the frame. The frame's orientation is a setting (landscape, or portrait turned left or right), so the editor knows the target: 800×480 or 480×800. When the photo's shape doesn't match, it says how much a crop would remove and offers three choices: **Fill the frame** (crop; drag to choose what stays), **Whole photo, blurred edges**, or **Whole photo, white edges**. It defaults to filling, unless that would throw away more than 30% of the photo. Zoom, brightness, contrast and saturation are also adjustable.
2. **Color remapping**: each pixel is matched to the closest of the panel's six colors in OKLab (perceptual) space. Matching uses the colors the panel *actually* shows (`PALETTE` in `main/www/index.html`, approximate measurements) rather than pure RGB.
3. **Floyd–Steinberg dithering** (serpentine), with error diffused in linear light so the average of the dots matches the original brightness.
4. **Compression**: indices are packed to the panel's native 4 bits per pixel, so a photo takes 192,000 bytes versus 1.15 MB for the vendor's 24-bit BMPs. The device can stream it to the panel without decoding.

The preview shows the dithered result in panel colors, and the gallery thumbnail is a downscaled copy of that preview.

**Batch upload.** When you pick two or more photos, you can choose **Upload all automatically**. Each photo gets the default shape for the frame and the current color settings, and goes straight to the card with `show=0`, so the panel doesn't change. While it runs:
- It shows progress and the time left.
- It keeps the phone screen awake (Wake Lock API), if the browser supports it.
- It retries failed uploads for about 30 s, long enough to ride out a frame restart.
- At the end it lists anything that couldn't be read or uploaded.

Each decoded photo is released as soon as it's packed, so memory use stays flat for hundreds of photos. Expect 3 to 4 s per photo from a phone.

## TF card layout

```
/config.txt          settings (created with defaults on first boot; editable by hand)
/photos/<id>.epd     panel frame, 800x480 @ 4bpp, left pixel in high nibble (portrait photos pre-rotated)
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
| `orientation` | `landscape` | How the frame hangs: `landscape`, `cw` (turned right) or `ccw` (turned left). The slideshow only shows photos cropped for the current orientation; the UI marks the others. |

Settings lists nearby networks, so you can pick one instead of typing it. It scans when the hotspot starts; **Rescan** briefly pauses hotspot clients. Changing Wi-Fi turns the Save button into **Save & connect**, which restarts the frame onto the new network and shows its new address on the panel. If joining fails, the frame comes back on its hotspot and the panel says why.

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

## Firmware updates

After the first USB flash, there's no need for a cable:

- **Web:** Settings → Firmware → choose `build/photoframe.bin` → Install. It takes about 30 s, then the frame restarts.
- **TF card:** copy `photoframe.bin` to the card root as `firmware.bin` and restart the frame (or power it on). It's renamed to `firmware.done`, or `firmware.failed` if it wasn't valid.

The image goes into the inactive app slot (two 6 MB slots; `partitions.csv`) and is checked before the frame switches to it. A new firmware has to reach a working web UI (or its first deep sleep) to confirm itself. If it resets before that, the bootloader goes back to the previous firmware. A new firmware that *hangs* without resetting needs a power cycle (hold PWR 4 s, then press it) to trigger the rollback.

Installs aren't refused on low battery, only when it's critical. Like the rest of the API, there's no authentication beyond `X-PF`: anyone on your network can install firmware.

The partition table and bootloader settings can only be changed over USB. Moving from an older build without OTA slots needs one USB flash.

## HTTP API

| Method | Path | Notes |
|---|---|---|
| GET | `/` | Web UI (embedded in the firmware) |
| GET | `/api/status` | Battery (and `battery_state` ok/low/critical), TF space, photo count, `bad_photos`, current photo, busy flag, IP, `wifi_ssid`, `wifi_error` (auth/not_found/other), `orientation`, `version`, `reset_reason` (`esp_reset_reason_t`: 1 power-on, 3 restart, 4 crash, 6 watchdog, 9 brownout, 11 USB), `uptime_s`. Polling it does not keep the frame awake. |
| GET | `/api/photos` | `{"photos":[ids…]}` sorted oldest first |
| GET | `/photos/<id>.jpg` / `.epd` | Stored files |
| POST | `/api/upload?id=<id>&type=jpg\|epd[&show=1\|0]` | Raw body. `epd` must be exactly 192000 bytes. Upload `jpg` first, since a photo only appears once its `.epd` exists. `show=1`: show now. No `show`: shown only if a setup or status screen is up. `show=0`: never touches the panel. |
| GET | `/api/wifi` | Nearby networks `{"networks":[{ssid,rssi,secure}]}`, strongest first |
| POST | `/api/wifi/scan` | Rescan, then the same response |
| POST | `/api/show?id=<id>`, `/api/next` | Queue a refresh |
| POST | `/api/delete?id=<id>` | |
| GET/POST | `/api/settings` | POST is form-urlencoded; only fields present are changed; passwords are write-only |
| POST | `/api/ota` | Raw body: the app image. Installs it, then restarts. 400 if it isn't a valid image for this chip. |
| POST | `/api/sleep`, `/api/reboot` | |

Ids are `[A-Za-z0-9_-]{1,32}`. The UI uses base-36 timestamps, so name order is upload order, and appends `_cw` or `_ccw` to portrait photos. That suffix is how the frame knows which orientation a photo was cropped for.

Every POST must include an `X-PF: 1` header. This forces a CORS preflight, which the server never answers, so other web pages can't drive the API. There's no authentication beyond that. Anyone on the same network (or on the access point) can manage photos, so use an access point password and a trusted LAN.

## Code map

| File | Purpose |
|---|---|
| `main/main.c` | Boot/wake flow, buttons, deep sleep |
| `main/epd.c` | Spectra 6 panel driver (init sequence from the Waveshare reference) |
| `main/display.c` | Slideshow selection, refresh task/queue, skip-if-unchanged |
| `main/screens.c` | Status/setup screens and their layout (`gfx.c` draws, `font.c` is generated from [Spleen](https://github.com/fcambus/spleen), BSD-2) |
| `main/storage.c` | TF card mount, `config.txt`, photo files |
| `main/wifi.c` | STA with SoftAP fallback, mDNS |
| `main/web.c` | HTTP server and API |
| `main/ota.c` | Firmware updates (web upload, `firmware.bin` on the card, rollback confirmation) |
| `main/pmic.cpp` | AXP2101 rails, battery status (via vendored `components/XPowersLib`, MIT; patched to use a 1 s I2C timeout instead of waiting forever) |
| `main/www/index.html` | UI and the image pipeline (the `<script id="core">` block is DOM-free and unit-testable) |
