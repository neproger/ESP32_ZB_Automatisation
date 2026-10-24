// Differential golden: execute original ZHC toZigbee converters with a mock
// endpoint and emit the expected ZCL command + payload for the Rust tests.
//
// usage: node converters_golden.cjs <zhc-package>
const fs = require('node:fs');
const path = require('node:path');

const pkg = process.argv[2];
if (!pkg) {
  console.error('usage: node converters_golden.cjs <zhc-package>');
  process.exit(2);
}
const tz = require(path.join(pkg, 'dist/converters/toZigbee'));

class Endpoint {}
function endpoint(calls, definition) {
  const ep = new Endpoint();
  ep.command = async (cluster, command, payload) => {
    calls.push({ cluster, command, payload });
  };
  ep.getDevice = () => ({ manufacturerID: 0 });
  ep.getClusterAttributeValue = (_cluster, attribute) =>
    attribute === 'colorTempPhysicalMin' ? 250 : attribute === 'colorTempPhysicalMax' ? 454 : undefined;
  ep.deviceIeeeAddress = '0x00124b0000000001';
  ep.meta = definition.meta || {};
  return ep;
}

function meta(message, definition) {
  return {
    message,
    state: { state: 'ON', brightness: 100, color_mode: 'xy' },
    options: { transition: 0 },
    mapped: definition,
    device: { manufacturerID: 0 },
  };
}

function u16(value) {
  return [value & 0xff, (value >> 8) & 0xff];
}

function encode(call) {
  const { command, payload } = call;
  const options = [];
  if (payload.optionsMask !== undefined) options.push(payload.optionsMask);
  if (payload.optionsOverride !== undefined) options.push(payload.optionsOverride);
  const transition = u16(payload.transtime || 0);
  switch (command) {
    case 'on':
    case 'off':
    case 'toggle':
      return { id: { on: 0x01, off: 0x00, toggle: 0x02 }[command], bytes: [] };
    case 'moveToLevelWithOnOff':
    case 'moveToLevel':
      return {
        id: command === 'moveToLevelWithOnOff' ? 0x04 : 0x00,
        bytes: [payload.level, ...transition, ...options],
      };
    case 'moveToColorTemp':
      return { id: 0x0a, bytes: [...u16(payload.colortemp), ...transition, ...options] };
    case 'moveToHueAndSaturation':
      return {
        id: 0x06,
        bytes: [payload.hue, payload.saturation, ...transition, ...options],
      };
    case 'enhancedMoveToHueAndSaturation':
      return {
        id: 0x43,
        bytes: [...u16(payload.enhancehue), payload.saturation, ...transition, ...options],
      };
    case 'moveToColor':
      return {
        id: 0x07,
        bytes: [...u16(payload.colorx), ...u16(payload.colory), ...transition, ...options],
      };
    default:
      return null;
  }
}

async function run(name, key, converter, message, definition) {
  const calls = [];
  const ep = endpoint(calls, definition);
  await converter.convertSet(ep, key, message[key], meta(message, definition));
  const call = calls[0];
  if (!call) return null;
  const encoded = encode(call);
  if (!encoded) return null;
  return {
    case: name,
    cluster: call.cluster,
    command: call.command,
    zcl: encoded.id,
    bytes: encoded.bytes,
  };
}

(async () => {
  const xyDef = { meta: { supportsHueAndSaturation: false } };
  const hsDef = { meta: { supportsHueAndSaturation: true } };
  const enhancedDef = {
    meta: { supportsHueAndSaturation: true, supportsEnhancedHue: true },
  };
  const cases = [
    ['state_on', 'state', tz.on_off, { state: 'ON' }, xyDef],
    ['state_off', 'state', tz.on_off, { state: 'OFF' }, xyDef],
    ['brightness', 'brightness', tz.light_onoff_brightness, { brightness: 100 }, xyDef],
    ['color_temp', 'color_temp', tz.light_colortemp, { color_temp: 350 }, xyDef],
    ['color_xy', 'color', tz.light_color, { color: { x: 0.5, y: 0.25 } }, xyDef],
    ['color_hs', 'color', tz.light_color, { color: { h: 180, s: 50 } }, hsDef],
    ['color_enhanced', 'color', tz.light_color, { color: { h: 180, s: 50 } }, enhancedDef],
  ];
  const rows = [];
  for (const [name, key, converter, message, definition] of cases) {
    const result = await run(name, key, converter, message, definition);
    if (!result) {
      console.error('no command for', name);
      process.exit(1);
    }
    rows.push(`${result.case}\t${result.cluster}\t${result.command}\t0x${result.zcl
      .toString(16)
      .padStart(2, '0')}\t${result.bytes.map((b) => b.toString(16).padStart(2, '0')).join('')}`);
  }
  const out = path.join(__dirname, '../../crates/z2m-rs/tests/golden/converters.tsv');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(out, rows.join('\n') + '\n');
  console.log(rows.join('\n'));
})();
