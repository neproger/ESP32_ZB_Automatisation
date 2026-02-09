# Smart Knob Display (1.5″, LVGL + ESP-IDF)

Прошивка для компактного настенного/настольного устройства на базе ESP32 и LVGL.

**Железо:**

- 1,5‑дюймовый дисплей Smart Knob с сенсорным управлением
- Модель: UEDX46460015-WB-A
- Версия: V1.1
- Дата: 16.10.2024

## Для чего этот проект

Устройство задумывается как “умный энкодер с экраном” — компактный контроллер для управления умным домом и отображения текущего состояния.

Ключевая фишка: UI собирается автоматически из данных Home Assistant. Как только вы добавляете новое устройство в комнату (area) в HA, оно появляется в интерфейсе этого дисплея без перепрошивки — прошивка подхватывает конфигурацию при bootstrap по HTTP.

Основные сценарии:

- просмотр комнат и устройств (свет, розетки и т.п.) и их переключение;
- отображение часов и погоды в режиме заставки;
- показ прогресса загрузки/инициализации (boot splash);
- интеграция с Home Assistant через HTTP/MQTT (через отдельный router/http слой).

## Структура проекта (по модулям)

### Основной вход

- `main/main.cpp`  
  Инициализация Wi‑Fi, дисплея, LVGL и устройств, старт bootstrap по HTTP, запуск роутера/MQTT и создание UI:
  - показывает boot splash и обновляет прогресс инициализации;
  - вызывает `ui_app_init()` для построения экранов комнат;
  - включает поддержку скринсейвера (`ui::screensaver::init_support()`);
  - запускает периодические опросы погоды (`ui::screensaver::start_weather_polling()`).

### Состояние и сетевое окружение

- `main/state_manager.cpp`  
  Хранит:
  - список areas (комнат);
  - список entities (устройства/сущности HA);
  - погоду (`WeatherState`) и время (`ClockState`);
  - подписки UI на изменение состояния (`subscribe_entity`).

- `main/http_manager.cpp`  
  - bootstrap состояния из Home Assistant по HTTP (CSV);
  - запуск фонового опроса погоды, вызов callback после `set_weather()`.

- `main/app/router.cpp`  
  - инкапсулирует транспорт (обычно MQTT);
  - предоставляет `router::toggle(entity_id)` и `router::is_connected()`.

### UI: комнаты

- `main/ui/rooms.hpp`, `main/ui/rooms.cpp`  
  Отвечает за “страницы комнат”:
  - описывает структуры:
    - `RoomPage` (корневой объект LVGL, заголовок, список виджетов устройств);
    - `DeviceWidget` (контейнер + подпись + контрол).
  - строит все страницы на основе `state::areas()` и `state::entities()` (`ui_build_room_pages()`);
  - хранит вектора `s_room_pages`, `s_current_room_index`, `s_current_device_index`;
  - умеет:
    - показать первую комнату (`show_initial_room()`);
    - листать комнаты с анимацией (`show_room_relative()`);
    - вернуть `entity_id` текущего выбранного девайса (`get_current_entity_id()`);
    - найти `entity_id` по LVGL‑контролу (`find_entity_for_control()`);
    - обновлять виджеты при изменении состояния сущности (`on_entity_state_changed()`);
    - обрабатывать жесты на корневом объекте комнаты (`root_gesture_cb()`).

### UI: скринсейвер (часы + погода)

- `main/ui/screensaver.hpp`, `main/ui/screensaver.cpp`  
  Экран заставки с погодой и часами:
  - строит отдельный корневой экран LVGL для скринсейвера (`ui_build_screensaver()`);
  - поддерживает:
    - `init_support()` — создание таймеров idle/clock, навешивание input‑callback;
    - `ui_update_weather_and_clock()` — читает `state::weather()` и `state::clock()`, обновляет лейблы и иконку;
    - `start_weather_polling()` — подключается к `http_manager` и обновляет скринсейвер при новых данных;
    - `show()` / `hide_to_room()` — переходы между скринсейвером и текущей комнатой;
    - `is_active()` — флаг, активен ли сейчас скринсейвер.

Логика таймаута (idle‑timer) и возврата по тапу полностью живёт внутри этого модуля.

### UI: splash (экран загрузки)

- `main/ui/splash.hpp`, `main/ui/splash.cpp`  
  Простой стартовый экран:
  - `show()` — создаёт корневой экран с логотипом и прогресс‑баром;
  - `update_progress(int percent)` — обновляет прогресс;
  - `destroy()` — убирает splash, когда UI полностью готов.

Используется только из `main.cpp`, без промежуточных врапперов в `ui_app`.

### UI: switch + toggle логика

- `main/ui/switch.hpp`, `main/ui/switch.cpp`  
  Инкапсулирует поведение свитчей и логику “переключить сущность в HA”:
  - `namespace ui::controls`:
    - `set_switch_state(lv_obj_t *control, bool is_on)` — проставляет `LV_STATE_CHECKED`;
    - `set_switch_enabled(lv_obj_t *control, bool enabled)` — включает/выключает `LV_STATE_DISABLED`.
  - `namespace ui::toggle`:
    - `switch_event_cb(lv_event_t *e)` — общий обработчик для всех switch‑контролов;
    - `trigger_toggle_for_entity(const std::string &entity_id)` — дебаунс, проверка Wi‑Fi/MQTT, запуск `ha_toggle_task`;
    - внутри: спиннер на экране, блокировка/разблокировка всех свитчей, запуск отдельной FreeRTOS‑таски.

Благодаря этому модулю, логика переключения больше не размазана по `ui_app` и может использоваться в других экранах.

### UI: приложение

- `main/ui/ui_app.cpp`  
  Сейчас это тонкий “оркестр” вокруг модулей:
  - строит страницы комнат (`ui_build_room_pages()`);
  - навешивает обработчики:
    - жесты комнат (`ui::rooms::root_gesture_cb`);
    - свитчи (`ui::toggle::switch_event_cb`);
    - knob / button события (`LVGL_knob_event`, `LVGL_button_event` — C API для `devices_init.c`);
  - подписывает UI на обновление сущностей через `state_manager` и делегирует изменения в `ui::rooms::on_entity_state_changed`;
  - в `handle_single_click()` по текущему выделенному виджету дергает `ui::toggle::trigger_toggle_for_entity()`.

В файле больше нет работы напрямую с “сырыми” LVGL‑объектами скринсейвера или разрозненных глобалов — всё вынесено в отдельные модули.

## Сборка и прошивка

Проект использует ESP‑IDF и стандартный `idf.py`‑workflow.

Минимальный цикл разработки:

```bash
idf.py set-target esp32s3   # или другой нужный чип
idf.py menuconfig           # настроить Wi‑Fi, пины дисплея и т.д.
idf.py -p COMx build flash monitor
```

LVGL уже интегрирован через `esp_lvgl_port`. Все обращения к LVGL внутри проекта выполняются под `lvgl_port_lock(...)` там, где это необходимо.
