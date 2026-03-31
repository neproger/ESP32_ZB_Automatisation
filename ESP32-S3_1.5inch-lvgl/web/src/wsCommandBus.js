let sSender = null

export function setWsCommandSender(sender) {
	sSender = typeof sender === 'function' ? sender : null
}

export function sendWsCommand(buffer) {
	if (typeof sSender !== 'function') {
		throw new Error('ws not connected')
	}
	sSender(buffer)
}
