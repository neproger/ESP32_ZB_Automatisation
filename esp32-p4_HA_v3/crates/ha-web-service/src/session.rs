//! Per-client bounded TX queues and WebSocket flush.

use super::*;

pub(super) fn enqueue_locked(client: &mut Client, frame_type: u8, payload: &[u8]) -> bool {
    let mut frame = Vec::new();
    write_frame(&mut frame, client.seq, frame_type, payload);
    client.seq = client.seq.wrapping_add(1);
    if client.queue.len() >= CLIENT_QUEUE || client.bytes + frame.len() > CLIENT_BYTES {
        return false;
    }
    client.bytes += frame.len();
    client.queue.push_back(frame);
    true
}

pub(super) fn close_client(fd: i32) {
    let Some(handle) = *SERVER.lock().unwrap() else {
        return;
    };
    CLIENTS.lock().unwrap().retain(|client| client.fd != fd);
    log::warn!("ws: slow client {fd}, closing");
    unsafe { httpd_sess_trigger_close(handle as httpd_handle_t, fd) };
}

pub(super) fn broadcast_one(frame_type: u8, payload: &[u8]) {
    let mut slow = Vec::new();
    {
        let mut clients = CLIENTS.lock().unwrap();
        for client in clients.iter_mut() {
            if !enqueue_locked(client, frame_type, payload) {
                slow.push(client.fd);
            }
        }
        if !slow.is_empty() {
            clients.retain(|client| !slow.contains(&client.fd));
        }
    }
    for fd in slow {
        close_client(fd);
    }
}

/// Send queued frames from the Web task. Queue is touched under mutex; the
/// WebSocket send runs outside it.
pub(super) fn flush() {
    let Some(handle) = *SERVER.lock().unwrap() else {
        return;
    };
    let mut sends = Vec::new();
    {
        let mut clients = CLIENTS.lock().unwrap();
        for client in clients.iter_mut() {
            while let Some(bytes) = client.queue.pop_front() {
                client.bytes -= bytes.len();
                sends.push((client.fd, bytes));
            }
        }
    }
    let mut dead = Vec::new();
    for (fd, bytes) in sends {
        if dead.contains(&fd) {
            continue;
        }
        let mut frame = ws_frame(&bytes);
        if unsafe { httpd_ws_send_frame_async(handle as httpd_handle_t, fd, &mut frame) } != ESP_OK
        {
            dead.push(fd);
        }
    }
    if !dead.is_empty() {
        let mut clients = CLIENTS.lock().unwrap();
        clients.retain(|client| !dead.contains(&client.fd));
        drop(clients);
        for fd in dead {
            log::warn!("ws: send failed, closing {fd}");
            unsafe { httpd_sess_trigger_close(handle as httpd_handle_t, fd) };
        }
    }
}
