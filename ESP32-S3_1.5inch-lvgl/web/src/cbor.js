// Minimal CBOR encoder/decoder for browser usage.
// Supports definite-length: map/object, array, text, uint/int, bool, null, float64.

function readUint(dv, o, n) {
	let v = 0n
	for (let i = 0; i < n; i++) v = (v << 8n) | BigInt(dv.getUint8(o + i))
	return v
}

function readArg(dv, st, ai) {
	if (ai < 24) return BigInt(ai)
	if (ai === 24) {
		const v = dv.getUint8(st.o)
		st.o += 1
		return BigInt(v)
	}
	if (ai === 25) {
		const v = readUint(dv, st.o, 2)
		st.o += 2
		return v
	}
	if (ai === 26) {
		const v = readUint(dv, st.o, 4)
		st.o += 4
		return v
	}
	if (ai === 27) {
		const v = readUint(dv, st.o, 8)
		st.o += 8
		return v
	}
	throw new Error('indefinite not supported')
}

function decodeItem(dv, st) {
	const ib = dv.getUint8(st.o)
	st.o += 1
	const major = ib >> 5
	const ai = ib & 0x1f

	if (major === 0) {
		const u = readArg(dv, st, ai)
		// safe-ish: we only expect 32-bit-ish numbers in our protocol
		return Number(u)
	}
	if (major === 1) {
		const u = readArg(dv, st, ai)
		return -1 - Number(u)
	}
	if (major === 3) {
		const n = Number(readArg(dv, st, ai))
		const bytes = new Uint8Array(dv.buffer, dv.byteOffset + st.o, n)
		st.o += n
		return new TextDecoder().decode(bytes)
	}
	if (major === 4) {
		const n = Number(readArg(dv, st, ai))
		const arr = new Array(n)
		for (let i = 0; i < n; i++) arr[i] = decodeItem(dv, st)
		return arr
	}
	if (major === 5) {
		const n = Number(readArg(dv, st, ai))
		const obj = {}
		for (let i = 0; i < n; i++) {
			const k = decodeItem(dv, st)
			const v = decodeItem(dv, st)
			obj[String(k)] = v
		}
		return obj
	}
	if (major === 7) {
		if (ai === 20) return false
		if (ai === 21) return true
		if (ai === 22) return null
		if (ai === 27) {
			const u = readUint(dv, st.o, 8)
			st.o += 8
			const tmp = new ArrayBuffer(8)
			const dv2 = new DataView(tmp)
			dv2.setBigUint64(0, u)
			return dv2.getFloat64(0)
		}
		throw new Error('unsupported simple/float')
	}

	throw new Error('unsupported major')
}

export function cborDecode(input) {
	const u8 = input instanceof Uint8Array ? input : new Uint8Array(input)
	const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength)
	const st = { o: 0 }
	const v = decodeItem(dv, st)
	return v
}

function pushU8(out, v) { out.push(v & 0xff) }
function pushBE(out, v, n) {
	let x = BigInt(v)
	for (let i = n - 1; i >= 0; i--) {
		out.push(Number((x >> BigInt(i * 8)) & 0xffn))
	}
}

function encUInt(out, major, v) {
	const x = BigInt(v)
	if (x < 24n) {
		pushU8(out, (major << 5) | Number(x))
		return
	}
	if (x <= 0xffn) {
		pushU8(out, (major << 5) | 24)
		pushU8(out, Number(x))
		return
	}
	if (x <= 0xffffn) {
		pushU8(out, (major << 5) | 25)
		pushBE(out, x, 2)
		return
	}
	if (x <= 0xffffffffn) {
		pushU8(out, (major << 5) | 26)
		pushBE(out, x, 4)
		return
	}
	pushU8(out, (major << 5) | 27)
	pushBE(out, x, 8)
}

function encText(out, s) {
	const bytes = new TextEncoder().encode(String(s))
	encUInt(out, 3, bytes.length)
	for (const b of bytes) out.push(b)
}

function encBool(out, b) {
	pushU8(out, (7 << 5) | (b ? 21 : 20))
}

function encNull(out) { pushU8(out, (7 << 5) | 22) }

function encFloat64(out, n) {
	pushU8(out, (7 << 5) | 27)
	const buf = new ArrayBuffer(8)
	const dv = new DataView(buf)
	dv.setFloat64(0, Number(n))
	const u = dv.getBigUint64(0)
	pushBE(out, u, 8)
}

function isPlainObject(v) {
	return v != null && typeof v === 'object' && !Array.isArray(v) && !(v instanceof Uint8Array) && !(v instanceof ArrayBuffer)
}

function encItem(out, v) {
	if (v === null || v === undefined) return encNull(out)
	if (typeof v === 'boolean') return encBool(out, v)
	if (typeof v === 'number') {
		if (Number.isFinite(v) && Number.isInteger(v) && v >= 0) return encUInt(out, 0, v)
		if (Number.isFinite(v) && Number.isInteger(v) && v < 0) return encUInt(out, 1, -1 - v)
		return encFloat64(out, v)
	}
	if (typeof v === 'string') return encText(out, v)
	if (Array.isArray(v)) {
		encUInt(out, 4, v.length)
		for (const it of v) encItem(out, it)
		return
	}
	if (isPlainObject(v)) {
		const keys = Object.keys(v)
		encUInt(out, 5, keys.length)
		for (const k of keys) {
			encText(out, k)
			encItem(out, v[k])
		}
		return
	}
	// Fallback: stringify unknown types
	encText(out, String(v))
}

export function cborEncode(v) {
	const out = []
	encItem(out, v)
	return new Uint8Array(out)
}

