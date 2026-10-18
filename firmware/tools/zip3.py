#!/usr/bin/env python3
"""Builds the ZIP-prefix -> latitude/longitude table embedded in www/index.html (for the sunrise/sunset
cards). Averages every US ZIP code's coordinates per 3-digit prefix: good to ~30 km, i.e. a minute or two
of sunrise time.

Usage: tools/zip3.py US.txt   (from https://download.geonames.org/export/zip/US.zip, CC BY 4.0, GeoNames)
Prints one JS line: const ZIP3 = {...};
"""
import sys
from collections import defaultdict

acc = defaultdict(lambda: [0.0, 0.0, 0])
for line in open(sys.argv[1], encoding="utf-8"):
    f = line.rstrip("\n").split("\t")
    if len(f) < 11 or not f[9] or not f[10]:
        continue
    a = acc[f[1][:3]]
    a[0] += float(f[9]); a[1] += float(f[10]); a[2] += 1
table = ",".join(f'"{k}":[{v[0] / v[2]:.2f},{v[1] / v[2]:.2f}]' for k, v in sorted(acc.items()))
print("// ZIP prefix -> [lat, lon], averaged from GeoNames postal codes (CC BY 4.0, geonames.org); tools/zip3.py")
print(f"const ZIP3 = {{{table}}};")
