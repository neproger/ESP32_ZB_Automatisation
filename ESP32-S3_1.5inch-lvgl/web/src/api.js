import { cborDecode, cborEncode } from './cbor'

const CBOR_HEADERS = {
	Accept: 'application/cbor',
	'Content-Type': 'application/cbor',
}

async function handleResponse(res, method, path) {
	if (!res.ok) {
		const ct = String(res.headers.get('content-type') ?? '')
		const details = ct.includes('text/') || ct.includes('application/json') ? await res.text() : ''
		const suffix = details ? ` ${details.trim()}` : ''
		throw new Error(`${method} ${path} failed: ${res.status}${suffix}`)
	}

	// esp_http_server often uses chunked responses without Content-Length.
	const buf = await res.arrayBuffer()
	if (!buf || buf.byteLength === 0) return null
	return cborDecode(buf)
}

export async function fetchCbor(path, options = {}) {
	const method = String(options.method ?? 'GET').toUpperCase()
	const res = await fetch(path, {
		...options,
		headers: {
			...CBOR_HEADERS,
			...(options.headers || {}),
		},
	})
	return handleResponse(res, method, path)
}

function buildOptions(method, body, overrides = {}) {
	const opts = {
		method,
		...overrides,
		headers: {
			...CBOR_HEADERS,
			...(overrides.headers || {}),
		},
	}
	if (body !== undefined) {
		opts.body = cborEncode(body)
	}
	return opts
}

export async function postCbor(path, body, overrides = {}) {
	return fetchCbor(path, buildOptions('POST', body, overrides))
}

export async function patchCbor(path, body, overrides = {}) {
	return fetchCbor(path, buildOptions('PATCH', body, overrides))
}

export async function deleteCbor(path, overrides = {}) {
	return fetchCbor(path, { method: 'DELETE', ...overrides, headers: { ...CBOR_HEADERS, ...(overrides.headers || {}) } })
}

export async function execAction(payload) {
	const body = Array.isArray(payload) ? { actions: payload } : { action: payload }
	return postCbor('/api/actions', body)
}
