//! Attribute Report sink: C callback -> fixed slot -> z2m-rs.
//!
//! The C shim copies one report (all attributes of one ZCL frame, re-framed as
//! Report Attributes) into a bounded inbox slot and returns; the SDK buffer never
//! crosses the boundary. The main loop drains the inbox, parses borrowed views
//! and runs `z2m-rs` without allocation.

use std::sync::Mutex;

use zigbee_frames::ZclFrame;

const SLOTS: usize = 8;
const FRAME_CAPACITY: usize = 160;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct FrameMeta {
    pub src_ext: u64,
    pub src_short: u16,
    pub cluster: u16,
    pub manuf_code: u16,
    pub src_ep: u8,
    pub command: u8,
    pub tsn: u8,
    pub fc: u8,
    pub rssi: i8,
}

struct Inbox {
    slots: [Option<(FrameMeta, ZclFrame<FRAME_CAPACITY>)>; SLOTS],
    queue: [usize; SLOTS],
    head: usize,
    tail: usize,
    dropped: u32,
}

impl Inbox {
    const fn new() -> Self {
        Self {
            slots: [None; SLOTS],
            queue: [0; SLOTS],
            head: 0,
            tail: 0,
            dropped: 0,
        }
    }

    fn push(&mut self, meta: FrameMeta, data: &[u8]) {
        if self.tail - self.head >= SLOTS {
            self.dropped = self.dropped.saturating_add(1);
            return;
        }
        let Some(index) = self.slots.iter().position(Option::is_none) else {
            self.dropped = self.dropped.saturating_add(1);
            return;
        };
        let mut frame = ZclFrame::default();
        if frame
            .set(meta.cluster, meta.src_ep, meta.command, data)
            .is_err()
        {
            self.dropped = self.dropped.saturating_add(1);
            return;
        }
        self.slots[index] = Some((meta, frame));
        self.queue[self.tail % SLOTS] = index;
        self.tail += 1;
    }

    fn pop(&mut self) -> Option<(FrameMeta, ZclFrame<FRAME_CAPACITY>, u32)> {
        if self.head == self.tail {
            return None;
        }
        let index = self.queue[self.head % SLOTS];
        self.head += 1;
        self.slots[index]
            .take()
            .map(|(meta, frame)| (meta, frame, self.dropped))
    }
}

static INBOX: Mutex<Inbox> = Mutex::new(Inbox::new());
static LAST_DEVICE: Mutex<Option<(u64, u16)>> = Mutex::new(None);

pub fn take_last_device() -> Option<(u64, u16)> {
    LAST_DEVICE.lock().unwrap().take()
}

/// Called from the C shim inside the Zigbee callback. Copies, never decodes.
pub unsafe extern "C" fn ha_frame_received(meta: *const FrameMeta, data: *const u8, len: u16) {
    if meta.is_null() || (data.is_null() && len != 0) {
        return;
    }
    let meta = *meta;
    let data = std::slice::from_raw_parts(data, usize::from(len));
    INBOX.lock().unwrap().push(meta, data);
}

extern "C" {
    fn zb_poc_set_frame_sink(
        sink: Option<unsafe extern "C" fn(*const FrameMeta, *const u8, u16)>,
    );
}

pub fn install() {
    unsafe { zb_poc_set_frame_sink(Some(ha_frame_received)) };
    log::info!("attribute report sink installed");
}

/// Drains at most one frame; called from the main loop.
pub fn process_pending() {
    let item = INBOX.lock().unwrap().pop();
    let Some((meta, frame, dropped)) = item else {
        return;
    };

    if meta.src_ext != 0 {
        *LAST_DEVICE.lock().unwrap() = Some((meta.src_ext, meta.src_short));
    }

    match frame.parse() {
        Ok(view) => {
            let attributes = view.attributes().count();
            if meta.cluster == 0x0006 {
                for attr in view.attributes() {
                    log::debug!(
                        "attr 0x{:04x} type=0x{:02x} value={:02x?}",
                        attr.id, attr.zcl_type, attr.value
                    );
                }
            }
            let Some(profile) = crate::interview::selected_profile(meta.src_ext) else {
                log::info!(
                    "attr frame cluster=0x{:04x} attrs={} dropped={} (interview pending)",
                    meta.cluster,
                    attributes,
                    dropped
                );
                return;
            };
            let mut decoded = z2m_rs::Decoded::default();
            let standard = profile.id == "standard.zcl";
            let descriptor = if standard { crate::store::descriptor(meta.src_ext) } else { None };
            let standard_endpoint = descriptor.as_ref().and_then(|d| d.endpoint(meta.src_ep));
            let count = if standard {
                standard_endpoint
                    .map(|_| {
                        z2m_rs::decode_frame(
                            &zigbee_devices::standard::STANDARD_PROFILE,
                            &view,
                            &mut decoded,
                        )
                    })
                    .unwrap_or(0)
            } else {
                z2m_rs::decode_frame(profile, &view, &mut decoded)
            };
            log::info!(
                "attr frame eui64={:016x} ep={} cluster=0x{:04x} tsn={} rssi={} attrs={} decoded={} dropped={} profile={}",
                meta.src_ext,
                meta.src_ep,
                meta.cluster,
                meta.tsn,
                meta.rssi,
                attributes,
                count,
                dropped,
                profile.id
            );
            if standard {
                if let Some(endpoint) = standard_endpoint {
                    if endpoint.has_server(meta.cluster) {
                        for attr in view.attributes() {
                            if zigbee_devices::attribute_capability(meta.cluster, attr.id).is_some() {
                                crate::interview::confirm_attribute(
                                    meta.src_ext,
                                    meta.src_ep,
                                    meta.cluster,
                                    attr.id,
                                );
                            }
                        }
                    }
                }
            }
            let entity = zigbee_devices::zigbee_entity_id(meta.src_ext, meta.src_ep);
            if standard {
                if let Some(endpoint) = standard_endpoint {
                    let confirmed = crate::interview::confirmed(meta.src_ext);
                    crate::canonical::push_capabilities(
                        entity,
                        zigbee_devices::standard::capabilities_confirmed(endpoint, &confirmed),
                    );
                }
            }
            for update in decoded.updates() {
                if standard {
                    let Some(endpoint) = standard_endpoint else {
                        continue;
                    };
                    if !zigbee_devices::update_is_discovered(endpoint, update) {
                        continue;
                    }
                }
                log::info!("decoded {update:?}");
                crate::canonical::push(entity, update);
            }
        }
        Err(error) => log::warn!(
            "malformed attr frame eui64={:016x} cluster=0x{:04x}: {error:?}",
            meta.src_ext,
            meta.cluster
        ),
    }
}
