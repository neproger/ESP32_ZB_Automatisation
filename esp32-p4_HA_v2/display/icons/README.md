# Иконки (иконочные шрифты)

Иконки — глифы, собранные в LVGL-шрифты. Цвет задаётся стилем (`text_color`), поэтому
иконки следуют теме. Два независимых набора (раздельные шрифты — без конфликта кодпоинтов):

| Набор | Список | Источник | Шрифт | Макросы |
| :-- | :-- | :-- | :-- | :-- |
| UI-хром | `icons.txt` | FontAwesome (free solid) | `ui_icons` | `UI_ICON_*` |
| Погода | `weather_icons.txt` | Weather Icons (erikflowers) | `wicons` | `WEATHER_ICON_*` |

## Как добавить иконку

1. Дописать имя в нужный список (`icons.txt` или `weather_icons.txt`).
2. Сгенерировать шрифты:
   ```cmd
   cd display/icons/tools
   npm install            # один раз
   node gen.mjs
   ```
3. В коде: `UI_ICON_<NAME>` / `WEATHER_ICON_<NAME>` после `#include "ui_icons.h"` / `"wicons.h"`.

`gen.mjs` резолвит юникоды по metadata пакетов (`@fortawesome/fontawesome-free`,
`weather-icons`) — без хардкода, и зовёт `lv_font_conv`. Результат (`ui_icons.c/.h`,
`wicons.c/.h`) коммитится — сборка прошивки от node не зависит.

## Использование

```c
lv_obj_t *icon = lv_label_create(parent);
lv_obj_set_style_text_font(icon, &ui_icons, 0);      /* или &wicons */
lv_obj_set_style_text_color(icon, lv_color_hex(UI_COL_TEXT), 0);
lv_label_set_text(icon, UI_ICON_GEAR);
```

Погода в строке состояния (`ui_status_bar.c`) маппит `ha_weather_condition_t` на глифы
`wicons` (`weather_glyph()`). Размер шрифтов — `SIZE` в `gen.mjs` (сейчас 28).
