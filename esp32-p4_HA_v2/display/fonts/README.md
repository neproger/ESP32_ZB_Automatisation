# Шрифты UI

`ui_font_20/32/48.c` — сгенерированы из `Montserrat-Regular.ttf` (лежит рядом,
лицензия OFL) под LVGL 9. Встроенные шрифты LVGL кириллицу не содержат, поэтому свои.

Диапазоны: ASCII `32-127`, `°` (0xB0), тире `–—` (0x2013-0x2014), `№` (0x2116),
кириллица U+0400..U+04FF.

## Перегенерация

```cmd
npx lv_font_conv --font Montserrat-Regular.ttf --size 20 --bpp 4 --format lvgl ^
  --range 32-127,0xB0,0x2013-0x2014,0x2116,0x400-0x4FF ^
  --no-compress --lv-include lvgl.h --lv-font-name ui_font_20 -o ui_font_20.c
```

То же для `--size 32` и `--size 48` (и именами `ui_font_32`, `ui_font_48`).

`--lv-include lvgl.h` обязателен: иначе генератор пишет `#include "lvgl/lvgl.h"`,
которого нет в include-путях IDF-сборки (там доступен только `lvgl.h`).
