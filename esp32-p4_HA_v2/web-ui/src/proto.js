// Кадры протокола v2 (docs/services/WEB_PROTOCOL.md). Little-endian, заголовок 8 байт.
// Кадры протокола v2 (docs/services/WEB_PROTOCOL.md). Little-endian, заголовок 8 байт.
import { COMMAND_VALUE, VALUE_KIND } from './semantics.js'

export const MSG = {
  SYNC_BEGIN: 0x01,
  SYNC_END: 0x02,
  ENTITY: 0x10,
  ENTITY_REMOVE: 0x11,
  GROUP_ITEM: 0x19,
  GROUP_ITEM_REMOVE: 0x1A,
  COMMAND: 0x20,
}

export const CMD = {
  SNAPSHOT: 1,
  AUTOMATION_PUT: 4,
  AUTOMATION_REMOVE: 5,
  PERMIT_JOIN: 8,
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

// v3 canonical (Шаг A, A6.0): identity = entity_id, без uid/endpoint/cluster/attr.
// Кадр тот же, что у v2; v3 отличается набором типов. Живут рядом до A6.x.
export const MSG3 = {
  ENTITY: 0x30,
  ENTITY_REMOVE: 0x31,
  PROPERTY_STATE: 0x32,
  PROPERTY_STATE_REMOVE: 0x33,
  CAPABILITIES: 0x34,
  CAPABILITIES_REMOVE: 0x35,
  AUTOMATION: 0x36,
  AUTOMATION_REMOVE: 0x37,
  EVENT: 0x38,
  DEVICE: 0x39,
  DEVICE_REMOVE: 0x3A,
}

export const HDR = 8
export const VER = 2

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

export function encodeFrame(type, payload) {
  const seq = ++seqCounter & 0xffff
  const out = new Uint8Array(HDR + payload.length)
  const dv = new DataView(out.buffer)
  dv.setUint8(0, VER)
  dv.setUint8(1, type)
  dv.setUint16(2, payload.length, true)
  dv.setUint16(4, seq, true)
  out.set(payload, HDR)
  return { seq, bytes: out }
}

// Entity-native правило → wire делает фронтенд (зеркало web.c:web_rule_encode).
const bU16 = (v) => [v & 0xff, (v >> 8) & 0xff]
const bU32 = (v) => [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff]
const bU64 = (v) => { const o = []; let x = BigInt(v); for (let i = 0; i < 8; i++) { o.push(Number(x & 0xffn)); x >>= 8n } return o }
function bF32(v) {
  const dv = new DataView(new ArrayBuffer(4)); dv.setFloat32(0, Number(v) || 0, true)
  return [dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3)]
}
// Entity-native правило (Шаг A, A6.2): identity = entity_id, без uid/ep/cluster/attr.
export const ENTITY_SOURCE = 0xFFFFFFFFFFFFFFFFn // HA_ENTITY_ID_SOURCE

function encodeRule(r) {
  const b = []
  const t = r.trigger
  b.push(r.enabled ? 1 : 0, t.kind)
  if (t.kind === 0) b.push(...bU64(t.entity ?? ENTITY_SOURCE), t.eventId & 0xff)
  else if (t.kind === 1) b.push(...bU16(t.minutesOfDay || 0), (t.weekdayMask ?? 0) & 0xff)
  else b.push(...bU64(t.entity ?? ENTITY_SOURCE), ...bU16(t.property || 0), t.op || 1, t.edge || 0, ...bF32(t.value || 0), ...bF32(t.value2 || 0))
  const conds = (r.conditions || []).slice(0, 4)
  b.push(conds.length)
  for (const c of conds) b.push(...bU64(c.entity ?? ENTITY_SOURCE), ...bU16(c.property || 0), c.op || 1, ...bF32(c.value || 0), ...bF32(c.value2 || 0))
  const a = r.action
  b.push(...bU64(a.entity ?? ENTITY_SOURCE), ...bU16(a.property || 0), a.action || 0)
  const vk = a.valueKind ?? (a.value == null ? COMMAND_VALUE.NONE : a.x != null ? COMMAND_VALUE.XY : COMMAND_VALUE.SCALAR)
  b.push(vk)
  if (vk === COMMAND_VALUE.SCALAR) b.push(VALUE_KIND.FLOAT, ...bF32(a.value || 0))
  else if (vk === COMMAND_VALUE.XY) b.push(...bF32(a.x || 0), ...bF32(a.y || 0))
  return b
}
export function automationPut(id, rule) {
  return encodeCommand(CMD.AUTOMATION_PUT, new Uint8Array([...bU64(id), ...encodeRule(rule)]))
}

export function snapshot() {
  return encodeCommand(CMD.SNAPSHOT)
}

// PERMIT_JOIN args = u8 seconds (0 — закрыть сеть).
export function permitJoin(seconds = 180) {
  return encodeCommand(CMD.PERMIT_JOIN, new Uint8Array([Math.max(0, Math.min(255, seconds))]))
}

// AUTOMATION_REMOVE args = u64 id.
export function automationRemove(id) {
  const args = new Uint8Array(8)
  new DataView(args.buffer).setBigUint64(0, BigInt(id), true)
  return encodeCommand(CMD.AUTOMATION_REMOVE, args)
}

// DEVICE_REMOVE (v2, raw uid) больше не используется обычным UI: удаление — LOGICAL_DEVICE_REMOVE.

const enc = new TextEncoder()
function putText(out, off, text, max) {
  out.set(enc.encode(text || '').subarray(0, max - 1), off)
}

// GROUP_PUT args = u64 id | char title[32].
export function groupPut(id, title) {
  const out = new Uint8Array(8 + 32)
  new DataView(out.buffer).setBigUint64(0, BigInt(id), true)
  putText(out, 8, title, 32)
  return encodeCommand(CMD.GROUP_PUT, out)
}

// GROUP_REMOVE args = u64 id.
export function groupRemove(id) {
  const out = new Uint8Array(8)
  new DataView(out.buffer).setBigUint64(0, BigInt(id), true)
  return encodeCommand(CMD.GROUP_REMOVE, out)
}

// Canonical group item (A4): identity = (group_id, entity_id, property).
// PUT (52): group_id,u64 @0; entity,u64 @8; property,u16 @16; order,u16 @18; title[32] @20.
export function groupItemPut(groupId, item) {
  const out = new Uint8Array(52)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, BigInt(groupId), true)
  dv.setBigUint64(8, BigInt(item.entity), true)
  dv.setUint16(16, item.property, true)
  dv.setUint16(18, item.order || 0, true)
  putText(out, 20, item.title || '', 32)
  return encodeCommand(CMD.GROUP_ITEM_PUT, out)
}
// REMOVE (18): group_id,u64 @0; entity,u64 @8; property,u16 @16.
export function groupItemRemove(groupId, entity, property) {
  const out = new Uint8Array(18)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, BigInt(groupId), true)
  dv.setBigUint64(8, BigInt(entity), true)
  dv.setUint16(16, property, true)
  return encodeCommand(CMD.GROUP_ITEM_REMOVE, out)
}

// LOCATION_PUT args = u64 uid | ha_location_record_t (60).
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

export function locationPut(uid, r) {
  const out = new Uint8Array(8 + 60)
  new DataView(out.buffer).setBigUint64(0, BigInt(uid), true)
  out.set(encodeLocationRecord(r), 8)
  return encodeCommand(CMD.LOCATION_PUT, out)
}

// DEVICE_RENAME args = u64 device_id | char name[32] (Шаг B, B6). Имя принадлежит Logical Device.
export function deviceRename(deviceId, name) {
  const out = new Uint8Array(8 + 32)
  new DataView(out.buffer).setBigUint64(0, BigInt(deviceId), true)
  out.set(new TextEncoder().encode(name || '').subarray(0, 31), 8)
  return encodeCommand(CMD.DEVICE_RENAME, out)
}

// LOGICAL_DEVICE_REMOVE args = u64 device_id (Шаг B, B7). Adapter remove + cascade.
export function deviceRemove(deviceId) {
  const out = new Uint8Array(8)
  new DataView(out.buffer).setBigUint64(0, BigInt(deviceId), true)
  return encodeCommand(CMD.LOGICAL_DEVICE_REMOVE, out)
}

// ENTITY_RENAME args = u64 entity_id | char name[32] (Шаг A, A6.1c). Имя принадлежит Entity.
export function entityRename(entityId, name) {
  const out = new Uint8Array(8 + 32)
  new DataView(out.buffer).setBigUint64(0, BigInt(entityId), true)
  const bytes = new TextEncoder().encode(name || '').subarray(0, 31)
  out.set(bytes, 8)
  return encodeCommand(CMD.ENTITY_RENAME, out)
}

// PROPERTY_COMMAND (Шаг A, A6.1c): u64 entity @0, u16 property @8, u8 action @10, u8 kind @11;
// SCALAR @12 u8 scalar_kind + f32 @13; XY @12 f32 x + f32 y. Без uid/ep/cluster/attr.
export function propertyCommand({ entity, property, action, value }) {
  const kind = value == null ? COMMAND_VALUE.NONE : value.xy ? COMMAND_VALUE.XY : COMMAND_VALUE.SCALAR
  const size = 12 + (kind === COMMAND_VALUE.SCALAR ? 5 : kind === COMMAND_VALUE.XY ? 8 : 0)
  const out = new Uint8Array(size)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, BigInt(entity), true)
  dv.setUint16(8, property, true)
  dv.setUint8(10, action)
  dv.setUint8(11, kind)
  if (kind === COMMAND_VALUE.SCALAR) {
    dv.setUint8(12, VALUE_KIND.FLOAT)
    dv.setFloat32(13, Number(value), true)
  } else if (kind === COMMAND_VALUE.XY) {
    dv.setFloat32(12, Number(value.xy.x), true)
    dv.setFloat32(16, Number(value.xy.y), true)
  }
  return encodeCommand(CMD.PROPERTY_COMMAND, out)
}

// DEVICE_RENAME (v2) не используется обычным UI: rename идёт по entity_id (entityRename).
