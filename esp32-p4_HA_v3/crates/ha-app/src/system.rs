//! Time and weather service (port of V2 `system`).
//!
//! A blocking worker thread waits for SNTP, resolves the location (GeoIP / Open-Meteo
//! geocoding), fetches Open-Meteo current weather every 15 minutes and posts canonical
//! records to the main loop. Domain/store/display writes stay in the main thread.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use embedded_svc::http::client::Client;
use embedded_svc::http::Method;
use embedded_svc::utils::io;
use esp_idf_svc::http::client::EspHttpConnection;
use ha_domain::canonical::{
    LocationRecord, WeatherRecord, LOCATION_NAME_MAX, WEATHER_CLEAR, WEATHER_CLEAR_NIGHT,
    WEATHER_CLOUDY, WEATHER_FOG, WEATHER_HAIL, WEATHER_LIGHTNING, WEATHER_LIGHTNING_RAINY,
    WEATHER_OVERCAST, WEATHER_PARTLYCLOUDY, WEATHER_POURING, WEATHER_RAINY, WEATHER_SNOWY,
    WEATHER_SNOWY_RAINY, WEATHER_UNKNOWN,
};

const GEOIP_URL: &str =
    "http://ip-api.com/json/?fields=status,message,city,regionName,lat,lon,timezone,offset";
const WEATHER_URL: &str = "http://api.open-meteo.com/v1/forecast";
const GEOCODE_URL: &str = "http://geocoding-api.open-meteo.com/v1/search";
const HTTP_BUF: usize = 4096;
const WEATHER_PERIOD: Duration = Duration::from_secs(15 * 60);
const RETRY_DELAY: Duration = Duration::from_secs(3);

pub enum Update {
    Location(LocationRecord),
    Weather(WeatherRecord),
}

static STARTED: AtomicBool = AtomicBool::new(false);
static LOCATION: Mutex<Option<LocationRecord>> = Mutex::new(None);
static UPDATES: Mutex<VecDeque<Update>> = Mutex::new(VecDeque::new());

pub fn take() -> Option<Update> {
    UPDATES.lock().unwrap().pop_front()
}

/// Current location snapshot (also updated after every service resolve).
pub fn set_location(location: LocationRecord) {
    *LOCATION.lock().unwrap() = Some(location);
}

fn snapshot() -> Option<LocationRecord> {
    *LOCATION.lock().unwrap()
}

fn post(update: Update) {
    let mut queue = UPDATES.lock().unwrap();
    if queue.len() >= 8 {
        queue.pop_front();
    }
    queue.push_back(update);
}

/// Start the worker once. Safe to call before the network/SNTP are ready.
pub fn start(initial: LocationRecord) {
    if STARTED.swap(true, Ordering::Relaxed) {
        return;
    }
    set_location(initial);
    log::info!("system: service started (waiting for SNTP)");
    let _ = std::thread::Builder::new()
        .name("system".into())
        .stack_size(8192)
        .spawn(system_task);
}

fn system_task() {
    wait_clock();
    let mut handled = snapshot().unwrap_or_default();
    let mut first = true;
    loop {
        let current = snapshot().unwrap_or(handled);
        if first || current != handled {
            handled = current;
            if let Some(resolved) = resolve_location(current) {
                handled = resolved;
                set_location(resolved);
                post(Update::Location(resolved));
            }
        }
        first = false;

        let location = snapshot().unwrap_or(handled);
        if location.latitude.abs() > 1e-4 || location.longitude.abs() > 1e-4 {
            if let Some(weather) = fetch_weather(location.latitude, location.longitude) {
                post(Update::Weather(weather));
            }
        }

        /* Sleep up to the next weather refresh; a manual location edit wakes us early. */
        let mut elapsed = Duration::ZERO;
        while elapsed < WEATHER_PERIOD {
            std::thread::sleep(Duration::from_secs(20));
            elapsed += Duration::from_secs(20);
            if snapshot().unwrap_or(handled) != handled {
                break;
            }
        }
    }
}

fn wait_clock() {
    loop {
        let ready = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|elapsed| elapsed.as_secs() > 1_600_000_000)
            .unwrap_or(false);
        if ready {
            log::info!("system: clock ready");
            return;
        }
        std::thread::sleep(Duration::from_secs(2));
    }
}

fn resolve_location(location: LocationRecord) -> Option<LocationRecord> {
    let position_manual = !location.pos_auto && !location.name_string().is_empty();
    if position_manual {
        let mut resolved = location;
        if resolved.latitude.abs() < 1e-4 && resolved.longitude.abs() < 1e-4 {
            let name = resolved.name_string();
            let Some((lat, lon)) = geocode_city(&name) else {
                log::warn!("system: geocode \"{name}\" failed");
                return None;
            };
            resolved.latitude = lat;
            resolved.longitude = lon;
        }
        if resolved.tz_auto {
            if let Some(geo) = fetch_geoip() {
                resolved.tz_offset_min = (geo.4 / 60) as i16;
            }
        }
        log::info!(
            "system: location manual \"{}\" ({:.4}, {:.4}) tz={}",
            resolved.name_string(),
            resolved.latitude,
            resolved.longitude,
            resolved.tz_offset_min
        );
        return Some(resolved);
    }

    let geo = fetch_geoip()?;
    let (city, region, lat, lon, offset_sec) = geo;
    let mut resolved = location;
    let label = match (city.is_empty(), region.is_empty()) {
        (false, false) => format!("{city}, {region}"),
        (false, true) => city,
        (true, false) => region,
        (true, true) => "Unknown".to_string(),
    };
    resolved.set_name(&label);
    resolved.latitude = lat;
    resolved.longitude = lon;
    resolved.pos_auto = true;
    if resolved.tz_auto {
        resolved.tz_offset_min = (offset_sec / 60) as i16;
    }
    log::info!(
        "system: location auto \"{}\" ({:.4}, {:.4}) tz={}",
        resolved.name_string(),
        resolved.latitude,
        resolved.longitude,
        resolved.tz_offset_min
    );
    Some(resolved)
}

fn fetch_geoip() -> Option<(String, String, f32, f32, i64)> {
    for attempt in 0..3 {
        if attempt > 0 {
            std::thread::sleep(RETRY_DELAY);
        }
        let Ok(text) = http_get(GEOIP_URL) else {
            continue;
        };
        if json_string(&text, "status").as_deref() != Some("success") {
            continue;
        }
        let (Some(lat), Some(lon)) = (json_number(&text, "lat"), json_number(&text, "lon")) else {
            continue;
        };
        let city = json_string(&text, "city").unwrap_or_default();
        let region = json_string(&text, "regionName").unwrap_or_default();
        let offset = json_number(&text, "offset").unwrap_or(0.0) as i64;
        return Some((city, region, lat as f32, lon as f32, offset));
    }
    log::warn!("system: GeoIP unavailable");
    None
}

fn geocode_city(city: &str) -> Option<(f32, f32)> {
    let url = format!(
        "{GEOCODE_URL}?name={}&count=1&language=en&format=json",
        url_encode(city)
    );
    for attempt in 0..3 {
        if attempt > 0 {
            std::thread::sleep(RETRY_DELAY);
        }
        let Ok(text) = http_get(&url) else {
            continue;
        };
        let Some(results) = text.split_once("\"results\":") else {
            continue;
        };
        let (Some(lat), Some(lon)) = (
            json_number(results.1, "latitude"),
            json_number(results.1, "longitude"),
        ) else {
            continue;
        };
        return Some((lat as f32, lon as f32));
    }
    None
}

fn fetch_weather(lat: f32, lon: f32) -> Option<WeatherRecord> {
    let url = format!(
        "{WEATHER_URL}?latitude={lat:.4}&longitude={lon:.4}&current=temperature_2m,\
         relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m,\
         cloud_cover,surface_pressure,is_day"
    );
    for attempt in 0..3 {
        if attempt > 0 {
            std::thread::sleep(RETRY_DELAY);
        }
        let Ok(body) = http_get(&url) else {
            continue;
        };
        let Some(current) = body.split_once("\"current\":") else {
            continue;
        };
        let current = current.1;
        let (Some(temp), Some(code)) = (
            json_number(current, "temperature_2m"),
            json_number(current, "weather_code"),
        ) else {
            continue;
        };
        let is_day = json_number(current, "is_day").map(|v| v != 0.0).unwrap_or(true);
        let humidity = json_number(current, "relative_humidity_2m").unwrap_or(0.0);
        let pressure = json_number(current, "surface_pressure").unwrap_or(0.0);
        let wind = json_number(current, "wind_speed_10m").unwrap_or(0.0);
        let wind_dir = json_number(current, "wind_direction_10m").unwrap_or(0.0);
        let cloud = json_number(current, "cloud_cover").unwrap_or(0.0);
        let weather = WeatherRecord {
            condition: wmo_to_condition(code as i32, is_day),
            cloud_pct: cloud.round().clamp(0.0, 100.0) as u8,
            temperature_c100: (temp * 100.0).round() as i16,
            humidity_p100: (humidity * 100.0).round() as u16,
            pressure_hpa: pressure.round() as u16,
            wind_kmh10: (wind * 10.0).round() as u16,
            wind_dir_deg: wind_dir.round() as u16,
        };
        log::info!(
            "system: weather cond={} t={:.1}C hum={:.0}%",
            weather.condition,
            temp,
            humidity
        );
        return Some(weather);
    }
    log::warn!("system: weather unavailable");
    None
}

fn http_get(url: &str) -> Result<String, String> {
    let mut client = Client::wrap(EspHttpConnection::new(&Default::default()).map_err(esp)?);
    let request = client
        .request(Method::Get, url, &[("accept", "application/json")])
        .map_err(esp)?;
    let mut response = request.submit().map_err(esp)?;
    if response.status() != 200 {
        return Err(format!("http status {}", response.status()));
    }
    let mut buf = [0u8; HTTP_BUF];
    let read = io::try_read_full(&mut response, &mut buf).map_err(|(error, _)| esp(error))?;
    Ok(String::from_utf8_lossy(&buf[..read]).into_owned())
}

fn esp(error: impl std::fmt::Debug) -> String {
    format!("{error:?}")
}

fn json_find<'a>(json: &'a str, key: &str) -> Option<&'a str> {
    let pattern = format!("\"{key}\"");
    let at = json.find(&pattern)? + pattern.len();
    let rest = json[at..].trim_start_matches([' ', '\t', ':']);
    Some(rest)
}

fn json_number(json: &str, key: &str) -> Option<f64> {
    let rest = json_find(json, key)?;
    let mut end = 0;
    for (index, ch) in rest.char_indices() {
        if ch.is_ascii_digit() || matches!(ch, '-' | '+' | '.' | 'e' | 'E') {
            end = index + ch.len_utf8();
        } else {
            break;
        }
    }
    if end == 0 {
        return None;
    }
    rest[..end].parse().ok()
}

fn json_string(json: &str, key: &str) -> Option<String> {
    let rest = json_find(json, key)?;
    if !rest.starts_with('"') {
        return None;
    }
    let mut out = String::new();
    let mut escaped = false;
    for ch in rest[1..].chars() {
        if escaped {
            out.push(ch);
            escaped = false;
        } else if ch == '\\' {
            escaped = true;
        } else if ch == '"' {
            return Some(out);
        } else {
            out.push(ch);
        }
    }
    None
}

fn url_encode(text: &str) -> String {
    const HEX: &[u8] = b"0123456789ABCDEF";
    let mut out = String::new();
    for byte in text.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b'~') {
            out.push(byte as char);
        } else {
            out.push('%');
            out.push(HEX[(byte >> 4) as usize] as char);
            out.push(HEX[(byte & 0x0f) as usize] as char);
        }
    }
    out
}

fn wmo_to_condition(code: i32, is_day: bool) -> u8 {
    match code {
        0 | 1 => {
            if is_day {
                WEATHER_CLEAR
            } else {
                WEATHER_CLEAR_NIGHT
            }
        }
        2 => WEATHER_PARTLYCLOUDY,
        3 => WEATHER_OVERCAST,
        45 | 48 => WEATHER_FOG,
        51 | 53 | 55 | 56 | 57 | 61 | 63 | 65 => WEATHER_RAINY,
        66 | 67 => WEATHER_SNOWY_RAINY,
        71 | 73 | 75 | 77 | 85 | 86 => WEATHER_SNOWY,
        80 | 81 | 82 => WEATHER_POURING,
        95 => WEATHER_LIGHTNING,
        96 | 99 => WEATHER_LIGHTNING_RAINY,
        4 => WEATHER_CLOUDY,
        11 => WEATHER_HAIL,
        _ => WEATHER_UNKNOWN,
    }
}

trait LocationName {
    fn name_string(&self) -> String;
    fn set_name(&mut self, name: &str);
}

impl LocationName for LocationRecord {
    fn name_string(&self) -> String {
        let end = self
            .name
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(self.name.len());
        String::from_utf8_lossy(&self.name[..end]).into_owned()
    }

    fn set_name(&mut self, name: &str) {
        self.name = [0u8; LOCATION_NAME_MAX];
        let bytes = name.as_bytes();
        let len = bytes.len().min(LOCATION_NAME_MAX);
        self.name[..len].copy_from_slice(&bytes[..len]);
    }
}
