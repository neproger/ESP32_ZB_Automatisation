# Display preview (LVGL Live Preview)

Host-превью UI без железа. Тот же код `../src/ui_*`, что и в прошивке; вместо Domain
подставлен фейк с мок-данными (`domain_fake.c`). См. `../../docs/clients/DISPLAY.md`.

## Какое расширение

**LVGL Live Preview** от `themastercoder007`
(`themastercoder007.lvgl-live-preview`) — компилирует LVGL C/C++ в WASM (Emscripten)
и показывает в webview с hot-reload.

Это **не** Microsoft **Live Preview** (`ms-vscode.live-server`): то расширение поднимает
веб-сервер для HTML/JS и LVGL C-код не собирает.

## Требования

- Python 3 в `PATH` (проверено: 3.12);
- ~1–2 ГБ на диске и сеть: расширение один раз скачивает Emscripten SDK (и SDL2-порт);
- LVGL 9.x. В списке расширения 9.6 нет — ставить **9.5.0** (прошивка идёт на 9.6.0;
  расхождение закрыто `../src/ui_compat.h`).

## Запуск

1. Открыть панель настроек превью (шестерёнка) и задать панель **480×800**
   (по умолчанию 480×320), LVGL version **9.5.0**.
2. `Ctrl+Shift+L` (LVGL: Start Live Preview) из `preview_entry.c`.
3. Правка `ui/*` → `Ctrl+S` → кадр обновляется.

Точка входа — `lvgl_live_preview_init()` под `#ifdef LVGL_LIVE_PREVIEW` (define даёт
само расширение). Сейчас она вызывает `domain_fake_init()` и `display_start()`.

## Конфигурация превью

`.lvgl-live-preview.json` (рядом с этим файлом) — т.к. авто-обнаружение по `#include`-графу
не находит `domain_fake.c` (у него нет заголовка) и отдельные `.c` шрифтов (у них нет
парных `.h`). Пути — **относительно конфига**:

```text
mainFile       preview_entry.c
includePaths   ../src, ../include, ../../ha_model/include, ../../domain/include, ../../sys/include
dependencies   ../src/display.c, ../src/ui_page.c, ../src/ui_widgets.c,
               ../fonts/ui_font_20.c, ui_font_32.c, ui_font_48.c, domain_fake.c
```

Если расширение ищет конфиг в корне рабочей папки, а не рядом с точкой входа —
переместить файл в корень и поправить пути.

## Мок-данные

`domain_fake.c`: устройства, состояния, группы и элементы. Меняешь фикстуры или экран →
`Ctrl+S` → превью обновляется.

## Что дальше

Порт под плату (`display/port/espidf`: esp_lcd + lvgl_port + GT911) и вызов
`display_start(&s_domain)` из `main/app_main.c` — когда будет железо.
