# Stage 5 — Ownership Decision: Wi-Fi + System

> Статус: **approved; implementation/build/initial smoke passed; WEATHER_CHANGED и autoconnect passed; reconnect/backoff pending**.
> Scope: [STAGE5_SCOPE_AUDIT.md](../audits/STAGE5_SCOPE_AUDIT.md).

## 1. Decision

Stage 5 moves Wi-Fi and System service ownership into dedicated crates:

```text
ha-wifi    radio/service owner, WIFI_* publication, commands, reconnect
ha-system  SNTP/time/ticks/GeoIP/location/weather, SOURCE_SYSTEM publication
ha-domain  canonical state/facts/persistence
ha-app     composition root: dependencies, configuration, wiring, start order
```

`ha-app` keeps startup/configuration responsibility and critical start-error
handling only. It must not publish WIFI_*/system facts, generate system ticks,
run SNTP, compute GeoIP/location/weather, or own Wi-Fi/System business logic.

## 2. Responsibility split

| Component | Owns | Must not own |
|---|---|---|
| `ha-wifi` | scan, connect, known networks, status, autoconnect, reconnect/backoff, WIFI_* publication | system time/ticks/weather, Display/Web |
| `ha-system` | SNTP, time properties, MINUTE/HALF_HOUR/HOUR/DAY, WEATHER_CHANGED, GeoIP, location, weather, SOURCE_SYSTEM | radio calls, WIFI_* commands, Display/Web |
| `ha-domain` | canonical facts/state/persistence | Wi-Fi/System policy |
| `ha-app` | Domain creation/restore, platform dependencies, service config, startup order, start errors | service business logic/publication |

## 3. Contracts

### Wi-Fi

- `ha-wifi::start(domain)` registers Wi-Fi command executors, subscribes to
  `WIFI_KNOWN`, starts the service task and publishes canonical records.
- Radio calls remain behind the existing C bridge (`native_net/ha_wifi.c`).
  The bridge is a platform adapter; only `ha-wifi` calls it.
- WIFI_STATUS/WIFI_SCAN/WIFI_KNOWN remain Domain entities; sources are
  `SOURCE_WIFI`.
- Known AP persistence remains Domain (`RamFlash`), radio keeps the RAM list.
- Autoconnect, reconnect and backoff behavior are preserved from V2/current
  `native_net` implementation.

### System

- `ha-system::start(domain)` owns SNTP, time state publication, all tick
  events, GeoIP/location and weather.
- System gets network readiness through the Domain Wi-Fi contract
  (`WIFI_STATUS` update/event), not by calling `ha-wifi` APIs.
- System publishes with `SOURCE_SYSTEM`; it must not use the generic
  `config::put_record` path that currently stamps `SOURCE_ZIGBEE`.
- System state/events are published from its own task; `main` polling
  (`system::take`, time publishing, event generation) is removed.

### `main`

Allowed:

- create/restore Domain;
- create infrastructure dependencies;
- configure services;
- pass dependencies;
- start services in order;
- handle critical start failure.

Forbidden:

- publish WIFI_*/system properties;
- generate system ticks;
- execute SNTP;
- compute GeoIP/location/weather;
- contain Wi-Fi/System business logic.

## 4. Implementation order

1. Freeze contracts and ownership (this decision).
2. `ha-wifi`: known networks and status first, then scan/connect, then
   autoconnect/reconnect/backoff.
3. `ha-system`: SNTP and time properties, then ticks/events, then
   GeoIP/location/weather.
4. Remove duplicate System/Wi-Fi business publication from `main`.
5. Host tests: contracts, events, reconnect, time and SOURCE.
6. P4/C3 smoke.

## 5. Gates

- Stage 1 persistence and Stage 3 hardware acceptance remain independent gates;
  they are not part of Stage 5 completion.
- Display remains Stage 6; current Display already reads Wi-Fi from Domain.
- No canonical ABI, Domain persistence, Zigbee, Device or Automation changes.
- Do not add new abstractions/mechanisms absent from V2.