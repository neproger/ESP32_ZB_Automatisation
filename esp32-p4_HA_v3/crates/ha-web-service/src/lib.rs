//! HTTP + binary WebSocket for the UI.
//!
//! The wire is V2 canonical: 8-byte header, entity frames carry `key + record`
//! verbatim, `EVENT` carries `ha_event_t`. Snapshots are read directly from
//! Domain; `/model.bin` is the V2 static model blob built from the same SCHEMA.

use std::collections::VecDeque;
use std::ffi::c_char;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use esp_idf_svc::sys::*;
use ha_domain::canonical::{ActionId, CommandValue, EntityId, PropertyId};
use ha_domain::{DomainHandle, Fact, FactKind, Filter};

mod session;
mod snapshot;
use ha_web::{command, control, write_frame};
use session::{broadcast_one, close_client, enqueue_locked};
use snapshot::*;

const INDEX: &[u8] = include_bytes!("../../../web/ui/index.html.gz");
const APP_JS: &[u8] = include_bytes!("../../../web/ui/app.js.gz");
const APP_CSS: &[u8] = include_bytes!("../../../web/ui/app.css.gz");

const GET_METHOD: httpd_method_t = http_method_HTTP_GET as httpd_method_t;
const MAX_CLIENTS: usize = 6;
const CLIENT_QUEUE: usize = 64;
const CLIENT_BYTES: usize = 16 * 1024;

struct Client {
    fd: i32,
    queue: VecDeque<Vec<u8>>,
    bytes: usize,
    seq: u16,
}

static CLIENTS: Mutex<Vec<Client>> = Mutex::new(Vec::new());
static SERVER: Mutex<Option<usize>> = Mutex::new(None);
static MODEL_BLOB: Mutex<Option<Arc<Vec<u8>>>> = Mutex::new(None);
static MODEL_DIRTY: AtomicBool = AtomicBool::new(true);
static ACTIONS: Mutex<Option<&'static dyn Actions>> = Mutex::new(None);
static ACTION_TX: Mutex<Option<SyncSender<ActionRequest>>> = Mutex::new(None);
static SNAPSHOT_TX: Mutex<Option<SyncSender<SnapshotRequest>>> = Mutex::new(None);

enum ActionRequest {
    Command {
        entity: EntityId,
        property: PropertyId,
        action: ActionId,
        value: CommandValue,
    },
    Upsert {
        entity: u32,
        key: Vec<u8>,
        record: Vec<u8>,
    },
    Remove {
        entity: u32,
        key: Vec<u8>,
    },
    RenameEntity {
        entity: EntityId,
        name: String,
    },
    RenameDevice {
        device: u64,
        name: String,
    },
    RemoveDevice {
        device: u64,
    },
    OpenNetwork(u8),
}

struct SnapshotRequest {
    fd: i32,
    state_only: bool,
}

/// Application callbacks the transport delegates to. Web never mutates Domain
/// directly; the owner handles commands through its service queue.
pub trait Actions: Send + Sync {
    fn command(
        &self,
        entity: EntityId,
        property: PropertyId,
        action: ActionId,
        value: CommandValue,
    ) -> Result<(), ha_domain::Error>;
    fn upsert(&self, entity: u32, key: Vec<u8>, record: Vec<u8>) -> Result<(), ha_domain::Error>;
    fn remove(&self, entity: u32, key: Vec<u8>) -> Result<(), ha_domain::Error>;
    fn rename_entity(&self, entity: EntityId, name: String) -> Result<(), ha_domain::Error>;
    fn rename_device(&self, device: u64, name: String) -> Result<(), ha_domain::Error>;
    fn remove_device(&self, device: u64) -> Result<(), ha_domain::Error>;
    fn open_network(&self, seconds: u8) -> i32;
}

fn actions() -> Option<&'static dyn Actions> {
    *ACTIONS.lock().unwrap()
}

pub fn start(domain: DomainHandle, actions: &'static dyn Actions) -> bool {
    *ACTIONS.lock().unwrap() = Some(actions);
    match build_model_blob(&domain) {
        Ok(blob) => {
            *MODEL_BLOB.lock().unwrap() = Some(Arc::new(blob));
            MODEL_DIRTY.store(false, Ordering::Relaxed);
        }
        Err(()) => MODEL_DIRTY.store(true, Ordering::Relaxed),
    }
    let (action_tx, action_rx) = sync_channel(16);
    *ACTION_TX.lock().unwrap() = Some(action_tx);
    std::thread::Builder::new()
        .name("web-actions".into())
        .stack_size(16384)
        .spawn(move || action_worker(action_rx))
        .expect("web action worker");
    let mut config: httpd_config_t = unsafe { std::mem::zeroed() };
    config.server_port = 80;
    config.ctrl_port = 32768;
    // 3 internal httpd sockets + 6 clients; 3 of LWIP's 12 remain for DNS/weather.
    config.max_open_sockets = MAX_CLIENTS as u16 + 3;
    config.max_uri_handlers = 8;
    config.max_resp_headers = 8;
    config.backlog_conn = 5;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.stack_size = 16384;
    config.task_priority = 5;
    config.core_id = 0x7fff_ffff; // tskNO_AFFINITY
    // Keep the network task in internal RAM so it does not depend on PSRAM
    // while the flash cache is disabled.
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    let mut handle: httpd_handle_t = std::ptr::null_mut();
    let err = unsafe { httpd_start(&mut handle, &config) };
    if err != ESP_OK || handle.is_null() {
        log::error!("httpd_start failed: {err}");
        return false;
    }
    *SERVER.lock().unwrap() = Some(handle as usize);

    let filter = Filter {
        kind_mask: FactKind::EntityUpserted.bit()
            | FactKind::EntityRemoved.bit()
            | FactKind::Event.bit(),
        ..Filter::all()
    };
    let (tx, rx) = sync_channel(64);
    if let Err(error) = domain.subscribe_channel(filter, tx) {
        log::error!("web: subscribe failed: {error:?}");
    }
    let (snapshot_tx, snapshot_rx) = sync_channel(8);
    *SNAPSHOT_TX.lock().unwrap() = Some(snapshot_tx);
    std::thread::Builder::new()
        .name("web-client".into())
        .stack_size(16384)
        .spawn(move || web_client(domain, rx, snapshot_rx))
        .expect("web client thread");

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

fn action_worker(receiver: Receiver<ActionRequest>) {
    loop {
        let Ok(request) = receiver.recv() else {
            return;
        };
        let Some(actions) = actions() else {
            continue;
        };
        match request {
            ActionRequest::Command {
                entity,
                property,
                action,
                value,
            } => {
                if let Err(error) = actions.command(entity, property, action, value) {
                    log::warn!("web action command failed: {error:?}");
                }
            }
            ActionRequest::Upsert {
                entity,
                key,
                record,
            } => {
                if let Err(error) = actions.upsert(entity, key, record) {
                    log::warn!("web action upsert failed: {error:?}");
                }
            }
            ActionRequest::Remove { entity, key } => {
                if let Err(error) = actions.remove(entity, key) {
                    log::warn!("web action remove failed: {error:?}");
                }
            }
            ActionRequest::RenameEntity { entity, name } => {
                if let Err(error) = actions.rename_entity(entity, name) {
                    log::warn!("web action entity rename failed: {error:?}");
                }
            }
            ActionRequest::RenameDevice { device, name } => {
                if let Err(error) = actions.rename_device(device, name) {
                    log::warn!("web action device rename failed: {error:?}");
                }
            }
            ActionRequest::RemoveDevice { device } => {
                if let Err(error) = actions.remove_device(device) {
                    log::warn!("web action device remove failed: {error:?}");
                }
            }
            ActionRequest::OpenNetwork(seconds) => {
                let result = actions.open_network(seconds);
                if result != 0 {
                    log::warn!("web action open network failed ({result})");
                }
            }
        }
    }
}

fn enqueue_action(request: ActionRequest) {
    let guard = ACTION_TX.lock().unwrap();
    let Some(sender) = guard.as_ref() else {
        log::warn!("web action queue unavailable");
        return;
    };
    if sender.try_send(request).is_err() {
        log::warn!("web action queue full");
    }
}

fn request_snapshot(fd: i32, state_only: bool) -> bool {
    let guard = SNAPSHOT_TX.lock().unwrap();
    let Some(sender) = guard.as_ref() else {
        return false;
    };
    sender
        .try_send(SnapshotRequest { fd, state_only })
        .is_ok()
}

fn web_client(
    domain: DomainHandle,
    receiver: Receiver<Fact>,
    snapshots: Receiver<SnapshotRequest>,
) -> ! {
    loop {
        while let Ok(request) = snapshots.try_recv() {
            if !enqueue_snapshot(request.fd, &domain, request.state_only) {
                log::warn!("ws snapshot enqueue failed, closing {}", request.fd);
                close_client(request.fd);
            }
        }
        drain(&domain, &receiver);
        if MODEL_DIRTY.swap(false, Ordering::Relaxed) {
            match build_model_blob(&domain) {
                Ok(blob) => *MODEL_BLOB.lock().unwrap() = Some(Arc::new(blob)),
                Err(()) => MODEL_DIRTY.store(true, Ordering::Relaxed),
            }
        }
        session::flush();
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// Drain the Web inbox. Domain facts are copied into the existing bounded
/// model; WebSocket sends happen here, never under Domain lock.
fn drain(domain: &DomainHandle, receiver: &Receiver<Fact>) {
    while let Ok(fact) = receiver.try_recv() {
        match fact.kind {
            FactKind::EntityUpserted => {
                if !web_visible(fact.entity) {
                    continue;
                }
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
                if !web_visible(fact.entity) {
                    continue;
                }
                if let Ok(frame_type) = u8::try_from(fact.entity) {
                    remove(frame_type, fact.key());
                }
            }
            FactKind::Event => {
                if fact.payload_size != 24 {
                    continue;
                }
                let mut payload = [0u8; 24];
                if domain.payload_get(fact.payload_ref, &mut payload).is_err() {
                    continue;
                }
                broadcast_one(control::EVENT, &payload);
            }
            _ => {}
        }
    }
}

fn web_visible(entity: u32) -> bool {
    u8::try_from(entity).is_ok_and(ha_web::is_web_visible)
}

/// Broadcasts one canonical record. Domain already filters unchanged writes;
/// Web keeps no canonical record cache.
fn upsert(frame_type: u8, key: &[u8], record: &[u8]) {
    let Some(schema) = ha_web::schema(frame_type) else {
        return;
    };
    if key.len() != schema.key_size || record.len() != schema.record_size {
        log::warn!(
            "web: bad record type={frame_type} key={} record={}",
            key.len(),
            record.len()
        );
        return;
    }
    if schema.model {
        MODEL_DIRTY.store(true, Ordering::Relaxed);
    }
    let mut payload = Vec::with_capacity(key.len() + record.len());
    payload.extend_from_slice(key);
    payload.extend_from_slice(record);
    broadcast_one(frame_type, &payload);
}

/// Domain removal is authoritative; it does not depend on a Web cache entry.
fn remove(frame_type: u8, key: &[u8]) {
    let Some(schema) = ha_web::schema(frame_type) else {
        return;
    };
    if key.len() != schema.key_size {
        return;
    }
    if schema.model {
        MODEL_DIRTY.store(true, Ordering::Relaxed);
    }
    let mut payload = Vec::with_capacity(1 + key.len());
    payload.push(frame_type);
    payload.extend_from_slice(key);
    broadcast_one(control::REMOVE, &payload);
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
    let blob = MODEL_BLOB.lock().unwrap().clone();
    let Some(blob) = blob else {
        // Built off the HTTP task; retry after the web task rebuilds it.
        httpd_resp_send_err(req, 0, b"Service Unavailable\0".as_ptr() as *const c_char);
        return ESP_FAIL;
    };
    httpd_resp_set_type(req, b"application/octet-stream\0".as_ptr() as *const c_char);
    httpd_resp_set_hdr(
        req,
        b"Cache-Control\0".as_ptr() as *const c_char,
        b"no-store\0".as_ptr() as *const c_char,
    );
    httpd_resp_set_hdr(
        req,
        b"Connection\0".as_ptr() as *const c_char,
        b"close\0".as_ptr() as *const c_char,
    );
    send_body(req, &blob)
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
                            bytes: 0,
                            seq: 0,
                        });
                    } else {
                        log::warn!("ws: client limit reached, {fd} ignored");
                    }
                }
            }
            let state_only = buf[8] == command::SNAPSHOT_STATE;
            if !request_snapshot(fd, state_only) {
                log::warn!("ws snapshot queue full, closing {fd}");
                close_client(fd);
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
            if len == 10 {
                let seconds = buf[9];
                enqueue_action(ActionRequest::OpenNetwork(seconds));
                log::info!("ws: network open for {seconds}s queued");
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
    enqueue_action(ActionRequest::Command {
        entity: command.entity,
        property: command.property,
        action: u32::from(command.action),
        value: command.value,
    });
}

fn handle_upsert(entity: u8, key_len: usize, payload: &[u8]) {
    let Some((key, record)) = ha_web::parse_key_record(payload, key_len) else {
        return;
    };
    let Some(schema) = ha_web::schema(entity) else {
        return;
    };
    if record.len() != schema.record_size {
        return;
    }
    enqueue_action(ActionRequest::Upsert {
        entity: u32::from(entity),
        key,
        record,
    });
}

fn handle_remove(entity: u8, key_len: usize, payload: &[u8]) {
    let Some(key) = ha_web::parse_key(payload, key_len) else {
        return;
    };
    enqueue_action(ActionRequest::Remove {
        entity: u32::from(entity),
        key,
    });
}

fn handle_entity_rename(payload: &[u8]) {
    let Some((entity, name)) = ha_web::parse_rename(payload) else {
        return;
    };
    enqueue_action(ActionRequest::RenameEntity { entity, name });
}

fn handle_device_rename(payload: &[u8]) {
    let Some((device, name)) = ha_web::parse_rename(payload) else {
        return;
    };
    enqueue_action(ActionRequest::RenameDevice { device, name });
}

fn handle_device_remove(payload: &[u8]) {
    let Some(device) = ha_web::parse_device_id(payload) else {
        return;
    };
    enqueue_action(ActionRequest::RemoveDevice { device });
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
    httpd_resp_set_hdr(
        req,
        b"Connection\0".as_ptr() as *const c_char,
        b"close\0".as_ptr() as *const c_char,
    );
    let err = send_body(req, body);
    drop(ct);
    err
}

/// httpd_resp_send blocks when one call queues more than lwIP's 16 TCP segments.
/// Send bounded chunks so the stack drains between writes.
unsafe fn send_body(req: *mut httpd_req_t, body: &[u8]) -> esp_err_t {
    const CHUNK: usize = 4096;
    let mut offset = 0;
    while offset < body.len() {
        let end = (offset + CHUNK).min(body.len());
        let err = httpd_resp_send_chunk(
            req,
            body[offset..end].as_ptr() as *const c_char,
            (end - offset) as isize,
        );
        if err != ESP_OK {
            return err;
        }
        offset = end;
    }
    httpd_resp_send_chunk(req, std::ptr::null(), 0)
}
