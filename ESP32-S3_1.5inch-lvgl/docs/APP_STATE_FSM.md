# AppState + Event FSM: План внедрения

Этот документ фиксирует обновлённый план архитектуры вокруг глобального состояния приложения (`AppState`) и событийной шины. Он дополняет общий `ARCHITECTURE.md` более конкретным сценарием для нашего проекта.

---

## 1. Цели

- Иметь один источник правды о режиме приложения: `AppState`.
- Распространять смену состояний по системе через событие `APP_STATE_CHANGED`.
- Разделить ответственность:
  - `main` (app_main) управляет `AppState` и принимает глобальные решения.
  - Модули *не меняют* `AppState` напрямую, но реагируют на его изменения в своей зоне.
  - Модули могут просить смену состояния через события‑запросы (`REQUEST_*`).

---

## 2. Глобальное состояние `AppState`

Живёт в `main/main.cpp` рядом с `app_main`:

```cpp
enum class AppState
{
    BootDevices,       // инициализация дисплея, тача, LVGL
    BootWifi,          // запуск Wi‑Fi стека, NVS, базовый конфиг
    BootBootstrap,     // bootstrap состояния из HA
    NormalAwake,       // обычная работа: UI комнат, MQTT, подсветка включена
    NormalScreensaver, // показ скринсейвера (подсветка включена)
    NormalSleep,       // скринсейвер, подсветка/панель выключены, возможен light sleep
    ConfigMode,        // режим точки доступа + HTTP веб‑настройка
};

static AppState g_app_state = AppState::BootDevices;
```

Ключевые правила:

- `g_app_state` изменяется **только** в `main.cpp` (или рядом, в контроллере).
- При каждом изменении вызывается helper `set_app_state(new_state)`, который:
  - проверяет, что состояние реально меняется;
  - обновляет `g_app_state`;
  - публикует событие `APP_STATE_CHANGED { old_state, new_state }` через `app_events`.

---

## 3. Новые события в `app_events`

### 3.1. APP_STATE_CHANGED

В `main/app/app_events.hpp`:

```cpp
enum Id : int32_t
{
    ...
    APP_STATE_CHANGED = 100,
};

struct AppStateChangedPayload
{
    int old_state; // static_cast<int>(AppState)
    int new_state; // static_cast<int>(AppState)
};

esp_err_t post_app_state_changed(AppState old_state, AppState new_state);
```

В `main/app/app_events.cpp`:

- стандартная реализация через `esp_event_post(APP_EVENTS, APP_STATE_CHANGED, &payload, sizeof(payload), 0);`

### 3.2. События‑запросы состояний (REQUEST_*)

Для того, чтобы модули *просили* смену состояния, а не делали её сами, вводим запросы, которые обрабатывает только `main`:

- `REQUEST_CONFIG_MODE` — пользователь нажал “настроить” или аналог;
- `REQUEST_SLEEP` — idle‑логика/модуль хочет перевести систему в sleep;
- `REQUEST_WAKE` — пользователь разбудил устройство (тап/кнопка).

Структурно:

```cpp
enum Id : int32_t
{
    ...
    REQUEST_CONFIG_MODE = 110,
    REQUEST_SLEEP       = 111,
    REQUEST_WAKE        = 112,
};

struct EmptyPayload
{
    std::int64_t timestamp_us = 0;
};

esp_err_t post_request_config_mode(std::int64_t timestamp_us, bool from_isr);
esp_err_t post_request_sleep(std::int64_t timestamp_us, bool from_isr);
esp_err_t post_request_wake(std::int64_t timestamp_us, bool from_isr);
```

---

## 4. Роль `main.cpp` как контроллера

### 4.1. Helper `set_app_state`

В `main.cpp`:

```cpp
static void set_app_state(AppState new_state)
{
    if (g_app_state == new_state)
    {
        return;
    }
    AppState old = g_app_state;
    g_app_state = new_state;
    (void)app_events::post_app_state_changed(old, new_state);
}
```

Все прямые присваивания `g_app_state = ...` постепенно заменяются на `set_app_state(...)`.

### 4.2. Boot‑последовательность в терминах состояний

В `app_main`:

1. `set_app_state(AppState::BootDevices);`
   - `devices_init()`;
   - `ui::splash::show(&enter_config_mode);`
   - `ui::splash::update_state(10, "WiFi...");`
2. `set_app_state(AppState::BootWifi);`
   - `wifi_manager_init();`
   - `config_store::init();`
3. Если конфиг есть, продолжаем; если нет — даём пользователю кнопку "настроить" (сплэш уже её предоставляет).
4. `set_app_state(AppState::BootBootstrap);`
   - `ui::splash::update_state(50, "bootstrap HA...");`
   - `http_manager::bootstrap_state()` (в цикле/с ретраями, если нужно).
5. При успешном bootstrap:
   - `ui::splash::update_state(100, "UI...");`
   - `wifi_manager_start_auto(...)`;
   - `router::start();`
   - `ui::screensaver::init_support();`
   - `ui_app_init();`
   - `set_app_state(AppState::NormalAwake);`
   - запуск `idle_controller_task`;
   - `ui::splash::destroy();`
   - `http_manager::start_weather_polling();`
6. При запросе config (событие `REQUEST_CONFIG_MODE` или callback “настроить”):
   - `set_app_state(AppState::ConfigMode);`
   - `enter_config_mode();` (AP + HTTP UI, "парковка" основного потока).

### 4.3. Idle‑контроллер (скринсейвер/сон)

Задача `idle_controller_task` не дергает UI напрямую — только меняет AppState:

```cpp
for (;;)
{
    uint32_t inactive_ms = ... // через lv_display_get_inactive_time()

    switch (g_app_state)
    {
    case AppState::NormalAwake:
        if (inactive_ms >= kScreensaverTimeoutMs)
            set_app_state(AppState::NormalScreensaver);
        break;

    case AppState::NormalScreensaver:
        if (inactive_ms >= kScreensaverTimeoutMs + kSleepTimeoutMs)
            set_app_state(AppState::NormalSleep);
        break;

    default:
        break;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

В ответ на `APP_STATE_CHANGED` UI‑модули сами решают, что делать (см. ниже).

### 4.4. Обработка запросов состояний (REQUEST_*)

В `main` регистрируется обработчик `APP_EVENTS`:

- `REQUEST_CONFIG_MODE` → `set_app_state(ConfigMode)` + `enter_config_mode()`;
- `REQUEST_SLEEP` → `set_app_state(NormalSleep)`;
- `REQUEST_WAKE` →
  - если `NormalScreensaver`/`NormalSleep` — `set_app_state(NormalAwake)`;
  - иначе игнор.

Так модули могут инициировать смену режима, не трогая `AppState` напрямую.

---

## 5. Реакция модулей на `AppState`

Каждый модуль подписывается на `APP_STATE_CHANGED` и реагирует только в своей зоне ответственности.

### 5.1. ui/screensaver.cpp

Подписчик на `APP_STATE_CHANGED`:

- при `NormalScreensaver`:
  - вызывает `ui::screensaver::show();`
- при `NormalSleep`:
  - может выключать подсветку через `devices_display_set_enabled(false);` (или это делает отдельный модуль подсветки);
- при `NormalAwake`:
  - ничего не делает или только сбрасывает локальные флаги (`s_active`), если `rooms` уже переключил экран.

Скринсейвер сам не считает idle‑время и не меняет `AppState`.

### 5.2. ui/rooms.cpp

Реакция на `APP_STATE_CHANGED`:

- при `NormalAwake`:
  - показывает актуальную комнату (например, через `show_initial_room()` при первом запуске или сохранённый индекс);
- при `NormalScreensaver`:
  - не обязателен явный код — экраны комнат остаются в памяти, но не активны;
- при `NormalSleep`:
  - можно ничего не делать; выключение подсветки/сон — задача других модулей.

### 5.3. Подсветка / устройства (devices_init или отдельный модуль)

Реакция на `APP_STATE_CHANGED`:

- при `NormalSleep` → `devices_display_set_enabled(false);`
- при `NormalAwake`/`NormalScreensaver` → `devices_display_set_enabled(true);`

Можно оформить как тонкий модуль `backlight_controller`, подписанный на события состояния.

### 5.4. wifi_manager и config_server

На старте:

- `BootWifi` / `BootBootstrap` используются только в main для пошагового запуска.

В режиме config:

- переход в `ConfigMode` инициируется main;
- сам вход в AP+HTTP UI остаётся в `enter_config_mode()` (в перспективе можно завести обработчик `APP_STATE_CHANGED` с `ConfigMode` и перенести туда).

### 5.5. http_manager / router

Реакция на `AppState` в перспективе:

- при `BootBootstrap` — выполнять bootstrap (уже делается через прямой вызов из main);
- при `Normal*` — запуск/остановка периодических задач (погода, MQTT).

Пока это остаётся под управлением main, но при необходимости можно перевести на реакцию на `APP_STATE_CHANGED`.

---

## 6. Input‑слой и запросы состояний

`app/input_controller` остаётся единой точкой входа для пользовательского ввода.

Роль:

- слушает кнопки/энкодер/LVGL‑жесты;
- публикует факты (`KNOB`, `BUTTON`, `GESTURE`);
- генерирует более высокоуровневые события:
  - `NAVIGATE_ROOM` — перелистывание комнат;
  - `TOGGLE_CURRENT_ENTITY` — переключение текущей сущности;
  - `WAKE_SCREENSAVER` или сразу `REQUEST_WAKE`, если тап/кнопка произошли, когда активно `NormalScreensaver/NormalSleep`;
  - `REQUEST_CONFIG_MODE` — по нажатию кнопки “настроить” (или это делает сплэш через callback, а main кидает REQUEST_CONFIG_MODE).

Важно:

- ни один UI‑модуль не изменяет `AppState` напрямую;
- максимум — публикует `REQUEST_*` события, которые обрабатываются в `main`.

---

## 7. Краткое резюме

- `AppState` — один глобальный enum в `main.cpp`.
- Только `main` меняет `AppState` через `set_app_state()` и публикует `APP_STATE_CHANGED`.
- Модули:
  - реагируют на `APP_STATE_CHANGED` в рамках своей ответственности (UI, подсветка, транспорт);
  - публикуют факты и запросы (`REQUEST_*`), но не трогают `AppState`.
- Idle‑логика (скринсейвер/сон) и режимы (config/normal) реализуются как простая машина состояний в main, а детали поведения распределены по модулям.

Этот подход даёт:

- прозрачную и централизованную логику переходов между режимами;
- модульность (каждый файл – “маленькое приложение” с чётким контрактом);
- предсказуемость (достаточно смотреть на `AppState` и `APP_STATE_CHANGED`, чтобы понять, что происходит в системе).

