# Tools

Generators for the checked-in data files, previews and host tests. Generated files say which tool made them.

| Tool | Makes | Source data |
|---|---|---|
| `ttf2c.py` | `main/typefaces.c` (status-screen glyphs) | OFL fonts, see `fonts/README.md` |
| `bdf2c.py` | `main/font.c` (setup-screen font) | Spleen 2.1.0 (BSD-2) |
| `tzdb2c.py` | `main/tzdb.c` (IANA to POSIX time zones) | posix_tz_db `zones.json`, commit `93447c0ddac3` (2025-09-29) |
| `zip3.py` | the `ZIP3` table in `main/www/index.html` | GeoNames `US.zip`, downloaded 2026-10-18 (CC BY 4.0) |
| `status_preview.py` | status-screen design previews (PNG) | the same fonts |
| `render_screens.sh` | every on-panel screen as PNG, via the real C code | none |
| `test_schedule.c`, `test_astro.c` | host tests: `cc -I ../main test_schedule.c ../main/schedule.c && ./a.out` (add `-lm` and `astro.c` for astro) | none |
