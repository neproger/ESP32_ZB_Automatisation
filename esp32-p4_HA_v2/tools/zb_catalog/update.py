#!/usr/bin/env python3
"""Обновление каталога Zigbee-устройств из zigbee-herdsman-converters.

Пайплайн (host, не в прошивке):
  1. ставит закреплённую версию npm-пакета в .cache/ (если ещё нет);
  2. Node дампит fingerprint + exposes в TSV (JSON не используется);
  3. скрипт маппит exposes в canonical id ha_model и генерит C-таблицу
     zigbee/src/generated/zb_catalog_generated.{c,h}.

Сгенерированные файлы коммитятся: обычная сборка прошивки без сети.
Запуск: python tools/zb_catalog/update.py [--force]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

VERSION = "26.123.0"  # пин: обновление каталога = осознанная смена версии
HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent.parent
CACHE = HERE / ".cache"
GENERATED_SRC = PROJECT / "zigbee" / "src" / "generated"
GENERATED_INC = PROJECT / "zigbee" / "include" / "zigbee" / "generated"

# Фильтр вендоров — только в генераторе (runtime не знает про фильтры).
MANUFACTURER_PREFIXES = ("_TZ", "_TZE", "LUMI", "lumi.", "Aqara", "eWeLink", "SONOFF")

# Модели, у которых профиль задан вручную в zigbee_profiles.c (генератор их пропускает).
MANUAL_MODELS = {"TS0041"}

# exposes → canonical property (ha_model/ha_properties.h).
PROPERTY_MAP = {
    ("switch", "state"): "POWER",
    ("light", "state"): "POWER",
    ("light", "brightness"): "BRIGHTNESS",
    ("light", "color_temp"): "COLOR_TEMPERATURE",
    ("light", "color"): "COLOR",
    ("light", "color_xy"): "COLOR",
    ("light", "color_hs"): "COLOR",
    ("numeric", "brightness"): "BRIGHTNESS",
    ("numeric", "temperature"): "TEMPERATURE",
    ("numeric", "humidity"): "HUMIDITY",
    ("numeric", "battery"): "BATTERY_PERCENT",
    ("numeric", "voltage"): "BATTERY_VOLTAGE",
    ("numeric", "illuminance"): "ILLUMINANCE",
    ("numeric", "illuminance_lux"): "ILLUMINANCE",
    ("binary", "occupancy"): "OCCUPANCY",
}

EVENT_MAP = {
    "single": "SINGLE_PRESS",
    "double": "DOUBLE_PRESS",
    "hold": "HOLD",
}

# Поля, которые не влияют на поддержку (диагностика/сервис/не наши свойства).
IGNORED_NAMES = {
    "linkquality",
    "last_seen",
    "update",
    "update_available",
    "battery_low",
    "action_group",
    "action_scene",
    "action_transition_time",
    "action_rate",
    "action_step_size",
    "action_direction",
    "action_time",
    "action_level",
    "identify",
    "effect",
    "power_on_behavior",
    "switch_type",
    "indicator_mode",
    "trigger_indicator",
    "temperature_calibration",
    "humidity_calibration",
    "pressure",
    "energy",
    "current",
    "power",
    "ac_frequency",
    "illuminance_lux",
}


class Unsupported(Exception):
    """Fingerprint есть, но поведение/свойства не покрыты нашим handler'ом."""


def npm_install() -> Path:
    """Поставить пакет с зависимостями в .cache; вернуть путь к dist/devices."""
    target = CACHE / f"zhc-{VERSION}"
    devices = target / "node_modules" / "zigbee-herdsman-converters" / "dist" / "devices"
    if devices.is_dir():
        return devices
    CACHE.mkdir(parents=True, exist_ok=True)
    target.mkdir(parents=True, exist_ok=True)
    print(f"[catalog] npm install zigbee-herdsman-converters@{VERSION}")
    subprocess.run(
        ["npm", "install", "--no-save", "--no-audit", "--no-fund",
         f"zigbee-herdsman-converters@{VERSION}"],
        cwd=target,
        check=True,
        shell=(os.name == "nt"),
    )
    return devices


def dump(devices: Path) -> list[dict]:
    print("[catalog] dump fingerprints/exposes (TSV)")
    result = subprocess.run(
        ["node", str(HERE / "dump_z2m.mjs"), str(devices)],
        check=True,
        capture_output=True,
        text=True,
    )
    rows = []
    for line in result.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) != 6:
            continue
        manufacturer, model, vendor, signature, flags, description = parts
        rows.append(
            {
                "manufacturer": manufacturer,
                "model": model,
                "vendor": vendor,
                "signature": signature,
                "flags": flags,
                "description": description,
            }
        )
    return rows


def included(manufacturer: str) -> bool:
    return any(manufacturer.startswith(p) for p in MANUFACTURER_PREFIXES)


def parse_signature(signature: str) -> tuple[list[str], list[str], bool, bool]:
    """→ (properties, events, unsupported, has_action)."""
    properties: list[str] = []
    events: list[str] = []
    unsupported = False
    has_action = False
    if not signature:
        return properties, events, unsupported, has_action
    for token in signature.split(";"):
        fields = token.split("|")
        if len(fields) < 3:
            continue
        kind, name, prop, _unit = fields[0], fields[1], fields[2], fields[3] if len(fields) > 3 else ""
        values = fields[4].split("/") if len(fields) > 4 and fields[4] else []
        if kind == "enum" and name in ("action", "click"):
            has_action = True
            mapped = [EVENT_MAP[v] for v in values if v in EVENT_MAP]
            if any(v not in EVENT_MAP for v in values):
                unsupported = True  # triple/release/... у нас нет
            for event in mapped:
                if event not in events:
                    events.append(event)
            continue
        key = (kind, name)
        prop_name = PROPERTY_MAP.get(key)
        if prop_name is not None:
            if prop_name not in properties:
                properties.append(prop_name)
            continue
        if name in IGNORED_NAMES or (prop and prop in IGNORED_NAMES):
            continue
        if kind in ("switch", "light", "numeric", "binary", "enum", "text", "composite"):
            unsupported = True
    return properties, events, unsupported, has_action


def c_string(text: str) -> str:
    """Строка для C-литерала: без управляющих символов, с экранированием."""
    clean = "".join(ch for ch in text if ch >= " " and ch != "\x7f")
    return clean.replace("\\", "\\\\").replace('"', '\\"')


def emit(entries: list[dict], src_dir: Path, inc_dir: Path) -> None:
    src_dir.mkdir(parents=True, exist_ok=True)
    inc_dir.mkdir(parents=True, exist_ok=True)
    c_path = src_dir / "zb_catalog_generated.c"
    h_path = inc_dir / "zb_catalog_generated.h"

    caps: dict[tuple, int] = {}
    proto_tuya_dp = False
    for entry in entries:
        key = (tuple(entry["properties"]), tuple(entry["events"]))
        caps.setdefault(key, len(caps))
        if entry["tuya_dp"]:
            proto_tuya_dp = True

    lines: list[str] = []
    lines.append("/* Сгенерировано tools/zb_catalog/update.py — не править вручную. */")
    lines.append(f"/* источник: zigbee-herdsman-converters@{VERSION} */")
    lines.append('#include "ha_model/ha_properties.h"')
    lines.append('#include "zigbee/generated/zb_catalog_generated.h"')
    lines.append("")
    lines.append("static const zb_protocol_binding_t k_proto_tuya_dp = {")
    lines.append("    .kind = ZB_PROTOCOL_TUYA_DP,")
    lines.append("    .button_endpoint = 0,")
    lines.append("};")
    lines.append("")

    for key, idx in caps.items():
        properties, events = key
        if properties:
            lines.append(f"static const uint8_t k_props_{idx}[] = {{")
            lines.append("    " + ", ".join(f"HA_PROPERTY_{p}" for p in properties) + ",")
            lines.append("};")
        if events:
            lines.append(f"static const uint8_t k_events_{idx}[] = {{")
            lines.append("    " + ", ".join(f"HA_EVENT_{e}" for e in events) + ",")
            lines.append("};")
        lines.append(f"static const zb_capability_profile_t k_cap_{idx} = {{")
        if properties:
            lines.append(f"    .properties = k_props_{idx},")
            lines.append(f"    .property_count = {len(properties)},")
        if events:
            lines.append(f"    .events = k_events_{idx},")
            lines.append(f"    .event_count = {len(events)},")
        lines.append("    .button_endpoint = 0,")
        lines.append("    .drop_power = false,")
        lines.append("};")
        lines.append("")

    lines.append("const zb_device_profile_t zb_catalog_generated[] = {")
    for entry in entries:
        key = (tuple(entry["properties"]), tuple(entry["events"]))
        idx = caps[key]
        protocol = "&k_proto_tuya_dp" if entry["tuya_dp"] else "NULL"
        if entry["unsupported"] and not entry["tuya_dp"]:
            protocol = "&k_proto_tuya_dp"  # нет handler'а: recognized, not supported
        lines.append(
            f'    {{.manufacturer = "{c_string(entry["manufacturer"])}", '
            f'.model = "{c_string(entry["model"])}", '
            f".capability = &k_cap_{idx}, .protocol = {protocol}}},"
        )
    lines.append("};")
    lines.append("")
    lines.append("const size_t zb_catalog_generated_count = "
                 "sizeof(zb_catalog_generated) / sizeof(zb_catalog_generated[0]);")
    lines.append("")
    c_path.write_text("\n".join(lines), encoding="utf-8")

    h_path.write_text(
        "/* Сгенерировано tools/zb_catalog/update.py — не править вручную. */\n"
        "#pragma once\n\n"
        "#include <stddef.h>\n\n"
        '#include "zigbee/zigbee_profiles.h"\n\n'
        "extern const zb_device_profile_t zb_catalog_generated[];\n"
        "extern const size_t zb_catalog_generated_count;\n",
        encoding="utf-8",
    )

    print(f"[catalog] entries={len(entries)} capability_profiles={len(caps)} "
          f"tuya_dp={'yes' if proto_tuya_dp else 'no'}")
    print(f"[catalog] wrote {c_path.relative_to(PROJECT)} и .h")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--force", action="store_true", help="перегенерировать даже если файл есть")
    args = parser.parse_args()

    if not shutil.which("node") or not shutil.which("npm"):
        print("[catalog] нужен Node.js/npm (он уже используется для web-ui)", file=sys.stderr)
        return 1

    devices = npm_install() if args.force else npm_install()
    rows = dump(devices)

    seen: set[tuple[str, str]] = set()
    entries: list[dict] = []
    for row in rows:
        manufacturer = row["manufacturer"].strip()
        model = row["model"].strip()
        if not included(manufacturer) or not model or model in MANUAL_MODELS:
            continue
        if (manufacturer, model) in seen:
            continue
        seen.add((manufacturer, model))
        properties, events, unsupported, _ = parse_signature(row["signature"])
        if not properties and not events:
            unsupported = True  # нечего показать приложению
        entries.append(
            {
                "manufacturer": manufacturer,
                "model": model,
                "properties": properties,
                "events": events,
                "unsupported": unsupported,
                "tuya_dp": row["flags"] == "tuya_dp",
                "vendor": row["vendor"],
                "description": row["description"],
            }
        )

    entries.sort(key=lambda e: (e["manufacturer"], e["model"]))
    if not entries:
        print("[catalog] пусто: проверь фильтр/версию", file=sys.stderr)
        return 1
    emit(entries, GENERATED_SRC, GENERATED_INC)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
