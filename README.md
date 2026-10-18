<p align="center"><img src="docs/mark.svg" width="88" alt=""></p>

<h1 align="center">Halcyon Frame</h1>

<p align="center"><em>A calm, local-first e-paper photo frame. Your photos stay home.</em></p>

Halcyon Frame is open firmware for the **Waveshare ESP32-S3-PhotoPainter**, a 7.3" six-colour e-paper
frame. It replaces the vendor's cloud firmware with one that runs entirely on the frame:

- **A web page served by the frame itself.** Add photos from your phone or computer and the browser crops,
  colour-maps and dithers them for the panel. Batch-upload hundreds of photos at once.
- **Photos and settings on the TF card.** No account, no app, no subscription, no cloud storage. Optional
  automatic setup asks `pool.ntp.org` for the time and `ip-api.com` for your time zone; one switch turns it off.
- **A status screen** in seven typefaces: clock, indoor temperature and humidity (with a mold-risk flag),
  battery, storage, sunrise/sunset, moon, daylight. Four configurable cards.
- **Calm by design.** E-paper keeps its picture with no power, so the frame sleeps between changes and
  saves its battery. Quiet hours, set change times, and no glow at night.
- **Updates over Wi-Fi**, with automatic rollback if a new version doesn't start.

## Install

The easiest way is the **[browser installer at halcyonframe.com](https://halcyonframe.com)**. Plug the frame
into a computer with USB-C, open the page in Chrome or Edge, and click Install. It's meant for a frame fresh
out of the box (still on Waveshare's firmware) and for one that needs rescuing. This is a first release, so
please [report](https://github.com/halcyonframe/halcyonframe/issues) anything that doesn't go smoothly.

Or flash a [release](https://github.com/halcyonframe/halcyonframe/releases) with esptool:

    esptool.py --chip esp32s3 write_flash 0x0 halcyonframe-full.bin

Then join the frame's Wi-Fi (it shows the name, password and QR codes on its screen) and open the page.

## Documentation

- [Firmware guide](firmware/README.md): how it behaves, buttons, settings, `config.txt`, HTTP API, building
- [Releases](https://github.com/halcyonframe/halcyonframe/releases): `halcyonframe.bin` for updates from the
  frame's Settings, and `halcyonframe-full.bin` for USB installs

## Building

ESP-IDF 5.5:

    cd firmware && idf.py set-target esp32s3 && idf.py build

Every push is built by GitHub Actions, and tags publish releases.

## License

MIT, see [LICENSE](LICENSE). Third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

<sub>Not affiliated with Waveshare. "PhotoPainter" is Waveshare's product name.</sub>
