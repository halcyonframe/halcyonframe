# Third-party notices

Halcyon Frame's firmware binaries include, or are built from, the following third-party components.

## ESP-IDF and Espressif components (Apache-2.0)

- [ESP-IDF](https://github.com/espressif/esp-idf) v5.5.1, Copyright Espressif Systems, Apache License 2.0
- [espressif/mdns](https://components.espressif.com/components/espressif/mdns), Apache License 2.0
- [espressif/qrcode](https://components.espressif.com/components/espressif/qrcode), Apache License 2.0

The full Apache 2.0 text: https://www.apache.org/licenses/LICENSE-2.0

## QR Code generator library (MIT), bundled in espressif/qrcode

```
QR Code generator library (C)

Copyright (c) Project Nayuki. (MIT License)
https://www.nayuki.io/page/qr-code-generator-library

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:
- The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
- The Software is provided "as is", without warranty of any kind, express or
  implied, including but not limited to the warranties of merchantability,
  fitness for a particular purpose and noninfringement. In no event shall the
  authors or copyright holders be liable for any claim, damages or other
  liability, whether in an action of contract, tort or otherwise, arising from,
  out of or in connection with the Software or the use or other dealings in the
  Software.
/
---- Forward declarations for private functions ----*/

// Regarding all public and private functions defined in this source file:
```

## XPowersLib (MIT)

`firmware/components/XPowersLib`, Copyright (c) 2022 lewis he. It is modified: the changes are marked
"Halcyon Frame patch". The license is in `firmware/components/XPowersLib/LICENSE`.

## Spleen (BSD-2-Clause), used for the setup screens (`firmware/main/font.c`)

```
Copyright (c) 2018-2024, Frederic Cambus
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## Typefaces (SIL Open Font License 1.1), used for the status screen (`firmware/main/typefaces.c`)

The status screen uses Inter, Fraunces, DM Serif Display, EB Garamond, Outfit, Figtree and JetBrains Mono,
rasterized to bitmaps. The license texts are in `firmware/tools/fonts/`. The licenses are available with
FAQ at https://openfontlicense.org. Reserved font names are not used.

## posix_tz_db (MIT)

`firmware/main/tzdb.c` is generated from https://github.com/nayarsystems/posix_tz_db (MIT; the upstream
LICENSE file is the MIT template without a filled-in copyright line. Copy: `firmware/tools/LICENSE-posix_tz_db.txt`).

## GeoNames postal codes (CC BY 4.0)

The ZIP-code lookup in the web UI uses coordinates averaged from GeoNames (https://www.geonames.org/),
licensed under Creative Commons Attribution 4.0.

## Online services (not bundled)

With automatic setup on (the default), the frame contacts `pool.ntp.org` (time) and `ip-api.com` (time zone
and city-level location; free tier, non-commercial use). Turn it off in Settings for fully local operation.
