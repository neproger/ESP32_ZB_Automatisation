// Кадры протокола v2 (docs/services/WEB_PROTOCOL.md). Little-endian, заголовок 8 байт.

export const MSG = {
  SYNC_BEGIN: 0x01,
  SYNC_END: 0x02,
  ENTITY: 0x10,
  ENTITY_REMOVE: 0x11,
  COMMAND: 0x20,
  CMD_RESULT: 0x21,
}

export const CMD = {
  SNAPSHOT: 1,
  ZB_COMMAND: 2,
  DEVICE_RENAME: 3,
  AUTOMATION_PUT: 4,
  AUTOMATION_REMOVE: 5,
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

// DEVICE_RENAME args = u64 uid | char name[32].
export function renameDevice(uid, name) {
  const out = new Uint8Array(8 + 32)
  const dv = new DataView(out.buffer)
  dv.setBigUint64(0, uid, true)
  const bytes = new TextEncoder().encode(name).subarray(0, 31)
  out.set(bytes, 8)
  return encodeCommand(CMD.DEVICE_RENAME, out)
}
