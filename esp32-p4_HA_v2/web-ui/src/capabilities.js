// Capabilities endpoint'а (порт v1 deriveEndpointMeta): из кластеров с ролью
// (server=принимает команды, client=отдаёт события) строим теги accepts/emits/reports.
// Виджеты выбираются по этим тегам.

export function deriveEndpointMeta(record) {
  const inClusters = []
  const outClusters = []
  const accepts = []
  const emits = []
  const reports = []

  for (const c of record?.clusters || []) {
    if (c.role === 1) {
      // server: устройство принимает команды на этот кластер
      inClusters.push(c.id)
      if (c.id === 0x0006) accepts.push('onoff.on', 'onoff.off', 'onoff.toggle')
      if (c.id === 0x0008) accepts.push('level.move_to_level')
      if (c.id === 0x0300) accepts.push('color.move_to_color_xy', 'color.move_to_color_temperature')
      if (c.id === 0x0402) reports.push('temperature_c')
      if (c.id === 0x0405) reports.push('humidity_pct')
      if (c.id === 0x0001) reports.push('battery_pct')
      if (c.id === 0x0406) reports.push('occupancy')
    } else {
      // client: устройство шлёт события / репорты
      outClusters.push(c.id)
      if (c.id === 0x0006) emits.push('onoff.off', 'onoff.on', 'onoff.toggle', 'button.single')
      if (c.id === 0x0008) emits.push('level')
    }
  }
  return { inClusters, outClusters, accepts, emits, reports }
}

export const hasAccept = (meta, prefix) =>
  (meta?.accepts || []).some((x) => x.startsWith(prefix))

export const hasReport = (meta, key) => (meta?.reports || []).includes(key)
