//! P4-only lock probe. Build with `--features p4-lock-metrics`; the probe
//! performs a bounded Domain getter and reports the lock metrics measured in
//! `ha-domain` plus its own wall-clock latency.

use std::thread;
use std::time::Duration;

use esp_idf_svc::sys::esp_timer_get_time;
use ha_domain::DomainHandle;

const REPORT_INTERVAL_US: i64 = 5_000_000;

pub fn start(domain: DomainHandle) {
    ha_domain::p4_lock_metrics_reset();
    thread::Builder::new()
        .name("p4-lock-probe".into())
        .stack_size(4096)
        .spawn(move || {
            let mut last_report = now_us();
            let mut probe_max_us = 0i64;
            loop {
                let started = now_us();
                let result = domain.subscriber_drops();
                let elapsed = now_us().saturating_sub(started);
                probe_max_us = probe_max_us.max(elapsed);
                if let Err(error) = result {
                    log::warn!("p4-lock: probe failed: {error:?}");
                }
                if now_us().saturating_sub(last_report) >= REPORT_INTERVAL_US {
                    let metrics = ha_domain::p4_lock_metrics();
                    log::info!(
                        "p4-lock: probe_max_us={} locks={} wait_max_us={} wait_total_us={} hold_max_us={} hold_total_us={}",
                        probe_max_us,
                        metrics.locks,
                        metrics.wait_max_us,
                        metrics.wait_total_us,
                        metrics.hold_max_us,
                        metrics.hold_total_us
                    );
                    last_report = now_us();
                }
                thread::sleep(Duration::from_millis(1));
            }
        })
        .expect("p4 lock probe thread");
}

fn now_us() -> i64 {
    unsafe { esp_timer_get_time() }
}
