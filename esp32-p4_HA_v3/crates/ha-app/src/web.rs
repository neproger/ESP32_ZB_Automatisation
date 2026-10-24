//! HTTP + binary WebSocket for the copied V2 UI.
//!
//! The wire is V2 canonical: 8-byte header, entity frames carry `key + record`
//! verbatim, `EVENT` carries `ha_event_t`. The UI fetches `/model.bin`; a 404 makes
//! it fall back to a full WS snapshot, so no model file is generated yet.
//!
//! Stub projection: one logical device + one entity + semantic press events. The
//! rest of the V2 model (properties/capabilities/groups/automations) is absent.

use std::ffi::c_char;
use std::sync::Mutex;

use esp_idf_svc::sys::*;
use ha_web::{control, entity, write_frame, HEADER_LEN};

const INDEX: &[u8] = include_bytes!("../../../web/ui/index.html.gz");
const APP_JS: &[u8] = include_bytes!("../../../web/ui/app.js.gz");
const APP_CSS: &[u8] = include_bytes!("../../../web/ui/app.css.gz");

const ENTITY_ID: u64 = 0x0100_0000_0000_0001;
const DEVICE_NAME: &str = "TS0041 button";
const DEVICE_MODEL: &str = "TS0041";
const GET_METHOD: httpd_method_t = http_method_HTTP_GET as httpd_method_t;

#[derive(Clone, Copy, Default)]
struct DeviceView {
    eui64: u64,
    action: u8,
    count: u16,
}

static VIEW: Mutex<DeviceView> = Mutex::new(DeviceView {
    eui64: 0,
    action: 0,
    count: 0,
});
static CLIENTS: Mutex<Vec<i32>> = Mutex::new(Vec::new());
static SERVER: Mutex<Option<usize>> = Mutex::new(None);

pub fn start() -> bool {
    let mut config: httpd_config_t = unsafe { std::mem::zeroed() };
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 4;
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

pub fn publish(eui64: u64, action: u8, count: u16) {
    {
        let mut view = VIEW.lock().unwrap();
        *view = DeviceView {
            eui64,
            action,
            count,
        };
    }
    let handle = match *SERVER.lock().unwrap() {
        Some(handle) => handle,
        None => return,
    };
    let mut seq = 0u16;
    let view = DeviceView {
        eui64,
        action,
        count,
    };
    // Keep the entity known to clients opened before the first press, then the event.
    let mut frames = snapshot_frames(&view, &mut seq);
    frames.retain(|frame| {
        frame.len() > HEADER_LEN && frame[1] != control::SYNC_BEGIN && frame[1] != control::SYNC_END
    });
    frames.push(event_frame(&view, &mut seq));

    let mut clients = CLIENTS.lock().unwrap();
    for bytes in frames {
        let mut frame = ws_frame(&bytes);
        clients.retain(|fd| unsafe {
            httpd_ws_send_frame_async(handle as httpd_handle_t, *fd, &mut frame) == ESP_OK
        });
        if clients.is_empty() {
            break;
        }
    }
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
    // No model file yet: the UI falls back to a full WS snapshot.
    httpd_resp_set_status(req, b"404 Not Found\0".as_ptr() as *const c_char);
    httpd_resp_send(req, b"\0".as_ptr() as *const c_char, 0);
    ESP_OK
}

unsafe extern "C" fn ws_handler(req: *mut httpd_req_t) -> esp_err_t {
    let mut buf = [0u8; 128];
    let mut frame: httpd_ws_frame_t = std::mem::zeroed();
    frame.payload = buf.as_mut_ptr();
    if httpd_ws_recv_frame(req, &mut frame, buf.len()) != ESP_OK {
        return ESP_OK;
    }
    if frame.type_ != httpd_ws_type_t_HTTPD_WS_TYPE_BINARY {
        return ESP_OK;
    }
    let len = frame.len.min(buf.len());
    // COMMAND frame: header[0..8], payload[0] = command id.
    if len >= 9 && buf[1] == control::COMMAND && buf[8] == command_snapshot() {
        let fd = httpd_req_to_sockfd(req);
        {
            let mut clients = CLIENTS.lock().unwrap();
            if !clients.contains(&fd) {
                clients.push(fd);
            }
        }
        let view = *VIEW.lock().unwrap();
        let mut seq = 0u16;
        for bytes in snapshot_frames(&view, &mut seq) {
            let mut frame = ws_frame(&bytes);
            if httpd_ws_send_frame(req, &mut frame) != ESP_OK {
                log::warn!("ws snapshot send failed");
                break;
            }
        }
    }
    ESP_OK
}

const fn command_snapshot() -> u8 {
    ha_web::command::SNAPSHOT
}

fn ws_frame(bytes: &[u8]) -> httpd_ws_frame_t {
    let mut frame: httpd_ws_frame_t = unsafe { std::mem::zeroed() };
    frame.type_ = httpd_ws_type_t_HTTPD_WS_TYPE_BINARY;
    frame.payload = bytes.as_ptr() as *mut u8;
    frame.len = bytes.len();
    frame
}

fn snapshot_frames(view: &DeviceView, seq: &mut u16) -> Vec<Vec<u8>> {
    let count = u32::from(view.eui64 != 0) * 2;
    let mut frames = Vec::with_capacity(4);
    let mut begin = Vec::new();
    write_frame(&mut begin, *seq, control::SYNC_BEGIN, &count.to_le_bytes());
    *seq = seq.wrapping_add(1);
    frames.push(begin);
    if view.eui64 != 0 {
        let mut payload = Vec::with_capacity(88);
        payload.extend_from_slice(&ha_web::entity_key(view.eui64));
        payload.extend_from_slice(&ha_web::logical_device(
            0,
            1, // Zigbee adapter source
            DEVICE_MODEL,
            DEVICE_NAME,
        ));
        let mut frame = Vec::new();
        write_frame(&mut frame, *seq, entity::LOGICAL_DEVICE, &payload);
        *seq = seq.wrapping_add(1);
        frames.push(frame);

        let mut payload = Vec::with_capacity(48);
        payload.extend_from_slice(&ha_web::entity_key(ENTITY_ID));
        payload.extend_from_slice(&ha_web::entity(view.eui64, "Button"));
        let mut frame = Vec::new();
        write_frame(&mut frame, *seq, entity::ENTITY, &payload);
        *seq = seq.wrapping_add(1);
        frames.push(frame);
    }
    let mut end = Vec::new();
    write_frame(&mut end, *seq, control::SYNC_END, &count.to_le_bytes());
    frames.push(end);
    frames
}

fn event_frame(view: &DeviceView, seq: &mut u16) -> Vec<u8> {
    // ha_event_t: id i32, padding, source_entity u64, ha_value_t (kind + union).
    let mut payload = [0u8; 24];
    let id = u32::from(view.action) + 1; // 1 single, 2 double, 3 hold
    payload[0..4].copy_from_slice(&id.to_le_bytes());
    payload[8..16].copy_from_slice(&ENTITY_ID.to_le_bytes());
    payload[16..20].copy_from_slice(&0i32.to_le_bytes()); // HA_VALUE_NONE
    let mut frame = Vec::new();
    write_frame(&mut frame, *seq, control::EVENT, &payload);
    *seq = seq.wrapping_add(1);
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

