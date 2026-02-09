# AppState FSM: что уже сделано и что дальше

Этот файл фиксирует практический прогресс по внедрению `AppState`‑архитектуры и список следующих шагов.

---

## 1. Уже сделано

- Введён `AppState` и глобальная переменная:
  - `enum class AppState { BootDevices, BootWifi, BootBootstrap, NormalAwake, NormalScreensaver, NormalSleep, ConfigMode };`
  - `static AppState g_app_state = AppState::BootDevices;` в `main/main.cpp`.
- Добавлен helper `set_app_state(AppState)`:
  - обновляет `g_app_state` только в одном месте;
  - публикует событие `APP_STATE_CHANGED` с `old_state` / `new_state` и `timestamp_us`.
- Расширен `app_events`:
  - `APP_STATE_CHANGED` + `AppStateChangedPayload`;
  - заготовки `REQUEST_CONFIG_MODE`, `REQUEST_SLEEP`, `REQUEST_WAKE` (пока почти не используются).
- Обновлён `event_logger`:
  - понимает и красиво логирует `APP_STATE_CHANGED` и `REQUEST_*`;
  - лог больше не показывает `id=100 (APP_EVENTS unknown)`.
- Интеграция FSM в `main.cpp`:
  - `set_app_state(...)` вызывается по основным этапам бутстрапа:
    - `BootDevices` → `BootWifi` → `BootBootstrap` → `NormalAwake`;
  - при успешном bootstrap:
    - стартует MQTT (`router::start()`), погода (`http_manager::start_weather_polling()`), UI (`ui_app_init()`), screensaver (`ui::screensaver::init_support()`), input/toggle‑контроллеры и logger;
  - добавлен `idle_controller_task`:
    - раз в 1 сек читает `lv_display_get_inactive_time`;
    - при `NormalAwake` + таймаут → `set_app_state(NormalScreensaver)`.
- Обработка `WAKE_SCREENSAVER`:
  - в `main` зарегистрирован обработчик события `WAKE_SCREENSAVER`;
  - при любом wake‑событии вызывается `set_app_state(AppState::NormalAwake)`.
- Скринсейвер приведён к «чистой» роли UI‑модуля:
  - больше не решает сам, когда включаться — слушает `APP_STATE_CHANGED`;
  - на `NormalScreensaver` вызывает `ui::screensaver::show()`;
  - на `NormalAwake`:
    - включает дисплей, если он был выключен;
    - сбрасывает локальный флаг активности.
- Внутренняя логика скринсейвера:
  - под капотом использует `lvgl_port_lock`/`unlock`, чтобы внешнему коду не нужно было заботиться о блокировке LVGL;
  - в `show()` создаётся таймер `s_backlight_timer` на 5 секунд:
    - через 5 секунд вызывает `backlight_timer_cb`;
    - коллбек логирует сообщение и выключает дисплей через `devices_display_set_enabled(false)`, помечая `s_backlight_off = true`;
  - при `hide_to_room(...)`:
    - загружает экран комнаты;
    - включает дисплей (`devices_display_set_enabled(true)`);
    - сбрасывает `s_active` и `s_backlight_off`.
- Input‑контроллер:
  - все аппаратные события (крутилка, кнопка, жесты) публикуются как `app_events`;
  - на любой ввод публикуется `WAKE_SCREENSAVER`, что в итоге переводит `AppState` в `NormalAwake`.

---

## 2. Следующие шаги (пошаговый план)

1. **Дочистить использование `AppState` в main**
   - [ ] В `enter_config_mode()` явно вызвать `set_app_state(AppState::ConfigMode)` перед запуском AP/HTTP‑конфига.
   - [ ] Убедиться, что нигде кроме `set_app_state()` `g_app_state` не изменяется напрямую.

2. **Сделать NormalSleep осмысленным**
   - [ ] В `idle_controller_task` добавить вторую ступень таймаута:
     - `NormalScreensaver` + дополнительный таймаут → `set_app_state(AppState::NormalSleep)`.
   - [ ] Решить, кто именно реагирует на `NormalSleep`:
     - простой вариант: отдельный модуль/функция, подписанная на `APP_STATE_CHANGED`, которая при `NormalSleep` выключает подсветку и/или переводит MCU в light sleep.

3. **Подписать ещё модули на `APP_STATE_CHANGED`**
   - [ ] Вынести управление подсветкой в отдельный тонкий слой (или использовать существующий `devices_display_*`) и подписать его на `APP_STATE_CHANGED`:
     - `NormalAwake` / `NormalScreensaver` → дисплей включён;
     - `NormalSleep` → дисплей выключен.
   - [ ] Подумать, нужно ли `rooms` что‑то делать на `NormalScreensaver` / `NormalSleep` (скорее всего нет, так как они уже скрыты за скринсейвером).

4. **Начать использовать `REQUEST_*` события**
   - [ ] Определить сценарии, где модули должны просить смену режима:
     - кнопка «настроить» на сплэше;
     - возможные future‑кейсы типа «попросить sleep».
   - [ ] Для кнопки «настроить»:
     - либо из callback в `splash` вызывать `app_events::post_request_config_mode(...)`;
     - либо оставить прямой вызов `enter_config_mode()`, но перевести его на `set_app_state(ConfigMode)` (минимальный шаг).
   - [ ] В `main` зарегистрировать обработчики `REQUEST_CONFIG_MODE`, `REQUEST_SLEEP`, `REQUEST_WAKE` и внутри них вызывать `set_app_state(...)` + необходимую логику (AP, sleep, wake и т.п.).

5. **Постепенно избавляться от прямых зависимостей на `g_app_state`**
   - [ ] Если какие‑то модули читают `g_app_state` напрямую, заменить это на реакцию на `APP_STATE_CHANGED` (подписка + локальное кэширование состояния).
   - [ ] Оставить прочтение `g_app_state` только внутри `main`/контроллеров (например, в `idle_controller_task`).

6. **Документация и проверка на железе**
   - [ ] Обновить `APP_STATE_FSM.md`, чтобы он отражал текущие реализации (скринсейвер, idle‑контроллер, `WAKE_SCREENSAVER`).
   - [ ] На устройстве проверить полный цикл:
     - BootDevices → BootWifi → BootBootstrap → NormalAwake;
     - простой → NormalScreensaver → выключение дисплея через 5 секунд;
     - любое действие пользователя → WAKE_SCREENSAVER → NormalAwake и включение дисплея;
     - после внедрения `NormalSleep` — проверить, что он включается и корректно выходим обратно.

