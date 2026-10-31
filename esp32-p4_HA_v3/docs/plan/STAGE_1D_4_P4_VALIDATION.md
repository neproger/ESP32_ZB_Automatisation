# Stage 1d.4 — P4 validation readiness

Status: instrumentation and protocol prepared; hardware execution pending.

## 1. Scope

This protocol validates the real ESP32-P4 runtime, not only host tests:

- Domain lock wait/hold while Display, Web/WS, Zigbee and Wi-Fi are active;
- slow WebSocket client and UI rebuild behavior;
- bounded-channel overflow and recovery;
- watchdog, stack and heap stability during a soak.

No production behavior changes when the probe feature is disabled.

## 2. Hardware prerequisites

- GUITION JC4880P443C_I_W, P4 rev < 3.0; `CONFIG_ESP32P4_REV_MIN_100` must
  match the board.
- PSRAM enabled (`CONFIG_SPIRAM=y`) and usable at 200 MHz.
- MIPI-DSI display and GT911 touch connected and initialized.
- C6 running standalone `ot_rcp` on the documented UART pins.
- C3 Wi-Fi CP running the ESP-Hosted build with the SPI configuration from
  `sdkconfig.defaults`.
- P4 programming port (`COM11` on the test board in `docs/build/BUILD.md`).

Record every firmware/config deviation before starting. A board revision or
C6/C3 image mismatch invalidates the run.

## 3. Build and instrumentation

Default build:

```powershell
cd esp32-p4_HA_v3
.\tools\build-ha-app.ps1
```

Instrumented build:

```powershell
.\tools\build-ha-app.ps1 -Features p4-lock-metrics
```

The feature enables:

- `ha-domain` counters for every `DomainHandle` lock acquisition: wait and
  hold totals/maxima, lock count;
- `p4-lock-probe` thread: a bounded getter every 1 ms, reporting cumulative
  `probe_max_us`, `wait_max_us`, `hold_max_us`, totals and lock count every
  5 seconds.

Metrics are reset when the probe starts. The probe itself is excluded from
normal firmware builds.

Expected log prefix:

```text
p4-lock: probe_max_us=... locks=... wait_max_us=... wait_total_us=...
         hold_max_us=... hold_total_us=...
```

## 4. Flash and boot checks

```powershell
espflash flash --port COM11 --baud 921600 `
  --partition-table crates/ha-app/partitions.csv `
  C:/t/hav3/riscv32imafc-esp-espidf/debug/ha-app --monitor
```

For the instrumented binary, run the same command after building with
`-Features p4-lock-metrics`.

Before load, verify:

- `ha-app ready`, `bootstrap done` (C6 RCP), web server and Wi-Fi IP;
- display and touch respond;
- no panic, brownout, watchdog reset or repeated reconnect;
- `p4-lock` reports appear every ~5 seconds with a plausible lock count.

Run an idle baseline for at least 60 seconds and record the metrics.

## 5. Runtime scenarios

### 5.1 Web/WS stress

1. Open four browser clients at `http://<p4-ip>/`.
2. Throttle or pause one client's network so its WS queue fills.
3. In the normal clients, trigger repeated property commands, group changes
   and state updates while the slow client remains stalled.
4. Verify the slow client is closed/dropped after its bounded queue fills,
   without blocking other clients.
5. Repeat with a normal snapshot reconnect.

Expected:

- `wait_max_us` and `probe_max_us` remain bounded; no long Domain hold;
- no watchdog/reset;
- fast clients continue to receive updates;
- `subscriber_drops` is explained by intentionally stalled consumers.

### 5.2 Display rebuild/UI stress

1. Navigate between groups and rebuild pages repeatedly.
2. Flood state updates for visible entities from Zigbee/Web/automation.
3. Exercise touch controls and switch-lock recovery.
4. Trigger a group/automation change while updates are arriving.

Expected:

- UI remains responsive; no `display: lock timeout` or watchdog;
- Domain metrics stay bounded while LVGL rebuild is running;
- state updates after rebuild are applied to the correct widgets.

### 5.3 Zigbee/Wi-Fi integration

- Interview a device, send property commands, observe reported state.
- Run Wi-Fi scan/connect and keep the web connection active.
- Trigger an automation/group command while Zigbee and Web traffic overlap.

Expected: no deadlock, no unexplained Domain metrics spike, command results
still reach Domain.

### 5.4 Soak

Run the combined workload for 30–60 minutes. Capture:

- `p4-lock` logs (all reports);
- WDT/reset messages;
- heap free/minimum and stack high-water marks (if enabled);
- `subscriber_drops`, web evictions/slow-client closes;
- final metrics after workload stops.

## 6. Provisional acceptance criteria

These are review thresholds, not host-test benchmark assertions. Adjust only
with a recorded hardware justification.

| Metric/behavior | Target | Hard failure |
|---|---|---|
| `hold_max_us` during normal/load | < 5,000 | > 50,000 |
| `wait_max_us` | < 20,000 | > 100,000 |
| `probe_max_us` | < 20,000 | > 100,000 |
| Watchdog/reset | none | any repeatable reset |
| Display lock timeout | none | repeatable timeout |
| Web slow client | isolated and closed | blocks other clients/Domain |
| Channel drops | only intentional stalled consumers | unexplained drops |
| 30–60 min soak | stable | panic/deadlock/heap exhaustion |

If a threshold fails, capture the complete log and the workload that produced
it; do not loosen the criterion without review.

## 7. Display/Web runtime harness

A host harness cannot exercise the real LVGL/httpd components without turning
Display/Web into a separately testable projection layer. That refactor is not
part of Stage 1d.4. The real-component harness is therefore the P4 run above:
the browser clients generate the Web/WS load, the display/touch generates UI
load, and `p4-lock` measures Domain lock behavior while those components run.

The existing host harness remains the pre-hardware check for Domain
backpressure; it does not replace this P4 run.

## 8. Deliverables from the hardware run

- Build mode and feature flags, binary hash, source commit.
- Board revision, C6 RCP image, C3 CP image, display/touch status.
- Full boot and `p4-lock` logs.
- Workload script/commands and duration.
- Metrics table for idle, each scenario and soak.
- Pass/fail per criterion, with deviations and follow-up defects.
