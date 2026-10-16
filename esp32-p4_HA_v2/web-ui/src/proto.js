// Кадры протокола v3 (docs/services/WEB_PROTOCOL.md). Little-endian, заголовок 8 байт.
//
// Web — тупой транспорт canonical-модели: кадры несут canonical structs verbatim
// (layout = ha_model, LE, fixed-width, reserved=0), а интерпретация живёт в браузере.
//   UPSERT: [type=ha_entity_t][key][record]
//   REMOVE: [type][key]
//   EVENT:  [WEB_MSG_EVENT][ha_event_t]
//   COMMAND:[WEB_MSG_COMMAND][cmd][args]   (args — canonical struct verbatim)
import { COMMAND_VALUE, VALUE_KIND } from './semantics.js'

export const MSG = {
  SYNC_BEGIN: 0x01,
  SYNC_END: 0x02,
  EVENT: 0xE0, // device → browser: payload = ha_event_t verbatim (вне диапазона ha_entity_t)
  COMMAND: 0x20,
}

// Тип сущности = числовое значение ha_entity_t (стабильный canonical ABI, не переиспользуется).
export const ENTITY = {
  AUTOMATION: 4,
  LOCATION: 6,
  GROUP: 7,
  GROUP_ITEM: 8,
  WEATHER: 9,
  WIFI_SCAN: 10,
  WIFI_KNOWN: 11,
  WIFI_STATUS: 12,
  SETTINGS: 13,
  ENTITY: 14,
  PROPERTY_STATE: 15,
  CAPABILITIES: 16,
  LOGICAL_DEVICE: 17,
}

export const CMD = {
  SNAPSHOT: 1,
  SNAPSHOT_STATE: 2,
  AUTOMATION_PUT: 4,
  AUTOMATION_REMOVE: 5,
  DISCOVERY: 8,
  GROUP_PUT: 9,
  GROUP_REMOVE: 10,
  LOCATION_PUT: 13,
  GROUP_ITEM_PUT: 16,
  GROUP_ITEM_REMOVE: 17,
  ENTITY_RENAME: 18,
  PROPERTY_COMMAND: 19,
  DEVICE_RENAME: 20,
  LOGICAL_DEVICE_REMOVE: 21,
}

export const HDR = 8
export const VER = 3

// Размеры canonical форм (ключ+запись, verbatim layout ha_model) для bulk-файлов.
// Зеркало canonical-форм (ключ+запись). Источник истины — ha_model; размеры там
// зафиксированы _Static_assert, поэтому правка структуры в C ломает сборку прошивки —
// при этом обнови и этот список (иначе страница молча читает не то).
export const SIZES = {
  17: { key: 8, rec: 80 },   // LOGICAL_DEVICE
  14: { key: 8, rec: 40 },   // ENTITY
  15: { key: 16, rec: 8 },   // PROPERTY_STATE
  16: { key: 8, rec: 216 },  // CAPABILITIES
  4: { key: 8, rec: 392 },   // AUTOMATION
  8: { key: 16, rec: 36 },   // GROUP_ITEM
  7: { key: 8, rec: 32 },    // GROUP
  6: { key: 8, rec: 60 },    // LOCATION
  9: { key: 8, rec: 16 },    // WEATHER
  12: { key: 8, rec: 36 },   // WIFI_STATUS
  13: { key: 1, rec: 8 },    // SETTINGS
}

// HTTP bulk-файл: 'HASM' (model) или 'HAS1' (полный) + ver u8 + flags u8 + reserved u16 +
// count u32, затем count × [type u8][key][record]. Возвращает [{ type, view }] или null.
export function parseSnapshot(buffer) {
  const dv = new DataView(buffer)
  if (dv.byteLength < 12) return null
  const magic = String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3))
  if ((magic !== 'HASM' && magic !== 'HAS1') || dv.getUint8(4) !== 1) return null
  const count = dv.getUint32(8, true)
  const out = []
  let off = 12
  for (let i = 0; i < count; i++) {
    if (off + 1 > dv.byteLength) return null
    const type = dv.getUint8(off)
    off += 1
    const s = SIZES[type]
    if (!s || off + s.key + s.rec > dv.byteLength) return null
    out.push({ type, view: new DataView(dv.buffer, dv.byteOffset + off, s.key + s.rec) })
    off += s.key + s.rec
  }
  return out
}

export function snapshotState() {
  return encodeCommand(CMD.SNAPSHOT_STATE)
}

export function decodeFrame(buf) {
  const dv = new DataView(buf)
  if (dv.byteLength < HDR || dv.getUint8(0) !== VER) return null
  const len = dv.getUint16(2, true)
  if (HDR + len > dv.byteLength) return null
  return { type: dv.getUint8(1), seq: dv.getUint16(4, true), payload: new DataView(buf, HDR, len) }
}

let seqCounter = 0

export function encodeCommand(cmd, args = new Uint8Array(0)) {
  const payload = new Uint8Array(1 + args.length)
  payload[0] = cmd
  payload.set(args, 1)
  return encodeFrame(MSG.COMMAND, payload)
}

export function encodeFrame(type, payload, seq) {
  const s = (seq === undefined ? ++seqCounter : seq) & 0xffff
  const out = new Uint8Array(HDR + payload.length)
  const dv = new DataView(out.buffer)
  dv.setUint8(0, VER)
  dv.setUint8(1, type)
  dv.setUint16(2, payload.length, true)
  dv.setUint16(4, s, true)
  out.set(payload, HDR)
  return { seq: s, bytes: out }
}

const enc = new TextEncoder()
function putText(out, off, text, max) {
  out.set(enc.encode(text || '').subarray(0, max - 1), off)
}

// HA_ENTITY_ID_SOURCE: «то же, что источник триггера».
export const ENTITY_SOURCE = 0xFFFFFFFFFFFFFFFFn

// --- canonical structs verbatim (layout = ha_model) --------------------------

// ha_property_command_t (32): entity u64@0, property i32@8, action u8@12, reserved[3],
// ha_command_value_t@16 {kind i32, scalar{kind i32@+4, value@+8} | xy{x@+4, y@+8}},
// reserved2[4]@28 (явный tail padding; wire = sizeof(struct)).
export function propertyCommand({ entity, property, action, value }) {
  const kind = value == null ? COMMAND_VALUE.NONE : value.xy ? COMMAND_VALUE.XY : COMMAND_VALUE.SCALAR
  const out = new Uint8Array(32)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, BigInt(entity), true)
  dv.setInt32(8, Number(property) || 0, true)
  dv.setUint8(12, Number(action) || 0)
  dv.setInt32(16, kind, true)
  if (kind === COMMAND_VALUE.SCALAR) {
    dv.setInt32(20, VALUE_KIND.FLOAT, true)
    dv.setFloat32(24, Number(value) || 0, true)
  } else if (kind === COMMAND_VALUE.XY) {
    dv.setFloat32(20, Number(value.xy.x) || 0, true)
    dv.setFloat32(24, Number(value.xy.y) || 0, true)
  }
  return encodeCommand(CMD.PROPERTY_COMMAND, out)
}

// ha_entity_rename_t (40): entity u64@0, name[32]@8.
export function entityRename(entityId, name) {
  const out = new Uint8Array(40)
  new DataView(out.buffer).setBigUint64(0, BigInt(entityId), true)
  putText(out, 8, name, 32)
  return encodeCommand(CMD.ENTITY_RENAME, out)
}

// DEVICE_RENAME: ha_device_id_t + char name[32].
export function deviceRename(deviceId, name) {
  const out = new Uint8Array(40)
  new DataView(out.buffer).setBigUint64(0, BigInt(deviceId), true)
  putText(out, 8, name, 32)
  return encodeCommand(CMD.DEVICE_RENAME, out)
}

// LOGICAL_DEVICE_REMOVE: ha_device_id_t.
export function deviceRemove(deviceId) {
  const out = new Uint8Array(8)
  new DataView(out.buffer).setBigUint64(0, BigInt(deviceId), true)
  return encodeCommand(CMD.LOGICAL_DEVICE_REMOVE, out)
}

export function snapshot() {
  return encodeCommand(CMD.SNAPSHOT)
}

// DISCOVERY args = u8 seconds (0 — стоп). Транспорт решает adapter.
export function startDiscovery(seconds = 180) {
  return encodeCommand(CMD.DISCOVERY, new Uint8Array([Math.max(0, Math.min(255, seconds))]))
}

// GROUP_PUT: ha_group_key_t + ha_group_record_t (8+32).
export function groupPut(id, title) {
  const out = new Uint8Array(40)
  new DataView(out.buffer).setBigUint64(0, BigInt(id), true)
  putText(out, 8, title, 32)
  return encodeCommand(CMD.GROUP_PUT, out)
}

// GROUP_REMOVE: ha_group_key_t (8).
export function groupRemove(id) {
  const out = new Uint8Array(8)
  new DataView(out.buffer).setBigUint64(0, BigInt(id), true)
  return encodeCommand(CMD.GROUP_REMOVE, out)
}

// GROUP_ITEM_PUT (52) = ha_group_item_key_t (16) + ha_group_item_record_t (36):
//   group_id u64@0, entity u64@8; order u16@16, reserved[2]@18, title[32]@20.
export function groupItemPut(groupId, item) {
  const out = new Uint8Array(52)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, BigInt(groupId), true)
  dv.setBigUint64(8, BigInt(item.entity), true)
  dv.setUint16(16, Number(item.order) || 0, true)
  putText(out, 20, item.title, 32)
  return encodeCommand(CMD.GROUP_ITEM_PUT, out)
}

// GROUP_ITEM_REMOVE: ha_group_item_key_t (16): group_id u64@0, entity u64@8.
export function groupItemRemove(groupId, entity) {
  const out = new Uint8Array(16)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, BigInt(groupId), true)
  dv.setBigUint64(8, BigInt(entity), true)
  return encodeCommand(CMD.GROUP_ITEM_REMOVE, out)
}

// ha_location_record_t (60): latitude f32@0, longitude f32@4, tz_offset_min i16@8,
// flags u8@10, reserved u8@11, name[48]@12.
export function encodeLocationRecord(r) {
  const out = new Uint8Array(60)
  const dv = new DataView(out.buffer)
  dv.setFloat32(0, Number(r.latitude) || 0, true)
  dv.setFloat32(4, Number(r.longitude) || 0, true)
  dv.setInt16(8, Math.round(Number(r.tzOffsetMin) || 0), true)
  let flags = 0
  if (r.tzAuto) flags |= 1
  if (r.posAuto) flags |= 2
  dv.setUint8(10, flags)
  putText(out, 12, r.name, 48)
  return out
}

// LOCATION_PUT: ha_device_id_t + ha_location_record_t (8+60).
export function locationPut(uid, r) {
  const out = new Uint8Array(68)
  new DataView(out.buffer).setBigUint64(0, BigInt(uid), true)
  out.set(encodeLocationRecord(r), 8)
  return encodeCommand(CMD.LOCATION_PUT, out)
}

// AUTOMATION_REMOVE: ha_automation_key_t (8).
export function automationRemove(id) {
  const out = new Uint8Array(8)
  new DataView(out.buffer).setBigUint64(0, BigInt(id), true)
  return encodeCommand(CMD.AUTOMATION_REMOVE, out)
}

// ha_rule_t (392) — плоский POD (offsetof-инварианты ha_rule.h):
//   triggers[4](32 each)@0   {entity u64, event_id i32@8, property i32@12, value f32@16,
//                             value2 f32@20, minutes_of_day u16@24, kind u8@26, op u8@27,
//                             edge u8@28, weekday_mask u8@29, reserved[2]@30}
//   actions[4](32 each)@128  {ref(16){entity u64, property i32@8, reserved u32@12},
//                             value(12)@16 {kind i32, scalar{i32 kind@+4, f32 value@+8} | xy{f32 x@+4,y@+8}},
//                             action u8@28, reserved[3]@29}
//   conditions[4](32 each)@256 {ref(16), cmp{op u8@16, reserved[3], value f32@20, value2 f32@24}, reserved u32@28}
//   enabled u8@384, trigger_count u8@385, action_count u8@386, conditions_count u8@387, reserved[4]@388
const RULE_TRIGGERS_OFF = 0
const RULE_ACTIONS_OFF = 128
const RULE_CONDITIONS_OFF = 256
const RULE_OFF_ENABLED = 384
const RULE_OFF_TRIGGER_COUNT = 385
const RULE_OFF_ACTION_COUNT = 386
const RULE_OFF_CONDITIONS_COUNT = 387
const RULE_TRIGGER_SIZE = 32
const RULE_ACTION_SIZE = 32
const CONDITION_SIZE = 32
const RULE_MAX_TRIGGERS = 4
const RULE_MAX_ACTIONS = 4
const RULE_MAX_CONDITIONS = 4
const RULE_SIZE = 392

function encodeTrigger(dv, o, t) {
  const kind = Number(t.kind) || 0
  dv.setBigUint64(o, kind === 0 || kind === 2 ? BigInt(t.entity ?? ENTITY_SOURCE) : 0n, true)
  dv.setInt32(o + 8, kind === 0 ? Number(t.eventId) || 0 : 0, true)
  dv.setInt32(o + 12, kind === 2 ? Number(t.property) || 0 : 0, true)
  dv.setFloat32(o + 16, kind === 2 ? Number(t.value) || 0 : 0, true)
  dv.setFloat32(o + 20, kind === 2 ? Number(t.value2) || 0 : 0, true)
  dv.setUint16(o + 24, kind === 1 ? Number(t.minutesOfDay) || 0 : 0, true)
  dv.setUint8(o + 26, kind)
  dv.setUint8(o + 27, kind === 2 ? Number(t.op) || 1 : 0)
  dv.setUint8(o + 28, kind === 2 ? Number(t.edge) || 0 : 0)
  dv.setUint8(o + 29, kind === 1 ? Number(t.weekdayMask) || 0 : 0)
}

function encodeAction(dv, o, a) {
  dv.setBigUint64(o, BigInt(a.entity ?? ENTITY_SOURCE), true)
  dv.setInt32(o + 8, Number(a.property) || 0, true)
  const v = o + 16
  const vk = a.valueKind ?? (a.value == null ? COMMAND_VALUE.NONE : a.x != null ? COMMAND_VALUE.XY : COMMAND_VALUE.SCALAR)
  dv.setInt32(v, vk, true)
  if (vk === COMMAND_VALUE.SCALAR) {
    dv.setInt32(v + 4, VALUE_KIND.FLOAT, true)
    dv.setFloat32(v + 8, Number(a.value) || 0, true)
  } else if (vk === COMMAND_VALUE.XY) {
    dv.setFloat32(v + 4, Number(a.x) || 0, true)
    dv.setFloat32(v + 8, Number(a.y) || 0, true)
  }
  dv.setUint8(o + 28, Number(a.action) || 0)
}

function encodeRule(dv, base, rule) {
  const triggers = (rule.triggers || []).slice(0, RULE_MAX_TRIGGERS)
  const actions = (rule.actions || []).slice(0, RULE_MAX_ACTIONS)
  const conds = (rule.conditions || []).slice(0, RULE_MAX_CONDITIONS)
  for (let i = 0; i < triggers.length; i++) {
    encodeTrigger(dv, base + RULE_TRIGGERS_OFF + i * RULE_TRIGGER_SIZE, triggers[i])
  }
  for (let i = 0; i < actions.length; i++) {
    encodeAction(dv, base + RULE_ACTIONS_OFF + i * RULE_ACTION_SIZE, actions[i])
  }
  for (let i = 0; i < conds.length; i++) {
    const p = base + RULE_CONDITIONS_OFF + i * CONDITION_SIZE
    const c = conds[i]
    dv.setBigUint64(p, BigInt(c.entity ?? ENTITY_SOURCE), true)
    dv.setInt32(p + 8, Number(c.property) || 0, true)
    dv.setUint8(p + 16, Number(c.op) || 1)
    dv.setFloat32(p + 20, Number(c.value) || 0, true)
    dv.setFloat32(p + 24, Number(c.value2) || 0, true)
  }
  dv.setUint8(base + RULE_OFF_ENABLED, rule.enabled ? 1 : 0)
  dv.setUint8(base + RULE_OFF_TRIGGER_COUNT, triggers.length)
  dv.setUint8(base + RULE_OFF_ACTION_COUNT, actions.length)
  dv.setUint8(base + RULE_OFF_CONDITIONS_COUNT, conds.length)
}

// AUTOMATION_PUT: ha_automation_key_t + ha_rule_t (8+388).
export function automationPut(id, rule) {
  const out = new Uint8Array(8 + RULE_SIZE)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, BigInt(id), true)
  encodeRule(dv, 8, rule)
  return encodeCommand(CMD.AUTOMATION_PUT, out)
}
