/*
 * vim:fileencoding=utf-8
 * fonts.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */
#include "fonts.h"
#include "state.h"
#include "emoji.h"
#include "unicode-data.h"
#include <stdbool.h>

/**
 * TODO 何？
 */
#define MISSING_GLYPH        (4)

/**
 * エクストラ・グリフの数
 */
#define MAX_EXTRA_GLYPHS     (8u)

/**
 * キャンバス(ピクセルバッファ)に含まれるセルの数
 */
#define MAX_CELLS_IN_CANVAS  ((MAX_EXTRA_GLYPHS + 1u) * 3u)

/**
 * TODO 何？
 */
#define MAX_NUM_EXTRA_GLYPHS_PUA 4u

/**
 * スプライトをGPUに転送する関数の型
 */
typedef void (*send_sprite_to_gpu_func)(FONTS_DATA_HANDLE, unsigned int, unsigned int, unsigned int, pixel *);

/**
 * スプライトをGPUに転送する関数
 */
send_sprite_to_gpu_func current_send_sprite_to_gpu = NULL;

/**
 * TODO 何？
 */
static PyObject *python_send_to_gpu_impl = NULL;

/**
 * TODO 何？
 */
extern PyTypeObject Line_Type;

/**
 * TODO 何？
 */
enum {
    NO_FONT      = -3,
    MISSING_FONT = -2,
    BLANK_FONT   = -1,
    BOX_FONT     = 0
};

/**
 * エクストラ・グリフ
 */
typedef struct {
    glyph_index data[MAX_EXTRA_GLYPHS];
} ExtraGlyphs;

typedef struct SpritePosition SpritePosition;

/**
 * スプライト位置
 */
struct SpritePosition {
    /**
     * 次のスプライト
     */
    SpritePosition *next;

    /**
     * フラグ
     *  - filled
     *  - rendered
     *  - colored
     */
    bool filled, rendered, colored;

    /**
     * スプライトインデックス
     *  - x
     *  - y
     *  - z
     */
    sprite_index x, y, z;

    /**
     * リガチャのインデックス
     */
    uint8_t ligature_index;

    /**
     * グリフのインデックス
     */
    glyph_index glyph;

    /**
     * エクストラ・グリフ
     */
    ExtraGlyphs extra_glyphs;
};

#define SPECIAL_FILLED_MASK      1
#define SPECIAL_VALUE_MASK       2
#define EMPTY_FILLED_MASK        4
#define EMPTY_VALUE_MASK         8
#define SPECIAL_GLYPH_CACHE_SIZE 1024

typedef struct SpecialGlyphCache SpecialGlyphCache;

/**
 * 特殊グリフのキャッシュ
 */
struct SpecialGlyphCache {
    SpecialGlyphCache *next; /** 次の特殊グリフキャッシュへのポインタ */
    glyph_index glyph; /** グリフのインデックス */
    uint8_t data; /** データ */
};

/**
 * GPUスプライトの管理情報
 */
typedef struct {
    size_t max_y;   /** y方向にテクスチャに配置可能なセルの個数 */
    unsigned int x, y, z;   /** 最後にスプライトを配置した座標 */
    unsigned int xnum;   /** x方向にテクスチャに配置可能なセルの個数 */
    unsigned int ynum;  /** 1固定 */
} GPUSpriteTracker;

/**
 * HarfBuzzバッファ
 *  何故にstaticなのか...
 */
static hb_buffer_t *harfbuzz_buffer = NULL;

/**
 * HarfBuzz機能
 *  何故にstaticなのか...
 */
static hb_feature_t hb_features[3] = {{0}};

/**
 * 形状バッファ
 *  と言いつつ、型は char_type なので、文字コードの配列でしかない。
 *  文字コードは UTF32 。
 *  さらに、load_hb_buffer 関数でしか使用してない。
 *  スタックオーバーフロー避けたいんだろうけど、横着しないで malloc/free すれば
 *  良いのに...
 *
 *  何故にstaticなのか...
 *  何故に4096なのか...
 *
 * \see load_hb_buffer 関数でしか使用してない
 */
static char_type shape_buffer[4096] = {0};

/**
 * 最大テクスチャサイズとアレイサイズ
 *  何故にstaticなのか...
 *  何故に非constなのか...
 */
static size_t max_texture_size = 1024, max_array_len = 1024;
typedef enum {
    LIGA_FEATURE,
    DLIG_FEATURE,
    CALT_FEATURE
} HBFeature;

/**
 * 記号マップ
 */
typedef struct {
    /**
     * 文字コード範囲
     * - left
     * - right
     */
    char_type left, right;

    /**
     * フォントインデックス
     */
    size_t font_idx;
} SymbolMap;

/**
 * 記号マップ
 *  何故にstaticなのか
 */
static SymbolMap *symbol_maps = NULL;

/**
 * 記号マップの要素数
 */
static size_t num_symbol_maps = 0;

/**
 * フォント構造体
 */
typedef struct {
    PyObject *face;

    /**
     * スプライト座標のマップ
     *  キーはグリフインデックス
     */
    SpritePosition sprite_map[1024];

    /**
     * HarfBuzz機能の配列
     */
    hb_feature_t hb_features[8];

    /**
     * HarfBuzz機能の件数
     */
    size_t num_hb_features;

    /**
     * 特殊グリフのキャッシュ
     */
    SpecialGlyphCache special_glyph_cache[SPECIAL_GLYPH_CACHE_SIZE];

    /**
     * スタイル
     * - ボールド
     * - イタリック
     * - 絵文字表現
     */
    bool bold, italic, emoji_presentation;
} Font;

/**
 * フォントグループ構造体
 */
typedef struct {
    FONTS_DATA_HEAD
    id_type id;
    unsigned int baseline, underline_position, underline_thickness;
    size_t fonts_capacity, fonts_count, fallback_fonts_count;
    ssize_t medium_font_idx,
            bold_font_idx,
            italic_font_idx,
            bi_font_idx,
            first_symbol_font_idx,
            first_fallback_font_idx;
    Font *fonts;
    pixel *canvas;

    /**
     * スプライトトラッカー
     */
    GPUSpriteTracker sprite_tracker;
} FontGroup;

static inline
size_t fontgroup_get_canvas_pixel_count(const FontGroup *fg) {
    return MAX_CELLS_IN_CANVAS * fg->cell_width * fg->cell_height;
}

static inline
size_t fontgroup_get_canvas_byte_size(const FontGroup *fg) {
    return fontgroup_get_canvas_pixel_count(fg) * sizeof(pixel);
}

static inline
pixel* fontgroup_get_canvas_tail(FontGroup *fg) {
    return &fg->canvas[fg->cell_width * fg->cell_height * (MAX_CELLS_IN_CANVAS - 1)];
}

static inline
pixel* fontgroup_get_canvas_at(FontGroup *fg, unsigned at) {
    return &fg->canvas[fg->cell_width * at];
}

/**
 * フォントグループの配列
 *  何故にstaticなのか
 */
static FontGroup *font_groups = NULL;

/**
 * フォントグループ配列を伸長する時の余剰サイズ
 */
static size_t font_groups_capacity = 0;

/**
 * フォントグループの件数
 */
static size_t num_font_groups = 0;

/**
 * フォントグループのIDカウンタ
 */
static id_type font_group_id_counter = 0;

static void initialize_font_group(FontGroup *fg);

/**
 * ウィンドウの割り当てられているフォントグループを退避する
 */
static inline void
save_window_font_groups(void) {
    for (size_t o = 0; o < global_state.num_os_windows; o++) {
        OSWindow *w = global_state.os_windows + o;
        w->temp_font_group_id = w->fonts_data ? ((FontGroup *)(w->fonts_data))->id : 0;
    }
}

/**
 * 退避してあるフォントグループをウィンドウに再割当する
 */
static inline void
restore_window_font_groups(void) {
    for (size_t o = 0; o < global_state.num_os_windows; o++) {
        OSWindow *w = global_state.os_windows + o;
        w->fonts_data = NULL;
        for (size_t i = 0; i < num_font_groups; i++) {
            if (font_groups[i].id == w->temp_font_group_id) {
                w->fonts_data = (FONTS_DATA_HANDLE)(&font_groups[i]);
                break;
            }
        }
    }
}

/**
 * フォントグループが未使用かどうか調べる
 *  退避していたら未使用とみなす
 *
 * @param fg フォントグループ
 */
static inline bool
font_group_is_unused(FontGroup *fg) {
    for (size_t o = 0; o < global_state.num_os_windows; o++) {
        OSWindow *w = global_state.os_windows + o;
        if (w->temp_font_group_id == fg->id) {
            return false;
        }
    }
    return true;
}

/**
 * 未使用フォントグループを削除する
 */
static inline void
trim_unused_font_groups(void) {
    save_window_font_groups();
    size_t i = 0;
    while (i < num_font_groups) {
        if (font_group_is_unused(&font_groups[i])) {
            size_t num_to_right = (--num_font_groups) - i;
            if (!num_to_right) {
                break;
            }
            memmove(&font_groups[i], &font_groups[i + 1], num_to_right * sizeof(FontGroup));
        }
        else {
            i++;
        }
    }
    restore_window_font_groups();
}

/**
 * フォントグループの追加
 */
static inline void
add_font_group(void) {
    // 未使用のフォントグループを削除する
    if (num_font_groups) {
        trim_unused_font_groups();
    }

    // スロットに空きが無いなら伸長する
    if (num_font_groups >= font_groups_capacity) {
        save_window_font_groups();
        font_groups_capacity += 5;
        font_groups = realloc(font_groups, sizeof(FontGroup) * font_groups_capacity);
        if (!font_groups) {
            fatal("Out of memory creating a new font group");
        }
        restore_window_font_groups();
    }
    num_font_groups++;
}

/**
 * フォントグループの照合
 *  マッチするフォントグループがなければ生成する
 *
 * @param font_sz_in_pts ポイントサイズ
 * @param logical_dpi_x 論理DPIのx
 * @param logical_dpi_y 論理DPIのy
 * @return FontGroupオブジェクト
 */
static inline FontGroup *
font_group_for(double font_sz_in_pts, double logical_dpi_x, double logical_dpi_y) {
    // 照合する
    for (size_t i = 0; i < num_font_groups; i++) {
        FontGroup *fg = &font_groups[i];
        if (fg->font_sz_in_pts == font_sz_in_pts &&
            fg->logical_dpi_x == logical_dpi_x &&
            fg->logical_dpi_y == logical_dpi_y) {
            return fg;
        }
    }

    // フォントグループのスロットを確保して最後の要素に引数の値をコピーする
    add_font_group();
    FontGroup *fg = &font_groups[num_font_groups - 1];
    zero_at_ptr(fg);
    fg->font_sz_in_pts = font_sz_in_pts;
    fg->logical_dpi_x = logical_dpi_x;
    fg->logical_dpi_y = logical_dpi_y;
    fg->id = ++font_group_id_counter;

    // フォントグループの初期化
    initialize_font_group(fg);
    return fg;
}

/**
 * フォントグループが内包するキャンバスをクリア(0埋め)する
 *  キャンバスは `pixel` の配列である
 */
static inline void
clear_canvas(FontGroup *fg) {
    if (fg->canvas) {
        memset(fg->canvas, 0, fontgroup_get_canvas_byte_size(fg));
    }
}

// Sprites {{{

/**
 * スプライト・マップのエラーを設定する
 */
static inline void
sprite_map_set_error(int error) {
    switch (error) {
        case 1:
            PyErr_NoMemory();
            break;
        case 2:
            PyErr_SetString(PyExc_RuntimeError, "Out of texture space for sprites");
            break;
        default:
            PyErr_SetString(PyExc_RuntimeError, "Unknown error occurred while allocating sprites");
            break;
    }
}

/**
 * スプライト・トラッカーの制限値を設定する
 *
 * @param mts 最大テクスチャサイズ
 * @param mal 最大アレイ長
 */
void
sprite_tracker_set_limits(size_t mts, size_t mal) {
    max_texture_size = mts;
    max_array_len = MIN(0xfffu, mal);
}

/**
 * スプライト・トラッカー情報を更新する
 *  ファイルシステムのボリュームビットマップみたいなものか？
 *
 * @param fg フォントグループ
 * @param error エラー情報 [out]
 */
static inline void
do_increment(FontGroup *fg, int *error) {
    fg->sprite_tracker.x++;
    if (fg->sprite_tracker.x >= fg->sprite_tracker.xnum) {
        // xが最大値に到達したら0にリセットしてyを増やし ynum も更新する
        fg->sprite_tracker.x = 0;
        fg->sprite_tracker.y++;
        fg->sprite_tracker.ynum = MIN(MAX(fg->sprite_tracker.ynum, fg->sprite_tracker.y + 1), fg->sprite_tracker.max_y);
        if (fg->sprite_tracker.y >= fg->sprite_tracker.max_y) {
            // yが最大値に到達したら0にリセットして z を更新する
            fg->sprite_tracker.y = 0;
            fg->sprite_tracker.z++;
            if (fg->sprite_tracker.z >= MIN((size_t)UINT16_MAX, max_array_len)) {
                // zの最大値に到達したらエラー
                *error = 2;
            }
        }
    }
}

/**
 * ExtraGlyphsの等値比較
 *
 * @param a ExtraGlyphs
 * @param b ExtraGlyphs
 * @return 等しいなら真
 */
static inline bool
extra_glyphs_equal(ExtraGlyphs *a, ExtraGlyphs *b) {
    for (size_t i = 0; i < MAX_EXTRA_GLYPHS; i++) {
        if (a->data[i] != b->data[i]) {
            return false;
        }
        if (a->data[i] == 0) {
            return true;
        }
    }
    return true;
}

/**
 * グリフに対するスプライト位置を探す
 *
 * \param[in] fg フォントグループ
 * \param[in] font フォント
 * \param[in] glyph_index グリフのインデックス
 * \param[in] glyph グリフ
 * \param[in] extra_glyphs エクストラグリフ
 * \param[in] ligature_index リガチャのインデックス
 * \param[in] error エラー値[out]
 * \return スプライト位置
 */
static SpritePosition *
sprite_position_for(
    FontGroup *fg,
    Font *font,
    glyph_index glyph,
    ExtraGlyphs *extra_glyphs,
    uint8_t ligature_index,
    int *error
) {
    // グリフインデックスはグリフ値そのものである
    const glyph_index idx = glyph & (SPECIAL_GLYPH_CACHE_SIZE - 1);

    // グリフに対応するスプライトの位置を得る
    SpritePosition *sp = &font->sprite_map[idx];

    // 既にキャッシュにある1024未満のグリフの一般的なケースに対して最適化する
    if (LIKELY(sp->glyph == glyph &&
               sp->filled &&
               extra_glyphs_equal(&sp->extra_glyphs, extra_glyphs) &&
               sp->ligature_index == ligature_index)) {
        return sp; // キャッシュヒット
    }

    // キャッシュにない場合はsp->nextを辿って線形探索する
    while (true) {
        if (sp->filled) {
            if (sp->glyph == glyph &&
                extra_glyphs_equal(&sp->extra_glyphs, extra_glyphs) &&
                sp->ligature_index == ligature_index) {
                return sp; // ヒット
            }
        }
        else {
            break;
        }
        if (!sp->next) {
            sp->next = calloc(1, sizeof(SpritePosition));
            if (!sp->next) {
                *error = 1;
                return NULL;
            }
        }
        sp = sp->next;
    }

    // spは空のスロットを指しているのでスプライト位置情報を設定して返す
    sp->glyph = glyph;
    memcpy(&sp->extra_glyphs, extra_glyphs, sizeof(ExtraGlyphs));
    sp->ligature_index = ligature_index;
    sp->filled = true;
    sp->rendered = false;
    sp->colored = false;
    sp->x = fg->sprite_tracker.x;
    sp->y = fg->sprite_tracker.y;
    sp->z = fg->sprite_tracker.z;

    // スプライト・トラッカーを更新する
    do_increment(fg, error);

    return sp;
}

/**
 * 特殊グリフを照合する
 *
 * @param fg フォントグループ
 * @param glyph_index グリフのインデックス
 * @param filled_mask ？
 * @return 特殊グリフキャッシュの要素 (SpecialGlyphCache)
 */
static inline SpecialGlyphCache *
special_glyph_cache_for(Font *font, glyph_index glyph, uint8_t filled_mask) {
    SpecialGlyphCache *sg = &font->special_glyph_cache[glyph & 0x3ff];

    // 既にキャッシュにあるSPECIAL_GLYPH_CACHE_SIZE以下のグリフの一般的なケース
    // に対して最適化する
    if (LIKELY(sg->glyph == glyph && sg->data & filled_mask)) {
        return sg; // キャッシュヒット
    }
    while (true) {
        if (sg->data & filled_mask) {
            if (sg->glyph == glyph) {
                return sg; // キャッシュヒット
            }
        }
        else {
            if (!sg->glyph) {
                break; // キャッシュスロットが空
            }
            else if (sg->glyph == glyph) {
                return sg; // filled_maskで示されるデータ以外を含むキャッシュスロット
            }
        }
        if (!sg->next) {
            sg->next = calloc(1, sizeof(SpecialGlyphCache));
            if (!sg->next) {
                return NULL;
            }
        }
        sg = sg->next;
    }
    sg->glyph = glyph;
    return sg;
}

/**
 * スプライト・トラッカーの現レイアウトを取得する
 *
 * @param data フォントグループ
 * @param x x座標 [out]
 * @param y y座標 [out]
 * @param z z座標 [out]
 */
void
sprite_tracker_current_layout(FONTS_DATA_HANDLE data, unsigned int *x, unsigned int *y, unsigned int *z) {
    FontGroup *fg = (FontGroup *)data;

    *x = fg->sprite_tracker.xnum;
    *y = fg->sprite_tracker.ynum;
    *z = fg->sprite_tracker.z;
}

static inline
void free_sprite_positions(Font *font) {
    for (size_t i = 0; i < sizeof(font->sprite_map) / sizeof(font->sprite_map[0]); i++) {
        SpritePosition *s = font->sprite_map[i].next;
        while (s) {
            SpritePosition *t = s;
            s = s->next;
            free(t);
        }
    }
    memset(font->sprite_map, 0, sizeof(font->sprite_map));
}

static inline
void free_special_glyph_cache(Font *font) {
    for (size_t i = 0; i < sizeof(font->special_glyph_cache) / sizeof(font->special_glyph_cache[0]); i++) {
        SpecialGlyphCache *s = font->special_glyph_cache[i].next;
        while (s) {
            SpecialGlyphCache *t = s;
            s = s->next;
            free(t);
        }
    }
    memset(font->special_glyph_cache, 0, sizeof(font->special_glyph_cache));
}

/**
 * マップ群を解放する
 *  - SpritePosition
 *  - SpecialGlyphCache
 *
 * @param font フォント
 */
void
free_maps(Font *font) {
    free_sprite_positions(font);
    free_special_glyph_cache(font);
}

static inline
void clear_sprite_position_impl(SpritePosition *sp) {
    sp->filled = sp->rendered = sp->colored = false;
    sp->glyph = 0;
    zero_at_ptr(&sp->extra_glyphs);
    sp->x = sp->y = sp->z = 0;
    sp->ligature_index = 0;
}

/**
 * スプライトの配列をクリアする
 *
 * \param font フォント
 * \note これどこからも呼ばれてないって😠
 */
void
clear_sprite_map(Font *font) {
    for (size_t i = 0; i < sizeof(font->sprite_map) / sizeof(font->sprite_map[0]); i++) {
        for (SpritePosition *sp = &font->sprite_map[i]; sp != NULL; sp = sp->next) {
            clear_sprite_position_impl(sp);
        }
    }
}

/**
 * 特殊グリフキャッシュをクリアする
 *
 * \param font フォント
 * \note これどこからも呼ばれてないって😠
 */
void
clear_special_glyph_cache(Font *font) {
#define CLEAR(s) \
    s->data = 0; \
    s->glyph = 0;

    SpecialGlyphCache *sg;
    for (size_t i = 0; i < sizeof(font->special_glyph_cache) / sizeof(font->special_glyph_cache[0]); i++) {
        sg = font->special_glyph_cache + i;
        CLEAR(sg);
        while ((sg = sg->next)) {
            CLEAR(sg);
        }
    }

#undef CLEAR
}

/**
 * スプライト・トラッカーのレイアウトを設定する
 *
 * @param st スプライト・トラッカー
 * @param cell_width セル幅
 * @param cell_height セル高さ
 */
static void
sprite_tracker_set_layout(GPUSpriteTracker *st, unsigned int cell_width, unsigned int cell_height) {
    st->xnum = MIN(MAX(1u, max_texture_size / cell_width), (size_t)UINT16_MAX);
    st->max_y = MIN(MAX(1u, max_texture_size / cell_height), (size_t)UINT16_MAX);
    st->ynum = 1;
    st->x = 0;
    st->y = 0;
    st->z = 0;
}

// }}}

/**
 * フォントでスクリプターからCTFaceを生成する
 *
 * @param desc フォントデスクリプター
 * @param fg フォントデータハンドル(フォントグループ？)
 */
static inline PyObject *
desc_to_face(PyObject *desc, FONTS_DATA_HANDLE fg) {
    PyObject *d = specialize_font_descriptor(desc, fg);
    if (!d) {
        return NULL;
    }

    PyObject *ans = face_from_descriptor(d, fg);
    Py_DECREF(d);
    return ans;
}

/**
 * HarfBuzz機能をコピーする
 *
 * @param f フォント
 * @param which HarfBuzz機能
 */
static inline void
copy_hb_feature(Font *f, HBFeature which) {
    memcpy(f->hb_features + f->num_hb_features++, hb_features + which, sizeof(hb_features[0]));
}

/**
 * フォントの初期化
 *
 * @param f フォント
 * @param face CTFace (PyObject)
 * @param bold ボールド
 * @param italic イタリック
 * @param emoji_presentation 絵文字
 */
static inline bool
init_font(Font *f, PyObject *face, bool bold, bool italic, bool emoji_presentation) {
    f->face = face;
    Py_INCREF(f->face);
    f->bold = bold;
    f->italic = italic;
    f->emoji_presentation = emoji_presentation;
    f->num_hb_features = 0;

    // Ninbus フォントであれば強制的にリガチャを有効にする
    const char *psname = postscript_name_for_face(face);
    if (strstr(psname, "NimbusMonoPS-") == psname) {
        copy_hb_feature(f, LIGA_FEATURE);
        copy_hb_feature(f, DLIG_FEATURE);
    }
    copy_hb_feature(f, CALT_FEATURE);
    return true;
}

/**
 * フォントの解放
 *  CTFaceと各種マップを削除する
 *
 * @param f フォント
 */
static inline void
del_font(Font *f) {
    Py_CLEAR(f->face);
    free_maps(f);
    f->bold = false;
    f->italic = false;
}

/**
 * フォントグループの削除
 *
 * @param fg フォントグループ
 */
static inline void
del_font_group(FontGroup *fg) {
    // キャンバスの解放
    free(fg->canvas);
    fg->canvas = NULL;

    // スプライトマップの解放
    fg->sprite_map = free_sprite_map(fg->sprite_map);

    // フォントの解放
    for (size_t i = 0; i < fg->fonts_count; i++) {
        del_font(&fg->fonts[i]);
    }
    free(fg->fonts);
    fg->fonts = NULL;
}

/**
 * 全てのフォントグループの削除
 */
static inline void
free_font_groups(void) {
    if (font_groups) {
        for (size_t i = 0; i < num_font_groups; i++) {
            del_font_group(font_groups + i);
        }
        free(font_groups);
        font_groups = NULL;

        font_groups_capacity = 0;
        num_font_groups = 0;
    }
}

/**
 * GPUへ転送する
 *
 * @param fg フォントデータハンドル
 * @param x x (SpritePositionのxっぽい: 以下同様)
 * @param y y
 * @param z z
 * @param buf ピクセルバッファ (FontGroupのcanvasっぽい)
 */
static void
python_send_to_gpu(FONTS_DATA_HANDLE fg, unsigned int x, unsigned int y, unsigned int z, pixel *buf) {
    if (python_send_to_gpu_impl) {
        if (num_font_groups == 0) {
            fatal("Cannot call send to gpu with no font groups");
        }
        PyObject *ret = PyObject_CallFunction(
            python_send_to_gpu_impl,
            "IIIN",
            x,
            y,
            z,
            PyBytes_FromStringAndSize(
                (const char *)buf,
                sizeof(pixel) * fg->cell_width * fg->cell_height));
        if (!ret) {
            PyErr_Print();
        }
        else {
            Py_DECREF(ret);
        }
    }
}

/**
 * セル寸法を計算する
 *
 * @param fg フォントグループ
 */
static inline void
calc_cell_metrics(FontGroup *fg) {

    // ミディアムフォントで寸法を図る
    unsigned int cell_height, cell_width, baseline, underline_position, underline_thickness;
    cell_metrics(fg->fonts[fg->medium_font_idx].face,
                 &cell_width,
                 &cell_height,
                 &baseline,
                 &underline_position,
                 &underline_thickness);
    if (cell_width == 0) {
        fatal("Failed to calculate cell width for the specified font");
    }
    const unsigned int before_cell_height = cell_height;

    // セルの幅と高さにアプリケーション・オプションの設定値を反映する
    int cw = cell_width, ch = cell_height;
    if (OPT(adjust_line_height_px) != 0) {
        ch += OPT(adjust_line_height_px);
    }
    if (OPT(adjust_line_height_frac) != 0.f) {
        ch = (int)(ch * OPT(adjust_line_height_frac));
    }
    if (OPT(adjust_column_width_px != 0)) {
        cw += OPT(adjust_column_width_px);
    }
    if (OPT(adjust_column_width_frac) != 0.f) {
        cw = (int)(cw * OPT(adjust_column_width_frac));
    }

    // セルの幅高さをチェックする
#define MAX_DIM    1000
#define MIN_WIDTH  2
#define MIN_HEIGHT 4
    if (cw >= MIN_WIDTH && cw <= MAX_DIM) {
        cell_width = cw;
    }
    else {
        log_error("Cell width invalid after adjustment, ignoring adjust_column_width");
    }
    if (ch >= MIN_HEIGHT && ch <= MAX_DIM) {
        cell_height = ch;
    }
    else {
        log_error("Cell height invalid after adjustment, ignoring adjust_line_height");
    }
    int line_height_adjustment = cell_height - before_cell_height;
    if (cell_height < MIN_HEIGHT) {
        fatal("Line height too small: %u", cell_height);
    }
    if (cell_height > MAX_DIM) {
        fatal("Line height too large: %u", cell_height);
    }
    if (cell_width < MIN_WIDTH) {
        fatal("Cell width too small: %u", cell_width);
    }
    if (cell_width > MAX_DIM) {
        fatal("Cell width too large: %u", cell_width);
    }
#undef MIN_WIDTH
#undef MIN_HEIGHT
#undef MAX_DIM

    // アンダーライン位置を求める
    underline_position = MIN(cell_height - 1, underline_position);
    // スタイル付き下線をレンダリングするために利用可能なピクセルが少なくとも数個あることを確認する
    while (underline_position > baseline + 1 && cell_height - underline_position < 2) {
        underline_position--;
    }
    if (line_height_adjustment > 1) {
        baseline += MIN(cell_height - 1, (unsigned)line_height_adjustment / 2);
        underline_position += MIN(cell_height - 1, (unsigned)line_height_adjustment / 2);
    }

    // スプライトトラッカーの設定
    sprite_tracker_set_layout(&fg->sprite_tracker, cell_width, cell_height);

    // フォントグループの更新
    fg->cell_width = cell_width;
    fg->cell_height = cell_height;
    fg->baseline = baseline;
    fg->underline_position = underline_position;
    fg->underline_thickness = underline_thickness;

    // キャンバスの再割当
    // NOTE: 1行27文字のサイズしか確保してないぞ🤔
    free(fg->canvas);
    fg->canvas = calloc(fontgroup_get_canvas_byte_size(fg), sizeof(pixel));
    if (!fg->canvas) {
        fatal("Out of memory allocating canvas for font group");
    }
}

/**
 * CTFaceが特定のコードポイントを持っているか判定する
 *
 * @param face CTFace
 * @param cp 文字コード
 * @return 持っているなら真
 */
static inline bool
face_has_codepoint(PyObject *face, char_type cp) {
    return glyph_id_for_codepoint(face, cp) > 0;
}

/**
 * 絵文字表現かどうか判定する
 *
 *  - GPUセル幅が2
 *  - CPUセル保持のコードポイントが絵文字領域のもの
 *  - CPUセルのcc_idx[0] が VS15 ではない
 *      - VS15 は絵文字モノクロ表現
 *      - なので、ここではカラー絵文字かどうか判定していると言う事
 *      - むむむ、モノクロ絵文字は偽になるのか...
 *
 * @param cpu_cell CPUセル
 * @param gpu_cell GPUセル
 * @return 持っているなら真
 */
static inline bool
has_emoji_presentation(CPUCell *cpu_cell, GPUCell *gpu_cell) {
    return (gpu_cell->attrs & WIDTH_MASK) == 2 &&
            is_emoji(cpu_cell->ch) &&
            cpu_cell->cc_idx[0] != VS15;
}

/**
 * CPUセルが保持する文字コード集合がテキストかどうか判定する
 *
 * @param self フォント
 * @param cell CPUセル
 * @return 真偽値
 */
static inline bool
has_cell_text(Font *self, CPUCell *cell) {
    if (!face_has_codepoint(self->face, cell->ch)) {
        return false;
    }
    for (unsigned i = 0; i < arraysz(cell->cc_idx) && cell->cc_idx[i] != 0; i++) {
        const combining_type mark = cell->cc_idx[i];
        if (mark == VS15 || mark == VS16) { // 絵文字の異体字セレクタならスキップ
            continue;
        }
        if (!face_has_codepoint(self->face, codepoint_for_mark(mark))) {
            return false;
        }
    }
    return true;
}

/**
 * デバッグ出力
 *
 * @param cell CPUセル
 * @param bold ボールド
 * @param italic イタリック
 * @param emoji_presentation 絵文字
 * @param face CTFaceオブジェクト (PyObject)
 * @param new_face 新フェイスフラグ
 */
static inline void
output_cell_fallback_data(CPUCell *cell, bool bold, bool italic, bool emoji_presentation, PyObject *face, bool new_face) {
    printf("U+%x ", cell->ch);
    for (unsigned i = 0; i < arraysz(cell->cc_idx) && cell->cc_idx[i]; i++) {
        printf("U+%x ", codepoint_for_mark(cell->cc_idx[i]));
    }
    if (bold) {
        printf("bold ");
    }
    if (italic) {
        printf("italic ");
    }
    if (emoji_presentation) {
        printf("emoji_presentation ");
    }
    PyObject_Print(face, stdout, 0);
    if (new_face) {
        printf(" (new face)");
    }
    printf("\n");
}

/**
 * フォールバックフォントをロードする
 *
 * @param fg フォントグループ
 * @param cell CPUセル
 * @param bold ボールド
 * @param italic イタリック
 * @param emoji_presentation 絵文字
 * @return フォールバックフォントのインデックス
 */
static inline ssize_t
load_fallback_font(FontGroup *fg, CPUCell *cell, bool bold, bool italic, bool emoji_presentation) {
    // フォールバックフォント数は100まで
    if (fg->fallback_fonts_count > 100) {
        log_error("Too many fallback fonts");
        return MISSING_FONT;
    }

    // フォントのインデックスを設定する
    ssize_t f;
    if (bold) {
        f = fg->italic_font_idx > 0 ? fg->bi_font_idx : fg->bold_font_idx;
    }
    else {
        f = italic ? fg->italic_font_idx : fg->medium_font_idx;
    }
    if (f < 0) {
        f = fg->medium_font_idx;
    }

    // フォールバックフォントを生成する
    PyObject *face = create_fallback_face(fg->fonts[f].face, cell, bold, italic, emoji_presentation, (FONTS_DATA_HANDLE)fg);
    if (!face) {
        PyErr_Print();
        return MISSING_FONT;
    }
    if (face == Py_None) {
        Py_DECREF(face);
        return MISSING_FONT;
    }

    // フラグが立っている時はデバッグ出力する
    if (global_state.debug_font_fallback) {
        output_cell_fallback_data(cell, bold, italic, emoji_presentation, face, true);
    }

    // フォントのポイントサイズを設定する
    set_size_for_face(face, fg->cell_height, true, (FONTS_DATA_HANDLE)fg);

    // フォントグループの領域を確保する
    ensure_space_for(fg, fonts, Font, fg->fonts_count + 1, fonts_capacity, 5, true);
    const ssize_t ans = fg->first_fallback_font_idx + fg->fallback_fonts_count;

    // フォントを初期化する
    Font *af = &fg->fonts[ans];
    if (!init_font(af, face, bold, italic, emoji_presentation)) {
        fatal("Out of memory");
    }
    Py_DECREF(face);

    // フォントがテキストに対するグリフを持っていない場合(？)はデバッグ出力する
    if (!has_cell_text(af, cell)) {
        if (global_state.debug_font_fallback) {
            printf("The font chosen by the OS for the text: ");
            printf("U+%x ", cell->ch);
            for (unsigned i = 0; i < arraysz(cell->cc_idx) && cell->cc_idx[i] != 0; i++) {
                printf("U+%x ", codepoint_for_mark(cell->cc_idx[i]));
            }
            printf("is ");
            PyObject_Print(af->face, stdout, 0);
            printf(" but it does not actually contain glyphs for that text\n");
        }
        del_font(af);
        return MISSING_FONT;
    }
    fg->fallback_fonts_count++;
    fg->fonts_count++;
    return ans;
}

/**
 * フォールバックする
 *
 * @param fg フォントグループ
 * @param cpu_cell CPUセル
 * @param gpu_cell GPUセル
 * @return フォールバックフォントのインデックス
 */
static inline ssize_t
fallback_font(FontGroup *fg, CPUCell *cpu_cell, GPUCell *gpu_cell) {
    const bool bold = (gpu_cell->attrs >> BOLD_SHIFT) & 1;
    const bool italic = (gpu_cell->attrs >> ITALIC_SHIFT) & 1;
    const bool emoji_presentation = has_emoji_presentation(cpu_cell, gpu_cell);

    // 既存の代替フォントにこのテキストがあるかどうかを確認します
    for (size_t i = 0, j = fg->first_fallback_font_idx; i < fg->fallback_fonts_count; i++, j++) {
        Font *font = &fg->fonts[j];
        if (font->bold == bold &&
            font->italic == italic &&
            font->emoji_presentation == emoji_presentation &&
            has_cell_text(font, cpu_cell)) {
            if (global_state.debug_font_fallback) {
                output_cell_fallback_data(cpu_cell, bold, italic, emoji_presentation, font->face, false);
            }
            return j;
        }
    }

    return load_fallback_font(fg, cpu_cell, bold, italic, emoji_presentation);
}

/**
 * 文字コードを含む記号マップ(?)を得る
 *
 * @param fg フォントグループ
 * @param ch 文字コード
 * @return 記号マップのインデックス
 */
static inline ssize_t
in_symbol_maps(FontGroup *fg, char_type ch) {
    for (size_t i = 0; i < num_symbol_maps; i++) {
        if (symbol_maps[i].left <= ch && ch <= symbol_maps[i].right) {
            return fg->first_symbol_font_idx + symbol_maps[i].font_idx;
        }
    }
    return NO_FONT;
}

/**
 * 特定のセルに使用するフォントを決定します。
 *
 * @param fg フォントグループ
 * @param cpu_cell CPUセル
 * @param gpu_cell GPUセル
 * @param is_fallback_font フォールバックフォントか？
 * @param is_emoji_presentation 絵文字か？
 * @return 結果コード
 *     - NO_FONT
 *     - MISSING_FONT
 *     - BLANK_FONT
 *     - BOX_FONT
 *     - フォンのインデックス
 */
static inline ssize_t
font_for_cell(
        FontGroup *fg,
        CPUCell *cpu_cell,
        GPUCell *gpu_cell,
        bool *is_fallback_font,
        bool *is_emoji_presentation
) {
    *is_fallback_font = false;
    *is_emoji_presentation = false;

    START_ALLOW_CASE_RANGE
    ssize_t ans;
    switch (cpu_cell->ch) {
        case 0:
        case ' ':
        case '\t':
            return BLANK_FONT; // ブランクフォント
        case 0x2500 ... 0x2573:
        case 0x2574 ... 0x259f:
        case 0xe0b0 ... 0xe0b4:
        case 0xe0b6:
        case 0xe0b8: // 
        case 0xe0ba: //   
        case 0xe0bc: // 
        case 0xe0be: //   
            return BOX_FONT; // ボックスフォント
        default:
            ans = in_symbol_maps(fg, cpu_cell->ch);
            if (ans > -1) {
                return ans;
            }
            switch (BI_VAL(gpu_cell->attrs)) {
                case 0:
                    ans = fg->medium_font_idx;
                    break;
                case 1:
                    ans = fg->bold_font_idx;
                    break;
                case 2:
                    ans = fg->italic_font_idx;
                    break;
                case 3:
                    ans = fg->bi_font_idx;
                    break;
            }
            if (ans < 0) {
                ans = fg->medium_font_idx;
            }

            // 絵文字かどうか
            *is_emoji_presentation = has_emoji_presentation(cpu_cell, gpu_cell);
            if (!*is_emoji_presentation && has_cell_text(fg->fonts + ans, cpu_cell)) {
                return ans;
            }

            // ここに来た場合はフォールバック確定
            *is_fallback_font = true;
            return fallback_font(fg, cpu_cell, gpu_cell);
    }
    END_ALLOW_CASE_RANGE
}

/**
 * スプライトを設定する
 *
 * @param cell CPUセル
 * @param x スプライト位置x
 * @param y スプライト位置y
 * @param z スプライト位置z
 */
static inline void
set_sprite(GPUCell *cell, sprite_index x, sprite_index y, sprite_index z) {
    cell->sprite_x = x;
    cell->sprite_y = y;
    cell->sprite_z = z;
}

/**
 * ボックスグリフに一意の（任意の）IDを与える
 *
 * @param ch 文字コード
 * @return グリフインデックス
 */
static inline glyph_index
box_glyph_id(char_type ch) {
    START_ALLOW_CASE_RANGE
    switch (ch) {
        case 0x2500 ... 0x259f:
            return ch - 0x2500; // 0x00 ~ 0x9f
        case 0xe0b0 ... 0xe0d4:
            return 0xa0 + ch - 0xe0b0; // 0xa0 ~ 0xc4
        default:
            return 0xff;
    }
    END_ALLOW_CASE_RANGE
}

static PyObject *box_drawing_function = NULL;
static PyObject *prerender_function = NULL;
static PyObject *descriptor_for_idx = NULL;

/**          
 * アルファマスクをレンダリングする
 *
 *  alpha_maskとdestをストライドに従ってマスクしているだけ
 *
 * @param alpha_mask アルファマスク(バイト配列)
 * @param dest 出力先ピクセルバッファ
 * @param src_rect 転送元の矩形
 * @param dest_rect 転送後の矩形
 * @param src_stride 転送元のストライド
 * @param dest_stride 転送後のストライド
 */
void
render_alpha_mask(
        uint8_t *alpha_mask,
        pixel *dest,
        Region *src_rect,
        Region *dest_rect,
        size_t src_stride,
        size_t dest_stride
) {
    for (size_t sy = src_rect->top, dy = dest_rect->top;
         sy < src_rect->bottom && dy < dest_rect->bottom;
         sy++, dy++) {
        pixel *dp = dest + dest_stride * dy;
        uint8_t *sp = alpha_mask + src_stride * sy;
        for (size_t sx = src_rect->left, dx = dest_rect->left;
             sx < src_rect->right && dx < dest_rect->right;
             sx++, dx++) {
            const pixel val = dp[dx];
            const uint8_t alpha = sp[sx];
            dp[dx] = 0xffffff00 | MIN(0xffu, alpha + (val & 0xff));
        }
    }
}

/**
 * ボックスセル（？）をレンダリングする
 *
 * @param fg フォントグループ
 * @param cpu_cell CPUセル
 * @param gpu_cell GPUセル
 */
static void
render_box_cell(FontGroup *fg, CPUCell *cpu_cell, GPUCell *gpu_cell) {
    int error = 0;

    // ボックスグリフのインデックスを得る
    glyph_index glyph = box_glyph_id(cpu_cell->ch);

    // スプライト位置とエクストラグリフを得る
    static ExtraGlyphs extra_glyphs = {{0}};
    SpritePosition *sp = sprite_position_for(fg, &fg->fonts[BOX_FONT], glyph, &extra_glyphs, false, &error);
    if (!sp) {
        sprite_map_set_error(error);
        PyErr_Print();
        set_sprite(gpu_cell, 0, 0, 0);
        return;
    }

    // スプライト位置を設定する
    set_sprite(gpu_cell, sp->x, sp->y, sp->z);
    if (sp->rendered) {
        return;
    }

    // レンダリング済にする
    sp->rendered = true;
    sp->colored = false;

    // ボックスグリフを描画する
    PyObject *ret = PyObject_CallFunction(
            box_drawing_function,
            "IIId",
            cpu_cell->ch,
            fg->cell_width,
            fg->cell_height,
            (fg->logical_dpi_x + fg->logical_dpi_y) / 2.0);
    if (!ret) {
        PyErr_Print();
        return;
    }

    // アルファマスクをレンダリングする
    uint8_t *alpha_mask = PyLong_AsVoidPtr(PyTuple_GET_ITEM(ret, 0));
    clear_canvas(fg);
    Region region = {
        .right = fg->cell_width,
        .bottom = fg->cell_height
    };
    render_alpha_mask(alpha_mask, fg->canvas, &region, &region, fg->cell_width, fg->cell_width);

    // スプライト(fg->canvas)をGPUに転送する
    current_send_sprite_to_gpu((FONTS_DATA_HANDLE)fg, sp->x, sp->y, sp->z, fg->canvas);

    Py_DECREF(ret);
}

/**
 * HarfBuzzバッファをロードする
 *
 * \param cpu_cell CPUセルの配列
 * \param first_gpu_cell GPUセルの配列
 * \param num_cells セル数
 */
static inline void
load_hb_buffer(CPUCell *cpu_cell, GPUCell *gpu_cell, index_type num_cells) {
    // HarfBuzzバッファをクリアする
    hb_buffer_clear_contents(harfbuzz_buffer);

    while (num_cells != 0) {
        attrs_type prev_width = 0;

        // shape_bufferに文字コードポイントを埋めていく
        index_type num;
        for (num = 0;
             num_cells != 0 && num < arraysz(shape_buffer) - 20 - arraysz(cpu_cell->cc_idx); // TODO 20って何？
             cpu_cell++, gpu_cell++, num_cells--) {

            // 直前の文字幅が2なら shape_buffer に入れない
            if (prev_width == 2) {
                prev_width = 0;
                continue;
            }

            // shape_buffer にコードポイントを詰める
            shape_buffer[num++] = cpu_cell->ch;

            // 文字幅を保持しておく
            prev_width = gpu_cell->attrs & WIDTH_MASK;

            // 結合文字も shape_buffer にいれる
            // - cc_idxには内部インデックスで格納されている
            // - なのでコードポイントに変換してからいれる
            for (unsigned int i = 0; i < arraysz(cpu_cell->cc_idx) && cpu_cell->cc_idx[i]; i++) {
                const combining_type mark = cpu_cell->cc_idx[i];
                shape_buffer[num++] = codepoint_for_mark(mark);
            }
        }

        // HarfBuzzバッファをまるっと shape_buffer で置換する
        hb_buffer_add_utf32(harfbuzz_buffer, shape_buffer, num, 0, num);
    }

    // バッファのUnicode内容に基づいて、未設定のバッファーセグメントプロパテ
    // ィを設定する
    hb_buffer_guess_segment_properties(harfbuzz_buffer);
}

/**
 * GPUセルのスプライト情報を設定する
 *
 * @param cell GPUセル
 * @param sp スプライト位置
 */
static inline void
set_cell_sprite(GPUCell *cell, SpritePosition *sp) {
    cell->sprite_x = sp->x;
    cell->sprite_y = sp->y;
    cell->sprite_z = sp->z;
    if (sp->colored) {
        cell->sprite_z |= 0x4000; // 謎のマスク
    }
}

/**
 * キャンバスからセルを展開する
 *
 * @param fg フォントグループ
 * @param i セルの位置
 * @param num_cells セルの個数
 * @return 展開したピクセルバッファ
 */
static inline pixel *
extract_cell_from_canvas(FontGroup *fg, unsigned int i, unsigned int num_cells) {
    // キャンバス配列の末尾に展開する
    pixel *tail = fontgroup_get_canvas_tail(fg);
    pixel *dest = tail;
    const pixel *src = fontgroup_get_canvas_at(fg, i);
    const unsigned int stride = fg->cell_width * num_cells;

    for (unsigned int y = 0; y < fg->cell_height; y++, dest += fg->cell_width, src += stride) {
        memcpy(dest, src, fg->cell_width * sizeof(pixel));
    }
    return tail;
}

/**
 * グループをレンダリングする
 *
 * @param fg フォントグループ
 * @param num_cells セルの個数
 * @param num_glyphs グリフの個数
 * @param cpu_cells CPUセルの配列
 * @param gpu_cells GPUセルの配列
 * @param info HarfBuzzグリフ情報
 * @param positions HarfBuzz位置情報
 * @param font フォント情報
 * @param index グリフのインデックス
 * @param extra_glyphs エクストラグリフ
 * @param center_glyph レンダリング位置を中央にする
 */
static inline void
render_group(
        FontGroup *fg,
        unsigned int num_cells,
        unsigned int num_glyphs,
        CPUCell *cpu_cells,
        GPUCell *gpu_cells,
        hb_glyph_info_t *info,
        hb_glyph_position_t *positions,
        Font *font,
        glyph_index glyph,
        ExtraGlyphs *extra_glyphs,
        bool center_glyph
) {
    static SpritePosition *sprite_position[16]; // なんでstatic... なんで16

    // スプライト位置を求めて sprite_positionに格納する
    num_cells = MIN(arraysz(sprite_position), num_cells);
    for (unsigned int i = 0; i < num_cells; i++) {
        int error = 0;
        sprite_position[i] = sprite_position_for(fg, font, glyph, extra_glyphs, (uint8_t)i, &error);
        if (error != 0) {
            sprite_map_set_error(error);
            PyErr_Print();
            return;
        }
    }

    // セル内のスプライト位置を更新する
    if (sprite_position[0]->rendered) {
        for (unsigned int i = 0; i < num_cells; i++) {
            set_cell_sprite(&gpu_cells[i], sprite_position[i]);
        }
        return;
    }

    // キャンバスの(ゼロ)クリア
    clear_canvas(fg);

    // 絵文字の判定
    bool was_colored = (gpu_cells->attrs & WIDTH_MASK) == 2 && is_emoji(cpu_cells->ch);

    /*
     * グリフをレンダリングする
     *  freetype/CoreText各々の実装に分岐する
     *  出力結果はビットマップとして fg->canvas に格納される
     */
    render_glyphs_in_cells(font->face,
                           font->bold,
                           font->italic,
                           info,
                           positions,
                           num_glyphs,
                           fg->canvas,
                           fg->cell_width,
                           fg->cell_height,
                           num_cells,
                           fg->baseline,
                           &was_colored,
                           (FONTS_DATA_HANDLE)fg,
                           center_glyph);
    if (PyErr_Occurred()) {
        PyErr_Print();
    }

    // スプライト情報を更新してGPUに転送する
    for (unsigned int i = 0; i < num_cells; i++) {
        sprite_position[i]->rendered = true;
        sprite_position[i]->colored = was_colored;
        set_cell_sprite(&gpu_cells[i], sprite_position[i]);

        // セルがn個の場合はcanvasを展開する (なんで？)
        pixel *p = num_cells == 1 ?
            fg->canvas :
            extract_cell_from_canvas(fg, i, num_cells);

        // スプライト(p)をGPUに転送する
        current_send_sprite_to_gpu((FONTS_DATA_HANDLE)fg,
                                   sprite_position[i]->x,
                                   sprite_position[i]->y,
                                   sprite_position[i]->z,
                                   p);
    }
}

#pragma mark -

/**
 * セルデータ構造体
 */
typedef struct {
    CPUCell *cpu_cell; /** CPUセルの配列 */
    GPUCell *gpu_cell; /** GPUセルの配列 */
    unsigned int num_codepoints; /** コードポイントの数 */
    unsigned int codepoints_consumed; /** 消費したコードポイントの数 TODO: 謎 */
    char_type current_codepoint; /** 現在のコードポイント */
} CellData;

/**
 * グループ構造体
 *  ほぼほぼラン
 */
typedef struct {
    /** 先頭グリフのインデックス */
    unsigned int first_glyph_idx;

    /** 先頭セルのインデックス */
    unsigned int first_cell_idx;

    /** グリフの個数 */
    unsigned int num_glyphs;

    /** セルの個数 */
    unsigned int num_cells;

    /** 特殊グリフを持っているか */
    bool has_special_glyph;

    /** 私用領域リガチャか */
    bool is_space_ligature;
} Group;

/**
 * グループにおける最大グリフ数
 */
#define MAX_GLYPHS_IN_GROUP (MAX_EXTRA_GLYPHS + 1u)

/**
 * グループ状態構造体
 *  レイアウト処理のコンテキスト
 *  fonts.cの shape 関数が呼ばれる都度、初期化される
 *  レイアウトの中間結果を保持する器
 *  レイアウト処理が終わったあとも参照される
 */
typedef struct {
    /**
     * 直前は特殊グリフだったか
     */
    bool prev_was_special;

    /**
     * 直前は空グリフだったか
     */
    bool prev_was_empty;

    /**
     * カレントのセルデータ
     */
    CellData current_cell_data;

    /**
     * グループの配列
     */
    Group *groups;

    /**
     * グループの配列のサイズ
     */
    size_t groups_capacity;

    /**
     * カレントのグループのインデックス
     *  shape 関数が終わったら後は、有効なグループの個数を示す
     */
    size_t group_idx;

    /**
     * グリフのインデックス
     */
    size_t glyph_idx;

    /**
     * セルのインデックス
     */
    size_t cell_idx;

    /**
     * render_run関数で指定された num_cells
     *  ランに含まれるセルの数？
     */
    size_t num_cells;

    /**
     * infoの件数
     */
    size_t num_glyphs;

    /**
     * 先頭のCPUセル
     */
    CPUCell *first_cpu_cell;

    /**
     * 末尾のCPUセル
     */
    CPUCell *last_cpu_cell;

    /**
     * 先頭のGPUセル
     */
    GPUCell *first_gpu_cell;

    /**
     * 末尾のGPUセル
     */
    GPUCell *last_gpu_cell;

    /**
     * HarfBuzzグリフ情報
     *  hb_buffer_get_glyph_infos の戻り値
     */
    hb_glyph_info_t *info;

    /**
     * HarfBuzz位置情報
     *  hb_buffer_get_glyph_positionsの戻り値
     */
    hb_glyph_position_t *positions;
} GroupState;

static inline
Group* group_state_move_glyph_to_next_group(GroupState* state, Group *group) {
    const size_t start_cell_idx = state->cell_idx;
    group->num_glyphs--;

    state->group_idx++;

    Group *next_group = &state->groups[state->group_idx];
    next_group->first_cell_idx = start_cell_idx;
    next_group->num_glyphs = 1;
    next_group->first_glyph_idx = state->glyph_idx;
    return next_group;
}

/**
 * グループ状態
 *  何故にstatic
 */
static GroupState group_state = {0};

/**
 * セル中のコードポイントを数える
 *
 * \param[in] cell CPUセル
 * \return コードポイントの個数
 */
static inline unsigned int
num_codepoints_in_cell(CPUCell *cell) {
    unsigned n = 1; // `ch` メンバがあるので無条件に+1
    for (unsigned i = 0; i < arraysz(cell->cc_idx) && cell->cc_idx[i] != 0; i++) {
        n++;
    }
    return n;
}

/**
 * テキストをレイアウトする
 *
 * HarfBuzz用語では `shaping` というため、この関数名になっている模様。
 *
 * \param first_cpu_cell 先頭CPUセル
 * \param first_gpu_cell 先頭GPUセル
 * \param num_cells セルの個数
 * \param font HarfBuzzフォント
 * \param fobj フォント
 * \param disable_ligature リガチャ無効
 */
static inline void
shape(
    CPUCell *first_cpu_cell,
    GPUCell *first_gpu_cell,
    index_type num_cells,
    hb_font_t *font,
    Font *fobj,
    bool disable_ligature
) {
    // キャパシティが充足しているかどうか確認する
    if (group_state.groups_capacity <= 2 * num_cells) {
        group_state.groups_capacity = MAX(128u, 2 * num_cells);  // avoid unnecessary reallocs
        group_state.groups = realloc(group_state.groups, sizeof(Group) * group_state.groups_capacity);
        if (!group_state.groups) {
            fatal("Out of memory");
        }
    }

    // グループ状態の初期化
    group_state.prev_was_special = false;
    group_state.prev_was_empty = false;
    group_state.current_cell_data.cpu_cell = first_cpu_cell;
    group_state.current_cell_data.gpu_cell = first_gpu_cell;
    group_state.current_cell_data.num_codepoints = num_codepoints_in_cell(first_cpu_cell);
    group_state.current_cell_data.codepoints_consumed = 0;
    group_state.current_cell_data.current_codepoint = first_cpu_cell->ch;
    zero_at_ptr_count(group_state.groups, group_state.groups_capacity);
    group_state.group_idx = 0;
    group_state.glyph_idx = 0;
    group_state.cell_idx = 0;
    group_state.num_cells = num_cells;
    group_state.first_cpu_cell = first_cpu_cell;
    group_state.first_gpu_cell = first_gpu_cell;
    group_state.last_cpu_cell = first_cpu_cell + (num_cells ? num_cells - 1 : 0);
    group_state.last_gpu_cell = first_gpu_cell + (num_cells ? num_cells - 1 : 0);

    // HarfBuzzバッファのロード - staticな `harfbuzz_buffer` にCPUセルの文字コードが埋められる
    load_hb_buffer(first_cpu_cell, first_gpu_cell, num_cells);

    // レイアウトする
    hb_shape(font, harfbuzz_buffer, fobj->hb_features, fobj->num_hb_features - (disable_ligature ? 0 : 1));

    // HarfBuzzからグリフ情報とグリフ位置を取り出す
    unsigned int info_length, positions_length;
    group_state.info = hb_buffer_get_glyph_infos(harfbuzz_buffer, &info_length);
    group_state.positions = hb_buffer_get_glyph_positions(harfbuzz_buffer, &positions_length);
    if (!group_state.info || !group_state.positions) {
        group_state.num_glyphs = 0;
    }
    else {
        group_state.num_glyphs = MIN(info_length, positions_length);
    }
}

/**
 * 特殊グリフかどうか
 *
 *  グリフは、対応するコードポイントがフォント内の別のグリフと一致する場合に特殊扱いされる
 *
 * @param glyph_index グリフインデックス
 * @param font フォントオブジェクト
 * @param cell_data セルデータ
 * @return 真偽値
 */
static inline bool
is_special_glyph(glyph_index glyph_id, Font *font, CellData *cell_data) {
    SpecialGlyphCache *s = special_glyph_cache_for(font, glyph_id, SPECIAL_FILLED_MASK);
    if (!s) {
        return false;
    }

    if (!(s->data & SPECIAL_FILLED_MASK)) {
        const bool is_special =
            cell_data->current_codepoint ?
                (glyph_id != glyph_id_for_codepoint(font->face, cell_data->current_codepoint)) :
                false;
        const uint8_t val = is_special ? SPECIAL_VALUE_MASK : 0;
        s->data |= val | SPECIAL_FILLED_MASK;
    }
    return s->data & SPECIAL_VALUE_MASK;
}

/**
 * 空のグリフかどうか判定する
 *
 *   - メトリックの幅がゼロの場合、グリフを空とする
 *   - 空のグリフは特殊グリフである
 *
 * @param glyph_index グリフインデックス
 * @param font フォントオブジェクト
 * @return 真偽値
 */
static inline bool
is_empty_glyph(glyph_index glyph_id, Font *font) {
    SpecialGlyphCache *s = special_glyph_cache_for(font, glyph_id, EMPTY_FILLED_MASK);
    if (s == NULL) {
        return false;
    }

    if (!(s->data & EMPTY_FILLED_MASK)) {
        const uint8_t val = is_glyph_empty(font->face, glyph_id) ? EMPTY_VALUE_MASK : 0;
        s->data |= val | EMPTY_FILLED_MASK;
    }
    return s->data & EMPTY_VALUE_MASK;
}

/**
 * コードポイントを消費して、必要ならスロットを空ける
 *
 * \param cell_data セルデータ
 * \param last_cpu_cell 最後のCPUセル
 * \return スロットを増やした数
 */
static inline unsigned int
check_cell_consumed(CellData *cell_data, CPUCell *last_cpu_cell) {

    // コードポイントを消費する
    cell_data->codepoints_consumed++;

    if (cell_data->codepoints_consumed >= cell_data->num_codepoints) {

        // 文字幅を得る
        const attrs_type width = cell_data->gpu_cell->attrs & WIDTH_MASK;

        // CPU/GPUセル(配列)のスロットを増やす
        cell_data->cpu_cell += MAX(1, width);
        cell_data->gpu_cell += MAX(1, width);

        // 消費カウンタをクリアする
        cell_data->codepoints_consumed = 0;

        if (cell_data->cpu_cell <= last_cpu_cell) {

            // コードポイントを数え上げる
            cell_data->num_codepoints = num_codepoints_in_cell(cell_data->cpu_cell);

            // カレントのコードポイントを設定する
            cell_data->current_codepoint = cell_data->cpu_cell->ch;
        }
        else {
            cell_data->current_codepoint = 0;
        }
        return width;
    }
    else {
        switch (cell_data->codepoints_consumed) {
            case 0:
                cell_data->current_codepoint = cell_data->cpu_cell->ch;
                break;
            default: {
                // 結合記号のマーク(インデックス値)を得る
                const index_type mark = cell_data->cpu_cell->cc_idx[cell_data->codepoints_consumed - 1];
                // VS15/16(絵文字の異体字セレクタ)は、特殊なグリフとしてマークされており、レンダリング
                // を中断させるため、0にマップして、それを回避します
                cell_data->current_codepoint =
                    (mark == VS15 || mark == VS16) ? 0 : codepoint_for_mark(mark);
                break;
            }
        }
    }
    return 0;
}

/**
 * ランをレイアウトする
 *  group_stateが更新される
 *
 * @param first_cpu_cell 先頭CPUセル
 * @param first_gpu_cell 先頭GPUセル
 * @param num_cells セルの個数
 * @param font フォント
 * @param disable_ligature リガチャ無効
 */
static inline void
shape_run(
    CPUCell *first_cpu_cell,
    GPUCell *first_gpu_cell,
    index_type num_cells,
    Font *font,
    bool disable_ligature
) {
    // レイアウトする - group_state のメンバが埋められる
    shape(first_cpu_cell, first_gpu_cell, num_cells, harfbuzz_font_for_face(font->face), font, disable_ligature);

    /*
     * グリフをセルのグループに分配する
     *
     * 留意すべき考慮事項：
     *
     * 最高のパフォーマンスを得るには、グループのサイズをできるだけ小さくする必
     * 要があります。
     *
     * 文字を結合すると、複数のグリフが単一のセルにレンダリングされる可能性があ
     * ります。
     *
     * 絵文字と東アジアのワイド文字により、単一のグリフが複数のセルにレンダリン
     * グされる可能性があります。
     *
     * リガチャフォントは、2つの一般的なアプローチを取ります。
     *  1. ABC は EMPTY、EMPTY、WIDE GLYPH になります。これは、N個のセルにN個の
     *     グリフをレンダリングする必要があることを意味します（例えば Fira Code）
     *  2. ABC は WIDE GLYPHになります。これは、N個のセルに1つのグリフをレンダ
     *     リングすることを意味します（例：Operator Mono Lig）
     *
     * harfbuzzのクラスター番号に基づいて、グリフが対応するUnicodeコードポイン
     * トの数を確認します。
     * 次に、グリフが合字グリフ（is_special_glyph）であり、空のグリフであるかど
     * うかを確認します。
     * この3つデータポイントは、さまざまなフォントについて、上記の制約を満たす
     * のに十分な情報を提供します。
     */
#define G(x) (group_state.x)

    while (G(glyph_idx) < G(num_glyphs) && G(cell_idx) < G(num_cells)) {

        // HarfBuzzバッファからグリフIDとクラスタを得る
        glyph_index glyph_id = G(info)[G(glyph_idx)].codepoint;
        const uint32_t cluster = G(info)[G(glyph_idx)].cluster;

        // 特殊グリフか
        const bool is_special = is_special_glyph(glyph_id, font, &G(current_cell_data));

        // 空グリフか
        const bool is_empty = is_special && is_empty_glyph(glyph_id, font);

        // グリフに割当たっているコードポイントの件数
        uint32_t num_codepoints_used_by_glyph = 0;

        // 最後のグリフかどうか
        const bool is_last_glyph = G(glyph_idx) == G(num_glyphs) - 1;

        // カレントグループを得る
        Group *current_group = G(groups) + G(group_idx);

        if (is_last_glyph) {
            num_codepoints_used_by_glyph = UINT32_MAX;
        }
        else {
            const uint32_t next_cluster = G(info)[G(glyph_idx) + 1].cluster;
            // アラビア語のようなRTL言語はクラスタ番号が減少していく
            if (next_cluster != cluster) {
                num_codepoints_used_by_glyph =
                    cluster > next_cluster ? cluster - next_cluster : next_cluster - cluster;
            }
        }

        // 現在のグループに追加できるかどうか判定する
        // カレントが1つもグリフ持っていないなら追加できる
        bool add_to_current_group;
        if (current_group->num_glyphs == 0) {
            add_to_current_group = true;
        }
        else {
            // カレントグリフが特殊文字で直前が空グリフであれば追加できる
            if (is_special) {
                add_to_current_group = G(prev_was_empty);
            }
            else {
                // 直前が特殊文字であれば追加できる
                add_to_current_group = !G(prev_was_special);
            }
        }

        // 保有グリフ可能数を超えたら無理
        if (current_group->num_glyphs >= MAX_GLYPHS_IN_GROUP ||
            current_group->num_cells >= MAX_GLYPHS_IN_GROUP) {
            add_to_current_group = false;
        }

        // グループに追加できない場合は次のグループに移動する
        if (!add_to_current_group) {
            G(group_idx)++;
            current_group = G(groups) + G(group_idx);
        }

        if (0 == current_group->num_glyphs++) {
            current_group->first_glyph_idx = G(glyph_idx);
            current_group->first_cell_idx = G(cell_idx);
        }

        if (is_special) {
            current_group->has_special_glyph = true;
        }
        if (is_last_glyph) {

            // 残りのすべてのセルを吸収する
            if (G(cell_idx) < G(num_cells)) {

                // 空きスロット数
                const unsigned int slots = G(num_cells) - G(cell_idx);

                // グリフを次のグループに移動する
                if (current_group->num_cells + slots > MAX_GLYPHS_IN_GROUP) {
                    current_group = group_state_move_glyph_to_next_group(&group_state, current_group);
                }
                current_group->num_cells += slots;

                if (current_group->num_cells > MAX_GLYPHS_IN_GROUP) {
                    current_group->num_cells = MAX_GLYPHS_IN_GROUP;
                }
                G(cell_idx) += slots;
            }
        }
        else {
            unsigned int num_cells_consumed = 0, start_cell_idx = G(cell_idx);
            while (num_codepoints_used_by_glyph && G(cell_idx) < G(num_cells)) {
                unsigned int w = check_cell_consumed(&G(current_cell_data), G(last_cpu_cell));
                G(cell_idx) += w;
                num_cells_consumed += w;
                num_codepoints_used_by_glyph--;
            }
            if (num_cells_consumed) {
                if (num_cells_consumed > MAX_GLYPHS_IN_GROUP) {
                    // 厄介。1つのグリフがMAX_GLYPHS_IN_GROUPより多くのセルを使
                    // 用しているため、このケースは正しくレンダリングでき無い
                    log_error("The glyph: %u needs more than %u cells, cannot render it", glyph_id, MAX_GLYPHS_IN_GROUP);
                    current_group->num_glyphs--;
                    while (num_cells_consumed) {
                        G(group_idx)++;
                        current_group = G(groups) + G(group_idx);
                        current_group->num_glyphs = 1;
                        current_group->first_glyph_idx = G(glyph_idx);
                        current_group->num_cells = MIN(num_cells_consumed, MAX_GLYPHS_IN_GROUP);
                        current_group->first_cell_idx = start_cell_idx;
                        start_cell_idx += current_group->num_cells;
                        num_cells_consumed -= current_group->num_cells;
                    }
                }
                else {
                    // グリフを次のグループに移動する
                    if (num_cells_consumed + current_group->num_cells > MAX_GLYPHS_IN_GROUP) {
                        current_group = group_state_move_glyph_to_next_group(&group_state, current_group);
                    }
                    current_group->num_cells += num_cells_consumed;
                    if (!is_special) {  // リガチャではない、グループの末端。
                        G(group_idx)++;
                        current_group = G(groups) + G(group_idx);
                    }
                }
            }
        }

        G(prev_was_special) = is_special;
        G(prev_was_empty) = is_empty;
        G(glyph_idx)++;
    }
}

/**
 * 私用領域(Private Use Area: PUA)リガチャのためにグループをマージする
 */
static inline void
merge_groups_for_pua_space_ligature(void) {
    // グループ配列を前詰する
    GroupState *gs = &group_state;
    while (gs->group_idx > 0) {
        Group *g0 = &gs->groups[0];
        Group *g1 = &gs->groups[1];

        // g1の内容をg0に加える
        g0->num_cells += g1->num_cells;
        g0->num_glyphs += g1->num_glyphs;
        g0->num_glyphs = MIN(g0->num_glyphs, MAX_EXTRA_GLYPHS + 1);
        gs->group_idx--;
    }
    gs->groups->is_space_ligature = true;
}

/**
 * 指定されたオフセット上のランが分割可能ならそのランの範囲を得る
 *
 *  以下の条件ならランを分割できる:
 *  - セルが2個以上あり
 *  - 特殊グリフを含有しており
 *  - ランの先頭セルの文字幅が1
 *
 * \param[in] offset オフセット
 * \param[in] left ランの開始位置 [out]
 * \param[in] right ランの終了位置 [out]
 */
static inline void
split_run_at_offset(index_type offset, index_type *left, index_type *right) {
    // ランが見つからない場合は0を返す
    *left = 0;
    *right = 0;

    // 有効な全てのグループを舐める
    for (unsigned i = 0; i < group_state.group_idx + 1; i++) {
        // offsetがグループのセルインデックス範囲に含まれるかどうかを調べる
        Group *group = &group_state.groups[i];
        const unsigned int from = group->first_cell_idx;
        const unsigned int to = group->first_cell_idx + group->num_cells;
        if (from <= offset && offset < to) {
            GPUCell *first_cell = &group_state.first_gpu_cell[from];
            if (group->num_cells > 1 &&
                group->has_special_glyph &&
                (first_cell->attrs & WIDTH_MASK) == 1) {
                // おそらく単一の `calt` リガチャ
                // `calt`: 前後関係に依存する字形でリガチャとは異なる。
                *left = from;
                *right = to;
            }
            break;
        }
    }
}

/**
 * フォントグループでレンダリングする TODO: 謎
 *
 * @param fg フォントグループ
 * @param font フォントオブジェクト
 * @param center_glyph レンダリング位置を中央にする
 */
static inline void
render_groups(FontGroup *fg, Font *font, bool center_glyph) {
    // 先頭のグループから舐めていく
    for (unsigned i = 0; i <= group_state.group_idx; i++) {
        Group *group = &group_state.groups[i];

        // グループにセルがなければ終わり
        if (group->num_cells == 0) {
            break;
        }

        // 先頭のコードポイントを得る
        const glyph_index gi = group->num_glyphs != 0 ?
            group_state.info[group->first_glyph_idx].codepoint : 0;

        // 残りのコードポイントを ExtraGlyphs に詰める
        ExtraGlyphs ed;
        int last = -1;
        for (unsigned int j = 1; j < MIN(arraysz(ed.data) + 1, group->num_glyphs); j++) {
            last = j - 1;
            ed.data[last] = group_state.info[group->first_glyph_idx + j].codepoint;
        }
        if ((size_t)(last + 1) < arraysz(ed.data)) {
            ed.data[last + 1] = 0;
        }

        // PUAリガチャでスペースをレンダリングしたくないのは、Powerline
        // のようなスペースグリフのない愚かなフォントが存在するから。
        // 特別な場合：https://github.com/kovidgoyal/kitty/issues/1225
        const unsigned int num_glyphs = group->is_space_ligature ? 1 : group->num_glyphs;

        // グループのレンダリング
        render_group(fg,
                     group->num_cells,
                     num_glyphs,
                     &group_state.first_cpu_cell[group->first_cell_idx],
                     &group_state.first_gpu_cell[group->first_cell_idx],
                     &group_state.info[group->first_glyph_idx],
                     &group_state.positions[group->first_glyph_idx],
                     font,
                     gi,
                     &ed,
                     center_glyph);
    }
}

/**
 * レイアウトのテスト(Pythonモジュール)
 *
 * @param self 未使用
 * @param args 引数
 * @return フォントグループのリスト
 */
static PyObject *
test_shape(PyObject UNUSED *self, PyObject *args) {
    Line *line;
    char *path = NULL;
    int index = 0;

    if (!PyArg_ParseTuple(args, "O!|zi", &Line_Type, &line, &path, &index)) {
        return NULL;
    }
    index_type num = 0;
    while (num < line->xnum && line->cpu_cells[num].ch) {
        num += line->gpu_cells[num].attrs & WIDTH_MASK;
    }
    PyObject *face = NULL;
    Font *font;
    if (!num_font_groups) {
        PyErr_SetString(PyExc_RuntimeError, "must create at least one font group first");
        return NULL;
    }
    if (path) {
        face = face_from_path(path, index, (FONTS_DATA_HANDLE)font_groups);
        if (face == NULL) {
            return NULL;
        }
        font = calloc(1, sizeof(Font));
        font->face = face;
        font->hb_features[0] = hb_features[CALT_FEATURE];
        font->num_hb_features = 1;
    }
    else {
        FontGroup *fg = font_groups;
        font = fg->fonts + fg->medium_font_idx;
    }
    shape_run(line->cpu_cells, line->gpu_cells, num, font, false);

    PyObject *ans = PyList_New(0);
    unsigned int idx = 0;
    glyph_index first_glyph;
    while (idx <= G(group_idx)) {
        Group *group = G(groups) + idx;
        if (!group->num_cells) {
            break;
        }
        first_glyph = group->num_glyphs ? G(info)[group->first_glyph_idx].codepoint : 0;

        PyObject *eg = PyTuple_New(MAX_EXTRA_GLYPHS);
        for (size_t g = 0; g < MAX_EXTRA_GLYPHS; g++) {
            PyTuple_SET_ITEM(eg, g,
                             Py_BuildValue("H", g + 1 < group->num_glyphs ? G(info)[group->first_glyph_idx + g].codepoint : 0));
        }
        PyList_Append(ans, Py_BuildValue("IIHN", group->num_cells, group->num_glyphs, first_glyph, eg));
        idx++;
    }
    if (face) {
        Py_CLEAR(face);
        free_maps(font);
        free(font);
    }
    return ans;
}

#undef G

/**
 * ランのレンダリング
 *
 * @param fg フォントグループ
 * @param first_cpu_cell 先頭CPUセル
 * @param first_gpu_cell 先頭GPUセル
 * @param num_cells セルの個数
 * @param font_idx フォントインデックス
 * @param pua_space_ligature PUA領域リガチャ
 * @param center_glyph レンダリング位置を中央にする
 * @param cursor_offset オフセット
 * @param disable_ligature_strategy リガチャ無効戦略
 */
static inline void
render_run(
    FontGroup *fg,
    CPUCell *first_cpu_cell,
    GPUCell *first_gpu_cell,
    index_type num_cells,
    ssize_t font_idx,
    bool pua_space_ligature,
    bool center_glyph,
    int cursor_offset,
    DisableLigature disable_ligature_strategy
) {
    switch (font_idx) {
    default: {
        Font *font = &fg->fonts[font_idx];
        // 全体をレイアウトする
        shape_run(first_cpu_cell,
                  first_gpu_cell,
                  num_cells,
                  font,
                  disable_ligature_strategy == DISABLE_LIGATURES_ALWAYS);
        if (pua_space_ligature) {
            merge_groups_for_pua_space_ligature();
        }
        else if (cursor_offset > -1) {
            // 分割レンダリングすべきかどうか調べる
            index_type left, right;
            split_run_at_offset(cursor_offset, &left, &right);
            if (right > left) {
                // 先頭からleftまでをレイアウトしてレンダリングする
                if (left != 0) {
                    shape_run(first_cpu_cell, first_gpu_cell, left, font, false);
                    render_groups(fg, font, center_glyph);
                }

                // leftからrightまでを(リガチャ無効で)レイアウトしてレンダリング
                shape_run(&first_cpu_cell[left], &first_gpu_cell[left], right - left, font, true);
                render_groups(fg, font, center_glyph);

                // rightから最後までをレイアウトしてレンダリング
                if (right < num_cells) {
                    shape_run(&first_cpu_cell[right], &first_gpu_cell[right], num_cells - right, font, false);
                    render_groups(fg, font, center_glyph);
                }
                break;
            }
        }
        // レンダリングする
        render_groups(fg, font, center_glyph);
        break;
    }
    case BLANK_FONT:
        while (num_cells--) {
            set_sprite(first_gpu_cell, 0, 0, 0);
            first_cpu_cell++;
            first_gpu_cell++;
        }
        break;
    case BOX_FONT:
        while (num_cells--) {
            render_box_cell(fg, first_cpu_cell, first_gpu_cell);
            first_cpu_cell++;
            first_gpu_cell++;
        }
        break;
    case MISSING_FONT:
        while (num_cells--) {
            set_sprite(first_gpu_cell, MISSING_GLYPH, 0, 0);
            first_cpu_cell++;
            first_gpu_cell++;
        }
        break;
    }
}

static inline
void
render_run_impl(
    FontGroup *fg,
    ssize_t run_font_idx,
    index_type i,
    index_type first_cell_in_run,
    const Cursor *cursor,
    bool disable_ligature_at_cursor,
    const Line *line,
    bool is_centering,
    DisableLigature strategy
) {
    if (run_font_idx != NO_FONT && i > first_cell_in_run) {
        int cursor_offset = -1;
        if (disable_ligature_at_cursor &&
            first_cell_in_run <= cursor->x && cursor->x <= i) {
            cursor_offset = cursor->x - first_cell_in_run;
        }
        render_run(fg,
                line->cpu_cells + first_cell_in_run,
                line->gpu_cells + first_cell_in_run,
                i - first_cell_in_run,
                run_font_idx,
                false,
                is_centering,
                cursor_offset,
                strategy);
    }
}

/**
 * 行のレンダリング
 *
 * @param fg_ フォントグループ
 * @param line 行
 * @param lnum 行番号
 * @param cursor カーソル
 * @param disable_ligature_strategy リガチャ無効戦略
 */
void
render_line(FONTS_DATA_HANDLE fg_, Line *line, index_type lnum, Cursor *cursor, DisableLigature disable_ligature_strategy) {
    FontGroup *fg = (FontGroup *)fg_;
    ssize_t run_font_idx = NO_FONT;
    bool center_glyph = false;
    bool disable_ligature_at_cursor =
        cursor != NULL &&
        disable_ligature_strategy == DISABLE_LIGATURES_CURSOR &&
        lnum == cursor->y;
    index_type first_cell_in_run, i;
    attrs_type prev_width = 0;
    for (i = 0, first_cell_in_run = 0; i < line->xnum; i++) {
        if (prev_width == 2) {
            prev_width = 0;
            continue;
        }
        CPUCell *cpu_cell = line->cpu_cells + i;
        GPUCell *gpu_cell = line->gpu_cells + i;
        bool is_fallback_font, is_emoji_presentation;
        ssize_t cell_font_idx = font_for_cell(fg, cpu_cell, gpu_cell, &is_fallback_font, &is_emoji_presentation);

        if (cell_font_idx != MISSING_FONT &&
            ((is_fallback_font && !is_emoji_presentation && is_symbol(cpu_cell->ch)) ||
            (cell_font_idx != BOX_FONT && is_private_use(cpu_cell->ch)))) {
            unsigned int desired_cells = 1;
            if (cell_font_idx > 0) {
                Font *font = (fg->fonts + cell_font_idx);
                glyph_index glyph_id = glyph_id_for_codepoint(font->face, cpu_cell->ch);

                int width = get_glyph_width(font->face, glyph_id);
                desired_cells = (unsigned int)ceilf((float)width / fg->cell_width);
            }

            unsigned int num_spaces = 0;
            while ((line->cpu_cells[i + num_spaces + 1].ch == ' ') &&
                   num_spaces < MAX_NUM_EXTRA_GLYPHS_PUA &&
                   num_spaces < desired_cells &&
                   i + num_spaces + 1 < line->xnum) {
                num_spaces++;

                // 私的利用文字の後に空白が続く場合、マルチセルのリガチャとして
                // レンダリングする
                GPUCell *space_cell = line->gpu_cells + i + num_spaces;

                // 空白セルがPUAセルの前景色を使用していることを保証する。
                // これは、空白とPUAに異なる前景色を持つPUA + 空白を使用する
                // Powerline などのアプリケーションがあるために必要です。
                // 参考: https://github.com/kovidgoyal/kitty/issues/467
                space_cell->fg = gpu_cell->fg;
                space_cell->decoration_fg = gpu_cell->decoration_fg;
            }
            // ランのレンダリング
            if (num_spaces != 0) {
                center_glyph = true;
                render_run_impl(fg,
                                run_font_idx,
                                i,
                                first_cell_in_run,
                                cursor,
                                disable_ligature_at_cursor,
                                line,
                                center_glyph,
                                disable_ligature_strategy);
                center_glyph = false;
                render_run(fg,
                           line->cpu_cells + i,
                           line->gpu_cells + i,
                           num_spaces + 1,
                           cell_font_idx,
                           true,
                           center_glyph,
                           -1,
                           disable_ligature_strategy);
                run_font_idx = NO_FONT;
                first_cell_in_run = i + num_spaces + 1;
                prev_width = line->gpu_cells[i + num_spaces].attrs & WIDTH_MASK;
                i += num_spaces;
                continue;
            }
        }
        prev_width = gpu_cell->attrs & WIDTH_MASK;
        if (run_font_idx == NO_FONT) {
            run_font_idx = cell_font_idx;
        }
        if (run_font_idx == cell_font_idx) {
            continue;
        }
        render_run_impl(fg,
                        run_font_idx,
                        i,
                        first_cell_in_run,
                        cursor,
                        disable_ligature_at_cursor,
                        line,
                        center_glyph,
                        disable_ligature_strategy);
        run_font_idx = cell_font_idx;
        first_cell_in_run = i;
    }
    render_run_impl(fg,
                    run_font_idx,
                    i,
                    first_cell_in_run,
                    cursor,
                    disable_ligature_at_cursor,
                    line,
                    center_glyph,
                    disable_ligature_strategy);
}

/**
 * 単純テキストをレンダリングする
 *
 * @param fg_ フォントグループ
 * @param text テキスト
 * @return StringCanvas
 * \note child-monitor.cの draw_resizing_text からしか呼ばれない...
 */
StringCanvas
render_simple_text(FONTS_DATA_HANDLE fg_, const char *text) {
    FontGroup *fg = (FontGroup *)fg_;

    if (fg->fonts_count && fg->medium_font_idx) {
        // FreeType or CoreText実装を呼び出す
        return render_simple_text_impl(fg->fonts[fg->medium_font_idx].face, text, fg->baseline);
    }
    StringCanvas ans = {0};
    return ans;
}

/**
 * シンボルマップをクリアする
 */
static inline void
clear_symbol_maps(void) {
    if (symbol_maps) {
        free(symbol_maps);
        symbol_maps = NULL;
        num_symbol_maps = 0;
    }
}

/**
 * デスクリプタインデックス
 *  メンバには render.py で保持している current_faces 配列のインデックスが格納
 *  される
 */
typedef struct {
    unsigned int main, // aka. medium
                 bold,
                 italic,
                 bi,
                 num_symbol_fonts;
} DescriptorIndices;

DescriptorIndices descriptor_indices = {0};

/**
 * フォントデータを設定する(Pythonモジュール)
 *
 * @param m 未使用
 * @param args 引数
 */
static PyObject *
set_font_data(PyObject UNUSED *m, PyObject *args) {
    PyObject *sm; // シンボルマップ?

    // PyObjectのクリア
    Py_CLEAR(box_drawing_function);
    Py_CLEAR(prerender_function);
    Py_CLEAR(descriptor_for_idx);

    if (!PyArg_ParseTuple(
                args,
                "OOOIIIIO!d",
                &box_drawing_function,
                &prerender_function,
                &descriptor_for_idx,
                &descriptor_indices.bold,
                &descriptor_indices.italic,
                &descriptor_indices.bi,
                &descriptor_indices.num_symbol_fonts,
                &PyTuple_Type,
                &sm,
                &global_state.font_sz_in_pts)) {
        return NULL;
    }
    Py_INCREF(box_drawing_function);
    Py_INCREF(prerender_function);
    Py_INCREF(descriptor_for_idx);
    free_font_groups();

    // シンボルマップを確保する
    clear_symbol_maps();
    num_symbol_maps = PyTuple_GET_SIZE(sm);
    symbol_maps = calloc(num_symbol_maps, sizeof(SymbolMap));
    if (!symbol_maps) {
        return PyErr_NoMemory();
    }

    for (size_t s = 0; s < num_symbol_maps; s++) {
        unsigned int left, right, font_idx;
        SymbolMap *x = &symbol_maps[s];
        if (!PyArg_ParseTuple(PyTuple_GET_ITEM(sm, s), "III", &left, &right, &font_idx)) {
            return NULL;
        }
        x->left = left;
        x->right = right;
        x->font_idx = font_idx;
    }
    Py_RETURN_NONE;
}

/**
 * レンダリング前のスプライトを転送する
 *
 * @param fg フォントグループ
 */
static inline void
send_prerendered_sprites(FontGroup *fg) {
    int error = 0;
    sprite_index x = 0, y = 0, z = 0;

    // ブランクセル
    clear_canvas(fg);

    // スプライト(fg->canvas)をGPUに転送する
    current_send_sprite_to_gpu((FONTS_DATA_HANDLE)fg, x, y, z, fg->canvas);

    // スプライト・トラッカーを更新する
    do_increment(fg, &error);
    if (error != 0) {
        sprite_map_set_error(error);
        PyErr_Print();
        fatal("Failed");
    }

    // プリレンダー関数を呼び出す
    PyObject *args = PyObject_CallFunction(prerender_function,
                                           "IIIIIdd",
                                           fg->cell_width,
                                           fg->cell_height,
                                           fg->baseline,
                                           fg->underline_position,
                                           fg->underline_thickness,
                                           fg->logical_dpi_x,
                                           fg->logical_dpi_y);
    if (!args) {
        PyErr_Print();
        fatal("Failed to pre-render cells");
    }

    // アルファマスクをレンダリングする
    for (ssize_t i = 0; i < PyTuple_GET_SIZE(args) - 1; i++) {
        x = fg->sprite_tracker.x;
        y = fg->sprite_tracker.y;
        z = fg->sprite_tracker.z;
        if (y > 0) {
            fatal("Too many pre-rendered sprites for your GPU or the font size is too large");
        }

        // スプライト・トラッカーを更新する
        do_increment(fg, &error);
        if (error != 0) {
            sprite_map_set_error(error);
            PyErr_Print();
            fatal("Failed");
        }

        uint8_t *alpha_mask = PyLong_AsVoidPtr(PyTuple_GET_ITEM(args, i));
        clear_canvas(fg);
        Region region = {
            .right = fg->cell_width,
            .bottom = fg->cell_height
        };
        render_alpha_mask(alpha_mask, fg->canvas, &region, &region, fg->cell_width, fg->cell_width);
        current_send_sprite_to_gpu((FONTS_DATA_HANDLE)fg, x, y, z, fg->canvas);
    }
    Py_CLEAR(args);
}

/**
 * フォントの初期化
 *
 * @param fg フォントグループ
 * @param desc_idx フォント配列のインデックス(この位置にフォントを初期化する)
 * @param ftype デバッグ用の文字列
 * @return フォントインデックス
 */
static inline size_t
initialize_font(FontGroup *fg, unsigned int desc_idx, const char *ftype) {
    // `descriptor_for_idx` 関数を呼び出す
    PyObject *d = PyObject_CallFunction(descriptor_for_idx, "I", desc_idx);
    if (!d) {
        PyErr_Print();
        fatal("Failed for %s font", ftype);
    }

    bool bold = PyObject_IsTrue(PyTuple_GET_ITEM(d, 1));
    bool italic = PyObject_IsTrue(PyTuple_GET_ITEM(d, 2));
    PyObject *face = desc_to_face(PyTuple_GET_ITEM(d, 0), (FONTS_DATA_HANDLE)fg);
    Py_CLEAR(d);
    if (!face) {
        PyErr_Print();
        fatal("Failed to convert descriptor to face for %s font", ftype);
    }
    // 空きスロットにフォント情報を設定する
    size_t idx = fg->fonts_count++;
    bool ok = init_font(fg->fonts + idx, face, bold, italic, false);
    Py_CLEAR(face);
    if (!ok) {
        if (PyErr_Occurred()) {
            PyErr_Print();
        }
        fatal("Failed to initialize %s font: %zu", ftype, idx);
    }
    return idx;
}

/**
 * フォントグループの初期化
 */
static void
initialize_font_group(FontGroup *fg) {
    fg->fonts_capacity = 10 + descriptor_indices.num_symbol_fonts;

    // フォント配列の確保
    fg->fonts = calloc(fg->fonts_capacity, sizeof(Font));
    if (!fg->fonts) {
        fatal("Out of memory allocating fonts array");
    }
    fg->fonts_count = 1;  // インデックス0はボックスフォントを指す

#define I(attr) \
    if (descriptor_indices.attr) \
        fg->attr ## _font_idx = initialize_font(fg, descriptor_indices.attr, #attr); \
    else \
        fg->attr ## _font_idx = -1;

    fg->medium_font_idx = initialize_font(fg, 0, "medium");
    I(bold);
    I(italic);
    I(bi);

#undef I

    fg->first_symbol_font_idx = fg->fonts_count;
    fg->first_fallback_font_idx = fg->fonts_count;
    fg->fallback_fonts_count = 0;

    // 記号フォント(?)を初期化していく
    for (size_t i = 0; i < descriptor_indices.num_symbol_fonts; i++) {
        initialize_font(fg, descriptor_indices.bi + 1 + i, "symbol_map");
        fg->first_fallback_font_idx++;
    }
    
    // セルの寸法を計算する
    calc_cell_metrics(fg);
}

/**
 * ウィンドウに対応するプリレンダリング・スプライトを転送する
 *
 * @param w OSウィンドウ
 */
void
send_prerendered_sprites_for_window(OSWindow *w) {
    FontGroup *fg = (FontGroup *)w->fonts_data;

    // スプライト・マップがなければ確保して転送する
    if (!fg->sprite_map) {
        fg->sprite_map = alloc_sprite_map(fg->cell_width, fg->cell_height);
        send_prerendered_sprites(fg);
    }
}

/**
 * フォントデータをロードする
 *
 * \param font_sz_in_pts ポイントサイズ
 * \param dpi_x DPIのx
 * \param dpi_x DPIのy
 * \return FONTS_DATA_HANDLE
 */
FONTS_DATA_HANDLE
load_fonts_data(double font_sz_in_pts, double dpi_x, double dpi_y) {
    FontGroup *fg = font_group_for(font_sz_in_pts, dpi_x, dpi_y);
    return (FONTS_DATA_HANDLE)fg;
}

/**
 * 後始末
 */
static void
finalize(void) {
    Py_CLEAR(python_send_to_gpu_impl);
    clear_symbol_maps();
    Py_CLEAR(box_drawing_function);
    Py_CLEAR(prerender_function);
    Py_CLEAR(descriptor_for_idx);
    free_font_groups();
    if (harfbuzz_buffer) {
        hb_buffer_destroy(harfbuzz_buffer);
        harfbuzz_buffer = NULL;
    }
    free(group_state.groups);
    group_state.groups = NULL;
    group_state.groups_capacity = 0;
}

/**
 * スプライトマップにレイアウトを設定する (Pythonモジュール)
 *
 * @param self ?
 * @param args 引数
 */
static PyObject *
sprite_map_set_layout(PyObject UNUSED *self, PyObject *args) {
    // 引数から幅と高さを得る
    unsigned int w, h;
    if (!PyArg_ParseTuple(args, "II", &w, &h)) {
        return NULL;
    }
    if (num_font_groups == 0) {
        PyErr_SetString(PyExc_RuntimeError, "must create font group first");
        return NULL;
    }

    // スプライトトラッカーのレイアウトを設定する
    sprite_tracker_set_layout(&font_groups->sprite_tracker, w, h);

    Py_RETURN_NONE;
}

/**
 * スプライト位置をテストする(Pythonモジュール)
 *  指定されたグリフのスプライト位置情報を返す
 *
 * @param self 未使用
 * @param args 引数
 * @return 3次元座標(PyObject)
 */
static PyObject *
test_sprite_position_for(PyObject UNUSED *self, PyObject *args) {
    // 引数からグリフインデックスとエクストラグリフを得る
    glyph_index glyph;
    ExtraGlyphs extra_glyphs = {{0}};
    if (!PyArg_ParseTuple(args, "H|H", &glyph, &extra_glyphs.data)) {
        return NULL;
    }
    if (num_font_groups == 0) {
        PyErr_SetString(PyExc_RuntimeError, "must create font group first");
        return NULL;
    }

    int error;
    FontGroup *fg = font_groups;
    SpritePosition *pos = sprite_position_for(fg, &fg->fonts[fg->medium_font_idx], glyph, &extra_glyphs, 0, &error);
    if (!pos) {
        sprite_map_set_error(error);
        return NULL;
    }
    return Py_BuildValue("HHH", pos->x, pos->y, pos->z);
}

/**
 * スプライトをGPUに転送する関数を設定する - Pythonモジュール
 *
 * @param self 未使用
 * @param fun 関数
 */
static PyObject *
set_send_sprite_to_gpu(PyObject UNUSED *self, PyObject *func) {
    // static変数に設定されている関数を削除する
    Py_CLEAR(python_send_to_gpu_impl);

    // static変数に設定する
    if (func != Py_None) {
        python_send_to_gpu_impl = func;
        Py_INCREF(python_send_to_gpu_impl);
    }
    current_send_sprite_to_gpu = python_send_to_gpu_impl ? python_send_to_gpu : send_sprite_to_gpu;
    Py_RETURN_NONE;
}

/**
 * 行レンダリングのテスト(Pythonモジュール)
 *
 * @param self 未使用
 * @param args 引数
 */
static PyObject *
test_render_line(PyObject UNUSED *self, PyObject *args) {
    // 引数から行種類と行を得る
    PyObject *line;
    if (!PyArg_ParseTuple(args, "O!", &Line_Type, &line)) {
        return NULL;
    }
    if (num_font_groups == 0) {
        PyErr_SetString(PyExc_RuntimeError, "must create font group first");
        return NULL;
    }

    // レンダリングする
    render_line((FONTS_DATA_HANDLE)font_groups, (Line *)line, 0, NULL, DISABLE_LIGATURES_NEVER);
    Py_RETURN_NONE;
}

/**
 * セルを連結する(Pythonモジュール)
 *  RGBAデータを返すセルを連結する
 *
 * @param self 未使用
 * @param args 引数
 */
static PyObject *
concat_cells(PyObject UNUSED *self, PyObject *args) {
    // 引数を得る
    unsigned int cell_width, cell_height;
    int is_32_bit;
    PyObject *cells;
    if (!PyArg_ParseTuple(args, "IIpO!", &cell_width, &cell_height, &is_32_bit, &PyTuple_Type, &cells)) {
        return NULL;
    }

    // ピクセルバッファを生成する(このバッファにレンダリング結果を詰める)
    const size_t num_cells = PyTuple_GET_SIZE(cells);
    PyObject *ans = PyBytes_FromStringAndSize(NULL, 4 * cell_width * cell_height * num_cells);
    if (!ans) {
        return PyErr_NoMemory();
    }
    pixel *dest = (pixel *)PyBytes_AS_STRING(ans);

    // 引数cellsからピクセル値を取り出しdestに詰める
    for (size_t r = 0; r < cell_height; r++) {
        for (size_t c = 0; c < num_cells; c++) {
            void *s = ((uint8_t *)PyBytes_AS_STRING(PyTuple_GET_ITEM(cells, c)));
            if (is_32_bit) {
                pixel *src = (pixel *)s + cell_width * r;
                for (size_t i = 0; i < cell_width; i++, dest++) {
                    uint8_t *rgba = (uint8_t *)dest;
                    rgba[0] = (src[i] >> 24) & 0xff;
                    rgba[1] = (src[i] >> 16) & 0xff;
                    rgba[2] = (src[i] >> 8) & 0xff;
                    rgba[3] = src[i] & 0xff;
                }
            }
            else {
                uint8_t *src = (uint8_t *)s + cell_width * r;
                for (size_t i = 0; i < cell_width; i++, dest++) {
                    uint8_t *rgba = (uint8_t *)dest;
                    if (src[i]) {
                        memset(rgba, 0xff, 3);
                        rgba[3] = src[i];
                    }
                    else {
                        *dest = 0;
                    }
                }
            }
        }
    }
    return ans;
}

/**
 * 現在のフォントを返す(Pythonモジュール)
 */
static PyObject *
current_fonts(PYNOARG) {
    if (num_font_groups == 0) {
        PyErr_SetString(PyExc_RuntimeError, "must create font group first");
        return NULL;
    }
    PyObject *ans = PyDict_New();
    if (!ans) {
        return NULL;
    }
    FontGroup *fg = font_groups;
#define SET(key, val) { \
    if (PyDict_SetItemString(ans, #key, fg->fonts[val].face) != 0) { goto error; } \
}
    SET(medium, fg->medium_font_idx);
    if (fg->bold_font_idx > 0) {
        SET(bold, fg->bold_font_idx);
    }
    if (fg->italic_font_idx > 0) {
        SET(italic, fg->italic_font_idx);
    }
    if (fg->bi_font_idx > 0) {
        SET(bi, fg->bi_font_idx);
    }
    PyObject *ff = PyTuple_New(fg->fallback_fonts_count);
    if (!ff) {
        goto error;
    }
    for (size_t i = 0; i < fg->fallback_fonts_count; i++) {
        Py_INCREF(fg->fonts[fg->first_fallback_font_idx + i].face);
        PyTuple_SET_ITEM(ff, i, fg->fonts[fg->first_fallback_font_idx + i].face);
    }
    PyDict_SetItemString(ans, "fallback", ff);
    Py_CLEAR(ff);
    return ans;
 error:
    Py_CLEAR(ans);
    return NULL;
#undef SET
}

/**
 * フォールバックフォントを取得する - Pythonモジュール
 *
 * \param[in] self 未使用
 * \param[in] args 引数
 * \return CTFaceオブジェクト
 */
static PyObject *
get_fallback_font(PyObject UNUSED *self, PyObject *args) {
    if (num_font_groups == 0) {
        PyErr_SetString(PyExc_RuntimeError, "must create font group first");
        return NULL;
    }

    // 引数からテキストとbold, italicフラグを得る
    PyObject *text;
    int bold, italic;
    if (!PyArg_ParseTuple(args, "Upp", &text, &bold, &italic)) {
        return NULL;
    }

    // テキストからCPUセルとGPUセルを作成する
    // フォールバックフォントを得るためだけに一時的に確保する
    CPUCell cpu_cell = {0};
    GPUCell gpu_cell = {0};
    static Py_UCS4 char_buf[2 + arraysz(cpu_cell.cc_idx)];
    if (!PyUnicode_AsUCS4(text, char_buf, arraysz(char_buf), 1)) {
        return NULL;
    }

    // CPUセルの `.ch` にコードポイントを格納する
    cpu_cell.ch = char_buf[0];

    // CPUセルの `.cc_idx[]` に結合文字の記号（異体字セレクタ）を入れる
    //
    // - 結合記号でなければ cc_idx には 0 が入る
    //  - mark_for_codepoint関数の仕様上、0が返って来るので
    // - 結合記号であれば、マーク(内部的なインデックス値)に変換して cc_idx に入
    //   れる
    //
    // NOTE: 結合文字表現は [基底文字のコードポイント][結合記号][結合記号] ...
    // となる。結合記号には、ダイアクリティカルマークや囲み記号、異体字セレクタ
    // などが用いられる。
    //
    for (unsigned i = 0; i + 1 < (unsigned)PyUnicode_GetLength(text) && i < arraysz(cpu_cell.cc_idx); i++) {
        cpu_cell.cc_idx[i] = mark_for_codepoint(char_buf[i + 1]);
    }

    // GPUセルにボールド、イタリック属性をセットする
    if (bold) {
        gpu_cell.attrs |= 1 << BOLD_SHIFT;
    }
    if (italic) {
        gpu_cell.attrs |= 1 << ITALIC_SHIFT;
    }

    // フォールバックフォントを得る
    FontGroup *fg = font_groups;
    ssize_t ans = fallback_font(fg, &cpu_cell, &gpu_cell);
    if (ans == MISSING_FONT) {
        PyErr_SetString(PyExc_ValueError, "No fallback font found");
        return NULL;
    }
    if (ans < 0) {
        PyErr_SetString(PyExc_ValueError, "Too many fallback fonts");
        return NULL;
    }
    return fg->fonts[ans].face;
}

/**
 * フォントグループ生成のテスト(Pythonモジュール)
 *
 * @param self 未使用
 * @param args 引数
 */
static PyObject *
create_test_font_group(PyObject *self UNUSED, PyObject *args) {
    // 引数からポイントサイズとDPI座標を得る
    double sz, dpix, dpiy;
    if (!PyArg_ParseTuple(args, "ddd", &sz, &dpix, &dpiy)) {
        return NULL;
    }

    // フォントを得る(生成する)
    FontGroup *fg = font_group_for(sz, dpix, dpiy);
    if (!fg->sprite_map) {
        send_prerendered_sprites(fg);
    }
    return Py_BuildValue("II", fg->cell_width, fg->cell_height);
}

/**
 * フォントデータを解放する(Pythonモジュール)
 *
 * @param self 未使用
 * @param args 引数
 */
static PyObject *
free_font_data(PyObject *self UNUSED, PyObject *args UNUSED) {
    // 後始末関数を呼ぶ
    finalize();
    Py_RETURN_NONE;
}

/**
 * Pythonモジュールメソッド定義
 */
static PyMethodDef module_methods[] = {
    METHODB(set_font_data,            METH_VARARGS),
    METHODB(free_font_data,           METH_NOARGS),
    METHODB(create_test_font_group,   METH_VARARGS),
    METHODB(sprite_map_set_layout,    METH_VARARGS),
    METHODB(test_sprite_position_for, METH_VARARGS),
    METHODB(concat_cells,             METH_VARARGS),
    METHODB(set_send_sprite_to_gpu,   METH_O),
    METHODB(test_shape,               METH_VARARGS),
    METHODB(current_fonts,            METH_NOARGS),
    METHODB(test_render_line,         METH_VARARGS),
    METHODB(get_fallback_font,        METH_VARARGS),
    {NULL,                            NULL,                           0, NULL} /* Sentinel */
};

/**
 * モジュールのフォント関連を初期化する
 *
 * @param module Pythonモジュール
 */
bool
init_fonts(PyObject *module) {
    // HarfBuzzバッファを生成する
    harfbuzz_buffer = hb_buffer_create();
    if (!harfbuzz_buffer ||
        !hb_buffer_allocation_successful(harfbuzz_buffer) ||
        !hb_buffer_pre_allocate(harfbuzz_buffer, 2048)) {
        PyErr_NoMemory();
        return false;
    }

    // HarfBuzzバッファのクラスタレベルを設定する
    hb_buffer_set_cluster_level(harfbuzz_buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

    // 機能を設定する
#define create_feature(feature, where) { \
        if (!hb_feature_from_string(feature, sizeof(feature) - 1, &hb_features[where])) { \
            PyErr_SetString(PyExc_RuntimeError, "Failed to create " feature " harfbuzz feature"); \
            return false; \
        }}
    create_feature("-liga", LIGA_FEATURE); // リガチャ
    create_feature("-dlig", DLIG_FEATURE); // 任意リガチャ
    create_feature("-calt", CALT_FEATURE); // 前後関係に依存する字形
#undef create_feature
    if (PyModule_AddFunctions(module, module_methods) != 0) {
        return false;
    }

    // スプライト転送関数のデフォルトを設定する
    current_send_sprite_to_gpu = send_sprite_to_gpu;
    return true;
}
