// Кадры протокола v2 (docs/services/WEB_PROTOCOL.md). Little-endian, заголовок 8 байт.
import { encodeAutomationRecord, SCHEMA, ENTITY } from './schema.js'

export const MSG = {
  SYNC_BEGIN: 0x01,
  SYNC_END: 0x02,
  ENTITY: 0x10,
  ENTITY_REMOVE: 0x11,
  COMMAND: 0x20,
}

export const CMD = {
  SNAPSHOT: 1,
  ZB_COMMAND: 2,
  DEVICE_RENAME: 3,
  AUTOMATION_PUT: 4,
  AUTOMATION_REMOVE: 5,
  DEVICE_REMOVE: 6,
  DEVICE_REMOVE_CANCEL: 7,
  PERMIT_JOIN: 8,
  GROUP_PUT: 9,
  GROUP_REMOVE: 10,
  GROUP_ITEM_PUT: 11,
  GROUP_ITEM_REMOVE: 12,
  LOCATION_PUT: 13,
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

// ZB_COMMAND args = ha_zb_command_t (layout с выравниванием, 32 байта):
// u64 uid @0, u8 ep @8, u16 cluster @10, u8 cmd @12, u8 args_len @13, u8 args[16] @14.
export function zbCommand({ uid, ep, cluster, command, args = [] }) {
  const out = new Uint8Array(32)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, uid, true)
  dv.setUint8(8, ep)
  dv.setUint16(10, cluster, true)
  dv.setUint8(12, command)
  dv.setUint8(13, Math.min(args.length, 16))
  out.set(args.slice(0, 16), 14)
  return encodeCommand(CMD.ZB_COMMAND, out)
}

export function snapshot() {
  return encodeCommand(CMD.SNAPSHOT)
}

// PERMIT_JOIN args = u8 seconds (0 — закрыть сеть).
export function permitJoin(seconds = 180) {
  return encodeCommand(CMD.PERMIT_JOIN, new Uint8Array([Math.max(0, Math.min(255, seconds))]))
}

// AUTOMATION_PUT args = u64 id | ha_automation_record_t (SCHEMA.recSize).
export function automationPut(id, rule) {
  const args = new Uint8Array(8 + SCHEMA[ENTITY.AUTOMATION].recSize)
  new DataView(args.buffer).setBigUint64(0, BigInt(id), true)
  args.set(encodeAutomationRecord(rule), 8)
  return encodeCommand(CMD.AUTOMATION_PUT, args)
}

// AUTOMATION_REMOVE args = u64 id.
export function automationRemove(id) {
  const args = new Uint8Array(8)
  new DataView(args.buffer).setBigUint64(0, BigInt(id), true)
  return encodeCommand(CMD.AUTOMATION_REMOVE, args)
}

// DEVICE_REMOVE args = u64 uid (пометка на удаление; leave при появлении).
export function removeDevice(uid) {
  const args = new Uint8Array(8)
  new DataView(args.buffer).setBigUint64(0, BigInt(uid), true)
  return encodeCommand(CMD.DEVICE_REMOVE, args)
}

export function cancelRemoveDevice(uid) {
  const args = new Uint8Array(8)
  new DataView(args.buffer).setBigUint64(0, BigInt(uid), true)
  return encodeCommand(CMD.DEVICE_REMOVE_CANCEL, args)
}

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

// GROUP_ITEM_PUT args = key{ u64 group_id; u64 uid; u16 cluster; u16 attr; u8 ep } (24) | record{ u16 order; u8 rsv[2]; char title[32] } (36).
export function groupItemPut(groupId, state, record) {
  const out = new Uint8Array(24 + 36)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, BigInt(groupId), true)
  dv.setBigUint64(8, BigInt(state.uid), true)
  dv.setUint16(16, state.cluster, true)
  dv.setUint16(18, state.attr, true)
  dv.setUint8(20, state.ep)
  dv.setUint16(24, record.order || 0, true)
  putText(out, 28, record.title, 32)
  return encodeCommand(CMD.GROUP_ITEM_PUT, out)
}

// GROUP_ITEM_REMOVE args = key (24).
export function groupItemRemove(groupId, state) {
  const out = new Uint8Array(24)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, BigInt(groupId), true)
  dv.setBigUint64(8, BigInt(state.uid), true)
  dv.setUint16(16, state.cluster, true)
  dv.setUint16(18, state.attr, true)
  dv.setUint8(20, state.ep)
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

// DEVICE_RENAME args = u64 uid | char name[32].
export function renameDevice(uid, name) {
  const out = new Uint8Array(8 + 32)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, uid, true)
  const bytes = new TextEncoder().encode(name).subarray(0, 31)
  out.set(bytes, 8)
  return encodeCommand(CMD.DEVICE_RENAME, out)
}
