// Генератор иконочных шрифтов LVGL.
//
// Два независимых набора (раздельные шрифты — без конфликта кодпоинтов):
//   icons.txt         -> ui_icons.c/.h   (FontAwesome free solid; UI_ICON_*)
//   weather_icons.txt -> wicons.c/.h     (Weather Icons;            WEATHER_ICON_*)
//
// Юникоды резолвятся по metadata пакетов (не хардкодим). Запуск из tools:
//   npm install && node gen.mjs

import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { execFileSync } from "node:child_process";

const HERE = dirname(fileURLToPath(import.meta.url));
const ICONS_DIR = join(HERE, "..");
const SIZE = 28;
const BPP = 4;

const FA_META = JSON.parse(
  readFileSync(join(HERE, "node_modules/@fortawesome/fontawesome-free/metadata/icon-families.json"), "utf8"),
);
const FA_TTF = join(HERE, "node_modules/@fortawesome/fontawesome-free/webfonts/fa-solid-900.ttf");

const WI_TTF = join(HERE, "node_modules/weather-icons/font/weathericons-regular-webfont.ttf");
const WI_MAP = (() => {
  const css = readFileSync(join(HERE, "node_modules/weather-icons/css/weather-icons.css"), "utf8");
  const map = {};
  const re = /\.wi-([a-z0-9-]+):before\s*\{\s*content:\s*"\\([0-9a-fA-F]+)"/g;
  let m;
  while ((m = re.exec(css))) map[m[1]] = parseInt(m[2], 16);
  return map;
})();

function readNames(path) {
  return readFileSync(path, "utf8")
    .split(/\r?\n/)
    .map((l) => l.replace(/#.*$/, "").trim())
    .filter(Boolean);
}

function generate({ fontName, listFile, ttf, resolve, resolveAll, prefix }) {
  const names = readNames(join(ICONS_DIR, listFile));
  /* Строка "*" означает «весь набор источника» — список имён не ведём вручную. */
  const list = names.includes("*") ? resolveAll() : names;
  const resolved = list.map((name) => {
    const code = resolve(name);
    if (code == null) throw new Error(`${listFile}: значок "${name}" не найден`);
    return { macro: `${prefix}_${name.toUpperCase().replace(/-/g, "_")}`, code };
  });

  const range = resolved.map((r) => "0x" + r.code.toString(16).toUpperCase()).join(",");
  const outC = join(ICONS_DIR, `${fontName}.c`);
  execFileSync(
    process.execPath,
    [
      join(HERE, "node_modules/lv_font_conv/lv_font_conv.js"),
      "--font", ttf,
      "--size", String(SIZE),
      "--bpp", String(BPP),
      "--format", "lvgl",
      "--range", range,
      "--no-compress",
      "--lv-include", "lvgl.h",
      "--lv-font-name", fontName,
      "-o", outC,
    ],
    { stdio: "inherit" },
  );

  const macros = resolved
    .map((r) => `#define ${r.macro} "${"\\u" + r.code.toString(16).padStart(4, "0")}"`)
    .join("\n");
  writeFileSync(
    join(ICONS_DIR, `${fontName}.h`),
    `#pragma once

/* Сгенерировано tools/gen.mjs из ${listFile} (${SIZE}px). Не править вручную. */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(${fontName});

${macros}

#ifdef __cplusplus
}
#endif
`,
  );
  console.log(`${fontName}: ${resolved.length} глифов -> ${fontName}.c/.h`);
}

generate({
  fontName: "ui_icons",
  listFile: "icons.txt",
  ttf: FA_TTF,
  resolve: (name) => (FA_META[name] ? parseInt(FA_META[name].unicode, 16) : null),
  resolveAll: () => Object.keys(FA_META).filter((n) => FA_META[n].unicode),
  prefix: "UI_ICON",
});

generate({
  fontName: "wicons",
  listFile: "weather_icons.txt",
  ttf: WI_TTF,
  resolve: (name) => (name in WI_MAP ? WI_MAP[name] : null),
  resolveAll: () => Object.keys(WI_MAP),
  prefix: "WEATHER_ICON",
});
