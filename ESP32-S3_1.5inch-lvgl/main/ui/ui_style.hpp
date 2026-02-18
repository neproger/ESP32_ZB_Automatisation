#pragma once

#include <stdint.h>
#include "lvgl.h"
#include "fonts.h"

namespace ui_style
{
// Palette
static constexpr uint32_t kScreenBgHex = 0x000000;
static constexpr uint32_t kTitleTextHex = 0xf8fafc;
static constexpr uint32_t kSubtitleTextHex = 0x9fb0d9;
static constexpr uint32_t kPanelBgHex = 0x111827;
static constexpr uint32_t kCardBgHex = 0x16223f;
static constexpr uint32_t kBorderHex = 0x2f3c63;
static constexpr uint32_t kCardTitleHex = 0xe5e7eb;

// Typography
static const lv_font_t *const kFontTheme = &Montserrat_30;
static const lv_font_t *const kFontTitle = &Montserrat_40;
static const lv_font_t *const kFontSubtitle = &Montserrat_30;
static const lv_font_t *const kFontBody = &Montserrat_30;
static const lv_font_t *const kFontSectionTitle = &Montserrat_30;

// Spacing and layout
static constexpr int32_t kTitleX = 12;
static constexpr int32_t kTitleY = 20;
static constexpr int32_t kSubtitleX = 12;
static constexpr int32_t kSubtitleY = 44;
static constexpr int32_t kListWidth = 390;
static constexpr int32_t kListHeight = 390;
static constexpr int32_t kListBottomOffsetY = 65;
static constexpr int32_t kCardBottomOffset = 8;
static constexpr int32_t kListPad = 8;
static constexpr int32_t kListItemGap = 8;
static constexpr int32_t kCardPad = 10;
static constexpr int32_t kCardRadius = 10;
static constexpr int32_t kCardRowGap = 6;

// Controls
static constexpr uint16_t kLevelMin = 0;
static constexpr uint16_t kLevelMax = 254;
} // namespace ui_style
