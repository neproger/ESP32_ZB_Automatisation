//! HTTP + binary WebSocket for the UI.
//!
//! The wire is V2 canonical: 8-byte header, entity frames carry `key + record`
//! verbatim, `EVENT` carries `ha_event_t`. The main loop mirrors canonical
//! records into a bounded model cache; `/ws` sends a snapshot on connect and
//! every change afterwards. `/model.bin` stays 404 on purpose: the UI falls back
//! to a full WS snapshot.

use std::collections::VecDeque;
use std::ffi::c_char;
use std::sync::atomic::{AtomicBool, AtomicU16, Ordering};
use std::sync::mpsc::Receiver;
use std::sync::Mutex;

use esp_idf_svc::sys::*;
use ha_domain::{DomainHandle, Fact, FactKind};
use ha_web::{command, control, write_frame};

const INDEX: &[u8] = include_bytes!("../../../web/ui/index.html.gz");
const APP_JS: &[u8] = include_bytes!("../../../web/ui/app.js.gz");
const APP_CSS: &[u8] = include_bytes!("../../../web/ui/app.css.gz");

const GET_METHOD: httpd_method_t = http_method_HTTP_GET as httpd_method_t;
const MAX_CLIENTS: usize = 4;
const CLIENT_QUEUE: usize = 64;
// Full 32-device config: devices/entities/caps, 256 states, groups, rules.
const MAX_MODEL: usize = 768;

struct ModelEntry {
    frame_type: u8,
    key: Vec<u8>,
    payload: Vec<u8>,
}

struct Client {
    fd: i32,
    queue: VecDeque<Vec<u8>>,
}

static MODEL: Mutex<Vec<ModelEntry>> = Mutex::new(Vec::new());
static CLIENTS: Mutex<Vec<Client>> = Mutex::new(Vec::new());
static SERVER: Mutex<Option<usize>> = Mutex::new(None);
static SEQ: AtomicU16 = AtomicU16::new(0);
static MODEL_EVICTED: AtomicBool = AtomicBool::new(false);

extern "C" {
    fn zb_poc_open_network(seconds: u8) -> i32;
}

pub fn start() -> bool {
    let mut config: httpd_config_t = unsafe { std::mem::zeroed() };
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = MAX_CLIENTS as u16 + 1;
    config.max_uri_handlers = 8;
    config.max_resp_headers = 8;
    config.backlog_conn = 5;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.stack_size = 8192;
    config.task_priority = 5;
    config.core_id = 0x7fff_ffff; // tskNO_AFFINITY
    config.task_caps = MALLOC_CAP_DEFAULT;

    let mut handle: httpd_handle_t = std::ptr::null_mut();
    let err = unsafe { httpd_start(&mut handle, &config) };
    if err != ESP_OK || handle.is_null() {
        log::error!("httpd_start failed: {err}");
        return false;
    }
    *SERVER.lock().unwrap() = Some(handle as usize);

    let ok = register(handle, "/", GET_METHOD, index_handler)
        && register(handle, "/app.js", GET_METHOD, js_handler)
        && register(handle, "/app.css", GET_METHOD, css_handler)
        && register(handle, "/model.bin", GET_METHOD, model_handler)
        && register_ws(handle, "/ws", ws_handler);
    if !ok {
        log::error!("httpd handler registration failed");
    }
    log::info!("web server on :80 (/ws)");
    ok
}

/// Drain the Web inbox. Domain facts are copied into the existing bounded
/// model; WebSocket sends happen here, never under Domain lock.
pub fn drain(domain: &DomainHandle, receiver: &Receiver<Fact>) {
    while let Ok(fact) = receiver.try_recv() {
        match fact.kind {
            FactKind::EntityUpserted => {
                let Some(record_len) = domain.entity_record_len(fact.entity).ok() else {
                    continue;
                };
                let mut record = vec![0u8; record_len];
                if domain
                    .entity_get(fact.entity, fact.key(), &mut record)
                    .is_err()
                {
                    continue;
                }
                if let Ok(frame_type) = u8::try_from(fact.entity) {
                    upsert(frame_type, fact.key(), &record);
                }
            }
            FactKind::EntityRemoved => {
                if let Ok(frame_type) = u8::try_from(fact.entity) {
                    remove(frame_type, fact.key());
                }
            }
            FactKind::Event => {
                let key = fact.key();
                if key.len() < 8 {
                    continue;
                }
                let entity = u64::from_le_bytes(key[..8].try_into().expect("event entity"));
                if let Some(event) = fact.value.as_u32() {
                    publish_event(entity, event);
                }
            }
            _ => {}
        }
    }
}

/// Mirrors one canonical record; an unchanged record is not broadcast again.
pub fn upsert(frame_type: u8, key: &[u8], record: &[u8]) {
    let mut payload = Vec::with_capacity(key.len() + record.len());
    payload.extend_from_slice(key);
    payload.extend_from_slice(record);
    let changed = {
        let mut model = MODEL.lock().unwrap();
        match model
            .iter_mut()
            .find(|entry| entry.frame_type == frame_type && entry.key.as_slice() == key)
        {
            Some(entry) if entry.payload == payload => false,
            Some(entry) => {
                entry.payload = payload.clone();
                true
            }
            None => {
                if model.len() == MAX_MODEL {
                    if !MODEL_EVICTED.swap(true, Ordering::Relaxed) {
                        log::warn!("web: model full, evicting oldest records");
                    }
                    model.remove(0);
                }
                model.push(ModelEntry {
                    frame_type,
                    key: key.to_vec(),
                    payload: payload.clone(),
                });
                true
            }
        }
    };
    if changed {
        broadcast_one(frame_type, &payload);
    }
}

/// Drops one cached record and tells clients to remove it too.
pub fn remove(frame_type: u8, key: &[u8]) {
    let changed = {
        let mut model = MODEL.lock().unwrap();
        match model
            .iter()
            .position(|entry| entry.frame_type == frame_type && entry.key.as_slice() == key)
        {
            Some(index) => {
                model.remove(index);
                true
            }
            None => false,
        }
    };
    if !changed {
        return;
    }
    let mut payload = Vec::with_capacity(1 + key.len());
    payload.push(frame_type);
    payload.extend_from_slice(key);
    broadcast_one(control::REMOVE, &payload);
}

pub fn publish_event(entity_id: u64, event_id: u32) {
    // ha_event_t: id u32, padding, source_entity u64, ha_value_t NONE.
    let mut payload = [0u8; 24];
    payload[0..4].copy_from_slice(&event_id.to_le_bytes());
    payload[8..16].copy_from_slice(&entity_id.to_le_bytes());
    broadcast_one(control::EVENT, &payload);
}

fn next_seq() -> u16 {
    SEQ.fetch_add(1, Ordering::Relaxed)
}

fn broadcast_one(frame_type: u8, payload: &[u8]) {
    let Some(handle) = *SERVER.lock().unwrap() else {
        return;
    };
    let mut frame = Vec::new();
    write_frame(&mut frame, next_seq(), frame_type, payload);
    let mut clients = CLIENTS.lock().unwrap();
    let mut slow = Vec::new();
    for client in clients.iter_mut() {
        if client.queue.len() >= CLIENT_QUEUE {
            slow.push(client.fd);
        } else {
            client.queue.push_back(frame.clone());
        }
    }
    if slow.is_empty() {
        return;
    }
    clients.retain(|client| !slow.contains(&client.fd));
    for fd in slow {
        log::warn!("ws: slow client {fd}, closing");
        unsafe { httpd_sess_trigger_close(handle as httpd_handle_t, fd) };
    }
}

/// Send queued frames from the Web task. The realtime path only enqueues, so a
/// slow client can never block Domain, Dispatcher or Zigbee.
pub fn flush() {
    let Some(handle) = *SERVER.lock().unwrap() else {
        return;
    };
    let mut clients = CLIENTS.lock().unwrap();
    let mut dead = Vec::new();
    for client in clients.iter_mut() {
        while let Some(bytes) = client.queue.pop_front() {
            let mut frame = ws_frame(&bytes);
            if unsafe { httpd_ws_send_frame_async(handle as httpd_handle_t, client.fd, &mut frame) }
                != ESP_OK
            {
                dead.push(client.fd);
                break;
            }
        }
    }
    clients.retain(|client| !dead.contains(&client.fd));
}

fn snapshot_frames() -> Vec<Vec<u8>> {
    let entries: Vec<(u8, Vec<u8>)> = {
        let model = MODEL.lock().unwrap();
        model
            .iter()
            .map(|entry| (entry.frame_type, entry.payload.clone()))
            .collect()
    };
    let count = entries.len() as u32;
    let mut frames = Vec::with_capacity(entries.len() + 2);
    let mut begin = Vec::new();
    write_frame(
        &mut begin,
        next_seq(),
        control::SYNC_BEGIN,
        &count.to_le_bytes(),
    );
    frames.push(begin);
    for (frame_type, payload) in entries {
        let mut frame = Vec::new();
        write_frame(&mut frame, next_seq(), frame_type, &payload);
        frames.push(frame);
    }
    let mut end = Vec::new();
    write_frame(
        &mut end,
        next_seq(),
        control::SYNC_END,
        &count.to_le_bytes(),
    );
    frames.push(end);
    frames
}

fn register(
    handle: httpd_handle_t,
    uri: &str,
    method: httpd_method_t,
    handler: unsafe extern "C" fn(*mut httpd_req_t) -> esp_err_t,
) -> bool {
    let cstr = std::ffi::CString::new(uri).unwrap();
    let desc = httpd_uri_t {
        uri: cstr.as_ptr(),
        method,
        handler: Some(handler),
        user_ctx: std::ptr::null_mut(),
        is_websocket: false,
        handle_ws_control_frames: false,
        supported_subprotocol: std::ptr::null(),
    };
    let err = unsafe { httpd_register_uri_handler(handle, &desc) };
    drop(cstr);
    err == ESP_OK
}

fn register_ws(
    handle: httpd_handle_t,
    uri: &str,
    handler: unsafe extern "C" fn(*mut httpd_req_t) -> esp_err_t,
) -> bool {
    let cstr = std::ffi::CString::new(uri).unwrap();
    let desc = httpd_uri_t {
        uri: cstr.as_ptr(),
        method: GET_METHOD,
        handler: Some(handler),
        user_ctx: std::ptr::null_mut(),
        is_websocket: true,
        handle_ws_control_frames: false,
        supported_subprotocol: std::ptr::null(),
    };
    let err = unsafe { httpd_register_uri_handler(handle, &desc) };
    drop(cstr);
    err == ESP_OK
}

unsafe extern "C" fn index_handler(req: *mut httpd_req_t) -> esp_err_t {
    serve(req, "text/html", INDEX)
}

unsafe extern "C" fn js_handler(req: *mut httpd_req_t) -> esp_err_t {
    serve(req, "text/javascript", APP_JS)
}

unsafe extern "C" fn css_handler(req: *mut httpd_req_t) -> esp_err_t {
    serve(req, "text/css", APP_CSS)
}

unsafe extern "C" fn model_handler(req: *mut httpd_req_t) -> esp_err_t {
    // No model file: the UI falls back to a full WS snapshot.
    httpd_resp_set_status(req, b"404 Not Found\0".as_ptr() as *const c_char);
    httpd_resp_send(req, b"\0".as_ptr() as *const c_char, 0);
    ESP_OK
}

unsafe extern "C" fn ws_handler(req: *mut httpd_req_t) -> esp_err_t {
    let mut buf = [0u8; 640];
    let mut frame: httpd_ws_frame_t = std::mem::zeroed();
    frame.payload = buf.as_mut_ptr();
    if httpd_ws_recv_frame(req, &mut frame, buf.len()) != ESP_OK {
        return ESP_OK;
    }
    if frame.type_ != httpd_ws_type_t_HTTPD_WS_TYPE_BINARY {
        return ESP_OK;
    }
    let len = frame.len.min(buf.len());
    if len < 9 || buf[1] != control::COMMAND {
        return ESP_OK;
    }
    match buf[8] {
        command::SNAPSHOT | command::SNAPSHOT_STATE => {
            let fd = httpd_req_to_sockfd(req);
            {
                let mut clients = CLIENTS.lock().unwrap();
                if !clients.iter().any(|client| client.fd == fd) {
                    if clients.len() < MAX_CLIENTS {
                        clients.push(Client {
                            fd,
                            queue: VecDeque::new(),
                        });
                    } else {
                        log::warn!("ws: client limit reached, {fd} ignored");
                    }
                }
            }
            for bytes in snapshot_frames() {
                let mut frame = ws_frame(&bytes);
                if httpd_ws_send_frame(req, &mut frame) != ESP_OK {
                    log::warn!("ws snapshot send failed");
                    break;
                }
            }
        }
        command::PROPERTY_COMMAND => property_command(&buf[9..len]),
        command::GROUP_PUT => handle_upsert(
            ha_web::entity::GROUP,
            ha_domain::canonical::GROUP_KEY_LEN,
            &buf[9..len],
        ),
        command::GROUP_REMOVE => handle_remove(
            ha_web::entity::GROUP,
            ha_domain::canonical::GROUP_KEY_LEN,
            &buf[9..len],
        ),
        command::LOCATION_PUT => handle_upsert(
            ha_web::entity::LOCATION,
            ha_domain::canonical::LOCATION_KEY_LEN,
            &buf[9..len],
        ),
        command::GROUP_ITEM_PUT => handle_upsert(
            ha_web::entity::GROUP_ITEM,
            ha_domain::canonical::GROUP_ITEM_KEY_LEN,
            &buf[9..len],
        ),
        command::GROUP_ITEM_REMOVE => handle_remove(
            ha_web::entity::GROUP_ITEM,
            ha_domain::canonical::GROUP_ITEM_KEY_LEN,
            &buf[9..len],
        ),
        command::GROUP_AUTOMATION_PUT => handle_upsert(
            ha_web::entity::GROUP_AUTOMATION,
            ha_domain::canonical::GROUP_ITEM_KEY_LEN,
            &buf[9..len],
        ),
        command::GROUP_AUTOMATION_REMOVE => handle_remove(
            ha_web::entity::GROUP_AUTOMATION,
            ha_domain::canonical::GROUP_ITEM_KEY_LEN,
            &buf[9..len],
        ),
        command::AUTOMATION_PUT => handle_upsert(
            ha_web::entity::AUTOMATION,
            ha_domain::canonical::GROUP_KEY_LEN,
            &buf[9..len],
        ),
        command::AUTOMATION_REMOVE => handle_remove(
            ha_web::entity::AUTOMATION,
            ha_domain::canonical::GROUP_KEY_LEN,
            &buf[9..len],
        ),
        command::ENTITY_RENAME => handle_entity_rename(&buf[9..len]),
        command::DEVICE_RENAME => handle_device_rename(&buf[9..len]),
        command::LOGICAL_DEVICE_REMOVE => handle_device_remove(&buf[9..len]),
        command::DISCOVERY => {
            if len >= 10 {
                let seconds = buf[9];
                let result = zb_poc_open_network(seconds);
                if result != 0 {
                    log::warn!("ws: open network failed ({result})");
                } else {
                    log::info!("ws: network open for {seconds}s");
                }
            }
        }
        _ => {}
    }
    ESP_OK
}

/// `ha_property_command_t` (32 bytes): entity u64@0, property i32@8, action u8@12,
/// kind i32@16, scalar/xy @20/@24.
fn property_command(payload: &[u8]) {
    let Some(command) = ha_web::parse_property_command(payload) else {
        return;
    };
    if let Err(error) = crate::intake::command(
        command.entity,
        command.property,
        u32::from(command.action),
        command.value,
    ) {
        log::warn!("command intake full, rejected: {error:?}");
    }
}

fn handle_upsert(entity: u8, key_len: usize, payload: &[u8]) {
    let Some((key, record)) = ha_web::parse_key_record(payload, key_len) else {
        return;
    };
    if let Err(error) = crate::intake::upsert(u32::from(entity), key, record) {
        log::warn!("upsert intake full, entity={entity}: {error:?}");
    }
}

fn handle_remove(entity: u8, key_len: usize, payload: &[u8]) {
    let Some(key) = ha_web::parse_key(payload, key_len) else {
        return;
    };
    if let Err(error) = crate::intake::remove(u32::from(entity), key) {
        log::warn!("remove intake full, entity={entity}: {error:?}");
    }
}

fn handle_entity_rename(payload: &[u8]) {
    let Some((entity, name)) = ha_web::parse_rename(payload) else {
        return;
    };
    if let Err(error) = crate::intake::rename_entity(entity, name) {
        log::warn!("rename entity intake full: {error:?}");
    }
}

fn handle_device_rename(payload: &[u8]) {
    let Some((device, name)) = ha_web::parse_rename(payload) else {
        return;
    };
    if let Err(error) = crate::intake::rename_device(device, name) {
        log::warn!("rename device intake full: {error:?}");
    }
}

fn handle_device_remove(payload: &[u8]) {
    let Some(device) = ha_web::parse_device_id(payload) else {
        return;
    };
    if let Err(error) = crate::intake::remove_device(device) {
        log::warn!("remove device intake full: {error:?}");
    }
}

fn ws_frame(bytes: &[u8]) -> httpd_ws_frame_t {
    let mut frame: httpd_ws_frame_t = unsafe { std::mem::zeroed() };
    frame.type_ = httpd_ws_type_t_HTTPD_WS_TYPE_BINARY;
    frame.payload = bytes.as_ptr() as *mut u8;
    frame.len = bytes.len();
    frame
}

unsafe fn serve(req: *mut httpd_req_t, content_type: &str, body: &'static [u8]) -> esp_err_t {
    let ct = std::ffi::CString::new(content_type).unwrap();
    httpd_resp_set_type(req, ct.as_ptr());
    httpd_resp_set_hdr(
        req,
        b"Content-Encoding\0".as_ptr() as *const c_char,
        b"gzip\0".as_ptr() as *const c_char,
    );
    httpd_resp_set_hdr(
        req,
        b"Cache-Control\0".as_ptr() as *const c_char,
        b"no-store\0".as_ptr() as *const c_char,
    );
    let err = httpd_resp_send(req, body.as_ptr() as *const c_char, body.len() as isize);
    drop(ct);
    err
}
