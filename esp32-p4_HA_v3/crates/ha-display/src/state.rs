use super::*;

pub fn start(domain: &DomainHandle) -> bool {
    let rc = unsafe { ha_display_start() };
    if rc != 0 {
        log::error!("display: start failed ({rc})");
        return false;
    }
    if !lock() {
        log::error!("display: lock timeout in start");
        return false;
    }
    let (time, location, weather_icon, weather_label, wifi_bars, wifi_box, wifi_cross) =
        build_status_bar();
    *UI.lock().unwrap() = Some(Ui {
        time,
        location,
        weather_icon,
        weather_label,
        wifi_bars,
        wifi_box,
        wifi_cross,
        wifi_level: -1,
        wifi_color: 0,
    });
    unsafe {
        build_menu();
    }
    unlock();
    *DOMAIN.lock().unwrap() = Some(domain.clone());
    STARTED.store(true, Ordering::Relaxed);
    initial_read(domain);
    rebuild(domain);
    let (tx, rx) = sync_channel(64);
    if let Err(error) = domain.subscribe_channel(Filter::all(), tx) {
        log::error!("display: subscribe failed: {error:?}");
        return false;
    }
    *DISPLAY_RX.lock().unwrap() = Some(rx);
    let timer = unsafe { lv_timer_create(Some(display_timer), 20, null_mut()) };
    if timer.is_null() {
        log::error!("display: timer create failed");
        return false;
    }
    log::info!("display: port ready");
    true
}

unsafe extern "C" fn display_timer(_timer: *mut lv_timer_t) {
    let Ok(domain) = domain() else {
        return;
    };
    while let Some(nav) = take_nav() {
        match nav {
            Nav::WifiScan => {
                if let Err(error) = request_wifi_scan() {
                    log::warn!("wifi: scan request rejected ({error:?})");
                }
            }
            _ => navigate(&domain, nav),
        }
    }
    if let Some(receiver) = DISPLAY_RX.lock().unwrap().as_ref() {
        drain(&domain, receiver);
    }
    let poll = {
        let mut last = LAST_CLOCK_POLL.lock().unwrap();
        let due = last
            .map(|at| at.elapsed() >= Duration::from_millis(500))
            .unwrap_or(true);
        if due {
            *last = Some(Instant::now());
        }
        due
    };
    if poll {
        sync_clock(&domain);
    }
    tick();
}

/// Status bar must converge even if a time fact is dropped or arrives late.
fn initial_read(domain: &DomainHandle) {
    set_brightness(display_settings(domain).brightness_pct);

    let mut location = [0u8; LOCATION_RECORD_LEN];
    if domain
        .entity_get(
            ENTITY_LOCATION,
            &SYSTEM_DEVICE_UID.to_le_bytes(),
            &mut location,
        )
        .is_ok()
    {
        if let Some(record) = LocationRecord::decode(&location) {
            set_location(&location_text(&record));
        }
    }

    let mut weather = [0u8; WEATHER_RECORD_LEN];
    if domain
        .entity_get(
            ENTITY_WEATHER,
            &WEATHER_DEVICE_UID.to_le_bytes(),
            &mut weather,
        )
        .is_ok()
    {
        if let Some(record) = WeatherRecord::decode(&weather) {
            set_weather(
                record.condition,
                f32::from(record.temperature_c100) / 100.0,
                f32::from(record.humidity_p100) / 100.0,
            );
        }
    }

    if let (Some(hour), Some(minute)) = (
        read_state(domain, ENTITY_SYSTEM, PROPERTY_SYSTEM_HOUR).and_then(|value| numeric(&value)),
        read_state(domain, ENTITY_SYSTEM, PROPERTY_SYSTEM_MINUTE).and_then(|value| numeric(&value)),
    ) {
        let clock = (
            hour.round().clamp(0.0, 23.0) as u8,
            minute.round().clamp(0.0, 59.0) as u8,
        );
        *CLOCK_LAST.lock().unwrap() = clock;
        set_clock(clock.0, clock.1);
    }

    let mut wifi = [0u8; WIFI_STATUS_RECORD_LEN];
    if domain
        .entity_get(
            ENTITY_WIFI_STATUS,
            &WIFI_DEVICE_UID.to_le_bytes(),
            &mut wifi,
        )
        .is_ok()
    {
        if let Some(record) = WifiStatusRecord::decode(&wifi) {
            set_wifi((record.connected != 0).then_some(i16::from(record.rssi)));
        }
    }
}

/// Local clock (status bar), 24-hour.
fn rebuild(domain: &DomainHandle) {
    if !STARTED.load(Ordering::Relaxed) {
        return;
    }
    let groups = collect_groups(domain);
    *GROUPS.lock().unwrap() = groups.clone();
    if VIEW.load(Ordering::Relaxed) != View::Page as u8 {
        return;
    }
    let current = CURRENT.load(Ordering::Relaxed);
    let index = if groups.is_empty() {
        0
    } else {
        current.min(groups.len() - 1)
    };
    if !lock() {
        log::warn!("display: lock timeout in rebuild");
        return;
    }
    unsafe {
        let page = build_page(domain, &groups, index);
        log::debug!(
            "display: page rebuilt groups={} index={} widgets={} colors={}",
            groups.len(),
            index,
            page.widgets.len(),
            page.colors.len()
        );
        replace_page(page);
    }
    CURRENT.store(index, Ordering::Relaxed);
    unlock();
}

/// Pending navigation from touch (arrows/menu; the main loop drains the queue).
pub fn take_nav() -> Option<Nav> {
    NAV.lock().unwrap().pop_front()
}

pub fn navigate(domain: &DomainHandle, nav: Nav) {
    if !STARTED.load(Ordering::Relaxed) {
        return;
    }
    match nav {
        Nav::Delta(_) | Nav::Index(_) => {
            let groups = GROUPS.lock().unwrap().clone();
            if groups.is_empty() {
                return;
            }
            let current = CURRENT.load(Ordering::Relaxed);
            let len = groups.len();
            let next = match nav {
                Nav::Delta(delta) => {
                    let mut next = current as i32 + delta;
                    next = ((next % len as i32) + len as i32) % len as i32;
                    next as usize
                }
                Nav::Index(index) => index.min(len - 1),
                _ => return,
            };
            if next == current {
                return;
            }
            if !lock() {
                return;
            }
            unsafe {
                let page = build_page(domain, &groups, next);
                replace_page(page);
            }
            CURRENT.store(next, Ordering::Relaxed);
            unlock();
            VIEW.store(View::Page as u8, Ordering::Relaxed);
        }
        Nav::Back => {
            let groups = GROUPS.lock().unwrap().clone();
            let current = CURRENT.load(Ordering::Relaxed);
            let index = if groups.is_empty() {
                0
            } else {
                current.min(groups.len() - 1)
            };
            if !lock() {
                return;
            }
            unsafe {
                hide_menu();
                close_wifi_dialog();
                let page = build_page(domain, &groups, index);
                replace_page(page);
            }
            CURRENT.store(index, Ordering::Relaxed);
            unlock();
            VIEW.store(View::Page as u8, Ordering::Relaxed);
        }
        Nav::OpenSettings => {
            if !lock() {
                return;
            }
            unsafe {
                hide_menu();
                close_wifi_dialog();
                let page = build_settings(domain);
                replace_page(page);
            }
            unlock();
            VIEW.store(View::Settings as u8, Ordering::Relaxed);
        }
        Nav::OpenWifi => {
            if !lock() {
                return;
            }
            unsafe {
                hide_menu();
                let page = build_wifi(domain);
                replace_page(page);
            }
            unlock();
            VIEW.store(View::Wifi as u8, Ordering::Relaxed);
        }
        Nav::WifiScan => { /* handled by the main loop (blocking C scan), not a screen switch */ }
    }
}

pub fn drain(domain: &DomainHandle, receiver: &Receiver<Fact>) {
    let mut rebuild_needed = false;
    while let Ok(fact) = receiver.try_recv() {
        match fact.kind {
            FactKind::EntityUpserted if fact.entity == ENTITY_PROPERTY_STATE => {
                let key = fact.key();
                if key.len() < 16 {
                    continue;
                }
                let Some(key) = StateKey::from_bytes(key) else {
                    continue;
                };
                let entity = key.entity;
                let property = key.property;
                if entity == ENTITY_SYSTEM
                    && matches!(property, PROPERTY_SYSTEM_HOUR | PROPERTY_SYSTEM_MINUTE)
                {
                    let Some(value) = numeric(&fact.value) else {
                        continue;
                    };
                    let mut clock = CLOCK_LAST.lock().unwrap();
                    if property == PROPERTY_SYSTEM_HOUR {
                        clock.0 = value.round().clamp(0.0, 23.0) as u8;
                    } else {
                        clock.1 = value.round().clamp(0.0, 59.0) as u8;
                    }
                    let (hour, minute) = *clock;
                    drop(clock);
                    set_clock(hour, minute);
                    continue;
                }
                on_state(entity, property, fact.value);
            }
            FactKind::EntityUpserted | FactKind::EntityRemoved => match fact.entity {
                ENTITY_LOCATION => {
                    if fact.kind == FactKind::EntityUpserted {
                        let mut bytes = [0u8; LOCATION_RECORD_LEN];
                        if domain
                            .entity_get(ENTITY_LOCATION, fact.key(), &mut bytes)
                            .is_ok()
                        {
                            if let Some(location) = LocationRecord::decode(&bytes) {
                                set_location(&location_text(&location));
                            }
                        }
                    }
                }
                ENTITY_WEATHER => {
                    if fact.kind == FactKind::EntityUpserted {
                        let mut bytes = [0u8; WEATHER_RECORD_LEN];
                        if domain
                            .entity_get(ENTITY_WEATHER, fact.key(), &mut bytes)
                            .is_ok()
                        {
                            if let Some(weather) = WeatherRecord::decode(&bytes) {
                                set_weather(
                                    weather.condition,
                                    f32::from(weather.temperature_c100) / 100.0,
                                    f32::from(weather.humidity_p100) / 100.0,
                                );
                            }
                        }
                    }
                }
                ENTITY_SETTINGS => {
                    if fact.kind == FactKind::EntityUpserted {
                        let mut bytes = [0u8; SETTINGS_RECORD_LEN];
                        if domain
                            .entity_get(ENTITY_SETTINGS, fact.key(), &mut bytes)
                            .is_ok()
                        {
                            if let Some(settings) = SettingsRecord::decode(&bytes) {
                                set_brightness(settings.brightness_pct);
                            }
                        }
                    }
                }
                ENTITY_AUTOMATION => {
                    if fact.kind == FactKind::EntityUpserted {
                        if let Ok(id) = fact.key().try_into().map(u64::from_le_bytes) {
                            refresh_automation(domain, id);
                        }
                    } else {
                        rebuild_needed = true;
                    }
                }
                ENTITY_LOGICAL_DEVICE
                | ENTITY_ENTITY
                | ENTITY_CAPABILITIES
                | ENTITY_GROUP
                | ENTITY_GROUP_ITEM
                | ENTITY_GROUP_AUTOMATION => rebuild_needed = true,
                ENTITY_WIFI_STATUS => {
                    if fact.kind == FactKind::EntityUpserted {
                        if let Some(status) = wifi_status(domain) {
                            set_wifi((status.connected != 0).then_some(i16::from(status.rssi)));
                        }
                    }
                    refresh_wifi(domain);
                }
                ENTITY_WIFI_SCAN => refresh_wifi(domain),
                _ => {}
            },
            _ => {}
        }
    }
    if rebuild_needed {
        rebuild(domain);
    }
}

fn refresh_automation(domain: &DomainHandle, automation: u64) {
    let mut bytes = [0u8; AUTOMATION_RECORD_LEN];
    if domain
        .entity_get(ENTITY_AUTOMATION, &automation.to_le_bytes(), &mut bytes)
        .is_err()
    {
        return;
    }
    let Some(rule) = RuleRecord::decode(&bytes).ok() else {
        return;
    };
    let entries: Vec<(*mut lv_obj_t, *mut lv_obj_t)> = {
        let page = PAGE.lock().unwrap();
        let Some(page) = page.as_ref() else {
            return;
        };
        page.automations
            .iter()
            .filter(|entry| entry.automation == automation)
            .map(|entry| (entry.title, entry.switch))
            .collect()
    };
    if entries.is_empty() || !lock() {
        return;
    }
    unsafe {
        for (title, switch) in entries {
            if rule.name[0] != 0 {
                set_label(title, &shorten(&c_string(&rule.name), 24));
            }
            if rule.enabled != 0 {
                lv_obj_add_state(switch, lv_state_t_LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(switch, lv_state_t_LV_STATE_CHECKED);
            }
        }
    }
    unlock();
}
fn location_text(location: &LocationRecord) -> String {
    let end = location
        .name
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(location.name.len());
    String::from_utf8_lossy(&location.name[..end]).into_owned()
}

/// Apply one confirmed property state to the widgets of the current page.
fn on_state(entity: EntityId, property: PropertyId, value: Value) {
    if property == PROPERTY_COLOR_X || property == PROPERTY_COLOR_Y {
        let Some(number) = numeric(&value) else {
            return;
        };
        let entries: Vec<ColorEntry> = {
            let mut guard = PAGE.lock().unwrap();
            let Some(page) = guard.as_mut() else {
                return;
            };
            for entry in &mut page.colors {
                if entry.entity != entity {
                    continue;
                }
                if property == PROPERTY_COLOR_X {
                    entry.x = number;
                } else {
                    entry.y = number;
                }
            }
            page.colors
                .iter()
                .filter(|entry| entry.entity == entity)
                .copied()
                .collect()
        };
        if entries.is_empty() {
            return;
        }
        if !lock() {
            return;
        }
        for entry in entries {
            let (r, g, b) = xy_to_rgb8(entry.x, entry.y);
            let (hue, sat, _) = rgb8_to_hsv(r, g, b);
            unsafe {
                lv_obj_set_style_bg_color(entry.swatch, color(rgb_hex(r, g, b)), 0);
                lv_slider_set_value(entry.hue, hue.round() as i32, false);
                lv_slider_set_value(entry.sat, (sat * 100.0).round() as i32, false);
            }
            set_label(entry.hue_label, &format!("{}°", hue.round() as i32));
            set_label(
                entry.sat_label,
                &format!("{} %", (sat * 100.0).round() as i32),
            );
        }
        unlock();
        return;
    }

    let entries: Vec<(WidgetKind, *mut lv_obj_t, *mut lv_obj_t, *mut CmdCtx)> = {
        let page = PAGE.lock().unwrap();
        let Some(page) = page.as_ref() else {
            return;
        };
        page.widgets
            .iter()
            .filter(|widget| widget.entity == entity && widget.property == property)
            .map(|widget| (widget.kind, widget.control, widget.value_label, widget.ctx))
            .collect()
    };
    if entries.is_empty() {
        return;
    }
    if !lock() {
        return;
    }
    for (kind, control, value_label, ctx) in entries {
        match kind {
            WidgetKind::Switch => {
                let Some(on) = value.as_bool() else {
                    continue;
                };
                if ctx.is_null() {
                    continue;
                }
                let ctx = unsafe { &*ctx };
                if ctx.syncing.load(Ordering::Relaxed) {
                    continue;
                }
                let bits = u32::from(on);
                let has_last = ctx.has_last.load(Ordering::Relaxed);
                let last_bits = ctx.last_bits.load(Ordering::Relaxed);
                if ctx.locked.load(Ordering::Relaxed) {
                    if has_last && bits == last_bits {
                        continue;
                    }
                    log::debug!(
                        "display: switch fresh entity={:016x} bits={} last={} unlock",
                        ctx.entity,
                        bits,
                        last_bits
                    );
                    *ctx.deadline.lock().unwrap() = None;
                    ctx.locked.store(false, Ordering::Relaxed);
                    unsafe { lv_obj_remove_state(control, lv_state_t_LV_STATE_DISABLED) };
                } else if has_last && bits == last_bits {
                    continue;
                }
                log::debug!("display: switch apply entity={:016x} on={}", ctx.entity, on);
                ctx.syncing.store(true, Ordering::Relaxed);
                unsafe {
                    if on {
                        lv_obj_add_state(control, lv_state_t_LV_STATE_CHECKED);
                    } else {
                        lv_obj_remove_state(control, lv_state_t_LV_STATE_CHECKED);
                    }
                }
                ctx.syncing.store(false, Ordering::Relaxed);
                ctx.last_bits.store(bits, Ordering::Relaxed);
                ctx.has_last.store(true, Ordering::Relaxed);
            }
            WidgetKind::Slider => {
                if unsafe { lv_obj_has_state(control, lv_state_t_LV_STATE_PRESSED) } {
                    continue;
                }
                if let Some(number) = numeric(&value) {
                    let (min, max) = if property == PROPERTY_COLOR_TEMPERATURE {
                        (2000, 6500)
                    } else {
                        (0, 100)
                    };
                    unsafe {
                        lv_slider_set_value(control, (number.round() as i32).clamp(min, max), false)
                    };
                    set_label(value_label, &format_value(property, &value));
                }
            }
            WidgetKind::Value => {
                set_label(value_label, &format_value(property, &value));
            }
        }
    }
    unlock();
}

/// V2-style switch lock: re-enable a switch after the command settles, even if
/// the domain never reports a fresh value.
pub fn tick() {
    let contexts: Vec<*mut CmdCtx> = {
        let page = PAGE.lock().unwrap();
        page.as_ref()
            .map(|page| {
                page.widgets
                    .iter()
                    .filter(|widget| widget.kind == WidgetKind::Switch && !widget.ctx.is_null())
                    .map(|widget| widget.ctx)
                    .collect()
            })
            .unwrap_or_default()
    };
    if contexts.is_empty() {
        return;
    }
    let now = Instant::now();
    for ctx in contexts {
        let ctx = unsafe { &*ctx };
        if !ctx.locked.load(Ordering::Relaxed) {
            continue;
        }
        let expired = ctx
            .deadline
            .lock()
            .unwrap()
            .map(|deadline| now >= deadline)
            .unwrap_or(false);
        if !expired {
            continue;
        }
        if !lock() {
            return;
        }
        log::debug!("display: switch unlock timeout entity={:016x}", ctx.entity);
        ctx.locked.store(false, Ordering::Relaxed);
        *ctx.deadline.lock().unwrap() = None;
        if !ctx.control.is_null() {
            unsafe { lv_obj_remove_state(ctx.control, lv_state_t_LV_STATE_DISABLED) };
        }
        unlock();
    }
}
