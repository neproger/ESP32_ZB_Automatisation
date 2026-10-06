#include "ui_palette.h"

/* Порядок полей совпадает с ui_palette_role_t. Производные (CARD/CHIP/OK/DANGER) —
 * оттенки базовых цветов темы, чтобы ролей хватило. */
static const uint32_t kRetro[UI_ROLE_COUNT] = {
    /* BG     */ 0x000000, /* чёрный       */
    /* CARD   */ 0x233D4D, /* тёмно-синий  */
    /* TEXT   */ 0xEAECF0, /* светло-серый */
    /* MUTED  */ 0x98A2AD,
    /* ACCENT */ 0xFE7F2D, /* оранжевый    */
    /* OK     */ 0x6FBF73,
    /* DANGER */ 0xE05440,
    /* CHIP   */ 0x33505F,
};

static const uint32_t kWarm[UI_ROLE_COUNT] = {
    /* BG     */ 0x524646,
    /* CARD   */ 0x5F5252,
    /* TEXT   */ 0xFCF2E5,
    /* MUTED  */ 0xA8A492,
    /* ACCENT */ 0xEC5B38,
    /* OK     */ 0x9DBF6A,
    /* DANGER */ 0xC0392B,
    /* CHIP   */ 0x6B5C5C,
};

static const uint32_t kDark[UI_ROLE_COUNT] = {
    /* BG     */ 0x0F141A,
    /* CARD   */ 0x1B2430,
    /* TEXT   */ 0xF3F6F9,
    /* MUTED  */ 0x93A1B0,
    /* ACCENT */ 0x37A2F0,
    /* OK     */ 0x3FBF7F,
    /* DANGER */ 0xE0533D,
    /* CHIP   */ 0x2A3440,
};

static const uint32_t kSummer[UI_ROLE_COUNT] = {
    /* BG     */ 0xFDF4AF, /* светло-жёлтый */
    /* CARD   */ 0xA5E9DD, /* светло-бирюзовый */
    /* TEXT   */ 0x234E4A, /* тёмно-бирюзовый (читаем на светлом) */
    /* MUTED  */ 0x34908B,
    /* ACCENT */ 0x6FBEB2, /* бирюзовый (кнопки/слайдер) */
    /* OK     */ 0x4FB477,
    /* DANGER */ 0xE05440,
    /* CHIP   */ 0x8FD4C8,
};

static const uint32_t kWinter[UI_ROLE_COUNT] = {
    /* BG     */ 0x091413, /* почти чёрный */
    /* CARD   */ 0x285A48, /* тёмно-зелёный */
    /* TEXT   */ 0xB0E4CC, /* светло-мятный */
    /* MUTED  */ 0x7FBFA6,
    /* ACCENT */ 0x408A71, /* зелёный */
    /* OK     */ 0x4FBF87,
    /* DANGER */ 0xE0533D,
    /* CHIP   */ 0x35695A,
};

static const uint32_t *const kPalettes[UI_PALETTE_COUNT] = {kRetro, kWarm, kDark, kSummer, kWinter};
static const char *const kNames[UI_PALETTE_COUNT] = {"Retro", "Warm", "Dark", "Summer", "Winter"};

static ui_palette_id_t s_id = UI_PALETTE_RETRO;

uint32_t ui_palette_color(ui_palette_role_t role)
{
    if ((unsigned)role >= UI_ROLE_COUNT) {
        return 0;
    }
    return kPalettes[s_id][role];
}

ui_palette_id_t ui_palette_id(void)
{
    return s_id;
}

void ui_palette_set(ui_palette_id_t id)
{
    if ((unsigned)id < UI_PALETTE_COUNT) {
        s_id = id;
    }
}

const char *ui_palette_name(ui_palette_id_t id)
{
    if ((unsigned)id >= UI_PALETTE_COUNT) {
        return "?";
    }
    return kNames[id];
}
