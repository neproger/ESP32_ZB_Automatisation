// Incoming differential: execute ZHC fromZigbee converters and emit normalized
// values for the Rust decoder tests.
//
// usage: node from_golden.cjs [zhc-package]
// Defaults come from tools/zhc-compiler's pinned npm dependency.
const fs = require('node:fs');
const path = require('node:path');
const { pkg } = require('../zhc-compiler/src/cli.cjs');
const fz = require(path.join(pkg, 'dist/converters/fromZigbee'));

class Endpoint {}
const ep = new Endpoint();
ep.getClusterAttributeValue = (_cluster, attribute) =>
  attribute === 'colorTempPhysicalMin' ? 250 : attribute === 'colorTempPhysicalMax' ? 454 : undefined;
ep.getDevice = () => ({ manufacturerID: 0 });

function flatten(result) {
  const out = {};
  if (!result) return out;
  if (result.state !== undefined) out.state = result.state;
  if (result.brightness !== undefined) out.brightness = result.brightness;
  if (result.color_temp !== undefined) out.color_temp = result.color_temp;
  if (result.color_mode !== undefined) out.color_mode = result.color_mode;
  if (result.color) {
    for (const key of ['x', 'y', 'hue', 'saturation']) {
      if (result.color[key] !== undefined) out[key] = result.color[key];
    }
  }
  return out;
}

const cases = [
  {
    name: 'state_on',
    cluster: 0x0006,
    data: { onOff: 1 },
    attrs: [[0x0000, 0x10, 1]],
    convert: fz.on_off,
  },
  {
    name: 'state_off',
    cluster: 0x0006,
    data: { onOff: 0 },
    attrs: [[0x0000, 0x10, 0]],
    convert: fz.on_off,
  },
  {
    name: 'state_invalid',
    cluster: 0x0006,
    data: { onOff: 2 },
    attrs: [[0x0000, 0x10, 2]],
    convert: fz.on_off,
  },
  {
    name: 'brightness',
    cluster: 0x0008,
    data: { currentLevel: 127 },
    attrs: [[0x0000, 0x20, 127]],
    convert: fz.brightness,
  },
  {
    name: 'color_temp',
    cluster: 0x0300,
    data: { colorTemperature: 350 },
    attrs: [[0x0007, 0x21, 350]],
    convert: fz.color_colortemp,
  },
  {
    name: 'color_xy',
    cluster: 0x0300,
    data: { currentX: 32000, currentY: 24000, colorMode: 1 },
    attrs: [
      [0x0003, 0x21, 32000],
      [0x0004, 0x21, 24000],
      [0x0008, 0x30, 1],
    ],
    convert: fz.color_colortemp,
  },
  {
    name: 'color_x_only',
    cluster: 0x0300,
    data: { currentX: 32000, colorMode: 1 },
    attrs: [
      [0x0003, 0x21, 32000],
      [0x0008, 0x30, 1],
    ],
    convert: fz.color_colortemp,
  },
  {
    name: 'color_hs',
    cluster: 0x0300,
    data: { currentHue: 127, currentSaturation: 127, colorMode: 0 },
    attrs: [
      [0x0000, 0x20, 127],
      [0x0001, 0x20, 127],
      [0x0008, 0x30, 0],
    ],
    convert: fz.color_colortemp,
  },
  {
    name: 'color_hue_only',
    cluster: 0x0300,
    data: { currentHue: 127, colorMode: 0 },
    attrs: [
      [0x0000, 0x20, 127],
      [0x0008, 0x30, 0],
    ],
    convert: fz.color_colortemp,
  },
  {
    name: 'color_empty',
    cluster: 0x0300,
    data: {},
    attrs: [],
    convert: fz.color_colortemp,
  },
];

const rows = [];
for (const item of cases) {
  const result = flatten(item.convert.convert(ep, { data: item.data, endpoint: ep }, () => {}, {}, {}));
  const attrs = item.attrs.map(([id, type, value]) => `${id}:${type}:${value}`).join(',');
  const expected = Object.entries(result)
    .map(([key, value]) => `${key}=${value}`)
    .join(',');
  rows.push(`${item.name}\t0x${item.cluster.toString(16)}\t${attrs}\t${expected}`);
}
const out = path.join(__dirname, '../../crates/z2m-rs/tests/golden/from_converters.tsv');
fs.writeFileSync(out, rows.join('\n') + '\n');
console.log(rows.join('\n'));
