#pragma once

/*
 * Единственное место цветов UI. Палитра выбирается макросом UI_PALETTE; всё остальное
 * берёт роли UI_COL_* отсюда. Чтобы примерить другую тему — правится только этот файл
 * (или добавляется пресет ниже).
 *
 * Роли:
 *   UI_COL_BG      фон экрана
 *   UI_COL_CARD    поверхности (карточки, меню, чипы-кнопки, строка состояния)
 *   UI_COL_TEXT    основной текст
 *   UI_COL_MUTED   вторичный текст
 *   UI_COL_ACCENT  акцент (активные элементы, кнопки действия, слайдер)
 *   UI_COL_OK      «нормально/занято» (индикаторы)
 *   UI_COL_DANGER  «тревога» (индикаторы)
 *   UI_COL_CHIP    неактивный чип/точка карусели
 */

#define UI_PALETTE_DARK 0
#define UI_PALETTE_WARM 1

/* Активная палитра. Меняется здесь одной строкой. */
#ifndef UI_PALETTE
#define UI_PALETTE UI_PALETTE_WARM
#endif

#if UI_PALETTE == UI_PALETTE_WARM
/*
 * Тёплая палитра из макета: #524646 (тёмно-коричневый), #A8A492 (серо-зелёный),
 * #FCF2E5 (кремовый), #EC5B38 (оранжевый). Производные (CARD/CHIP/OK/DANGER) —
 * оттенки базы, чтобы ролей хватило.
 */
#define UI_COL_BG     0x524646
#define UI_COL_CARD   0x5F5252
#define UI_COL_TEXT   0xFCF2E5
#define UI_COL_MUTED  0xA8A492
#define UI_COL_ACCENT 0xEC5B38
#define UI_COL_OK     0x9DBF6A
#define UI_COL_DANGER 0xC0392B
#define UI_COL_CHIP   0x6B5C5C
#elif UI_PALETTE == UI_PALETTE_DARK
/* Прежняя тёмно-синяя тема. */
#define UI_COL_BG     0x0F141A
#define UI_COL_CARD   0x1B2430
#define UI_COL_TEXT   0xF3F6F9
#define UI_COL_MUTED  0x93A1B0
#define UI_COL_ACCENT 0x37A2F0
#define UI_COL_OK     0x3FBF7F
#define UI_COL_DANGER 0xE0533D
#define UI_COL_CHIP   0x2A3440
#else
#error "UI_PALETTE: неизвестная палитра"
#endif
