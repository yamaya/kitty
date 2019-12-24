/*
 * data-types.h
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>
#include <stdbool.h>
#include <poll.h>
#include <pthread.h>
#include "glfw-wrapper.h"

// Required minimum OpenGL version
#define OPENGL_REQUIRED_VERSION_MAJOR 3
#define OPENGL_REQUIRED_VERSION_MINOR 3
#define GLFW_MOD_KITTY                1024
#define UNUSED                        __attribute__ ((unused))
#define PYNOARG                       PyObject *__a1 UNUSED, PyObject *__a2 UNUSED
#define EXPORTED                      __attribute__ ((visibility("default")))

/**
 * 多くの場合に真となる場合に使用するマクロ
 *  分岐予測のヒントをコンパイラに与える。Linux Kernelで用いられる手法。
 *  see: https://qiita.com/kaityo256/items/8a0c5376fad17907e1f6
 *
 * @param x 式
 */
#define LIKELY(x)                   __builtin_expect(!!(x), 1)

/**
 * 多くの場合に偽となる場合に使用するマクロ
 *
 * @param x 式
 */
#define UNLIKELY(x)                 __builtin_expect(!!(x), 0)

#define MAX(x, y)                   __extension__({ \
        __typeof__ (x) a = (x); __typeof__ (y) b = (y); \
        a > b ? a : b;})
#define MIN(x, y)                   __extension__({ \
        __typeof__ (x) a = (x); __typeof__ (y) b = (y); \
        a < b ? a : b;})
#define xstr(s)                     str(s)
#define str(s)                      #s
#define arraysz(x)                  (sizeof(x) / sizeof(x[0]))
#define zero_at_i(array, idx)       memset((array) + (idx), 0, sizeof((array)[0]))
#define zero_at_ptr(p)              memset((p), 0, sizeof((p)[0]))
#define zero_at_ptr_count(p, count) memset((p), 0, (count) * sizeof((p)[0]))
void log_error(const char *fmt, ...) __attribute__ ((format(printf, 1, 2)));
#define fatal(...)                  {log_error(__VA_ARGS__); exit(EXIT_FAILURE);}

typedef unsigned long long id_type;
typedef uint32_t char_type;
typedef uint32_t color_type;
typedef uint16_t combining_type;
typedef uint32_t pixel;
typedef unsigned int index_type;
typedef uint16_t sprite_index;
typedef uint16_t attrs_type;

/**
 * 行属性型
 */
typedef uint8_t line_attrs_type;

/**
 * カーソル形状
 */
typedef enum CursorShapes {
    NO_CURSOR_SHAPE, /** 形状なし */
    CURSOR_BLOCK,   /** ブロック */
    CURSOR_BEAM,   /** ビーム */
    CURSOR_UNDERLINE,   /** アンダーライン */
    NUM_OF_CURSOR_SHAPES   /** 種類数 */
} CursorShape;

/**
 * リガチャ無効
 */
typedef enum {
    DISABLE_LIGATURES_NEVER,    /** しない */
    DISABLE_LIGATURES_CURSOR,   /** カーソル */
    DISABLE_LIGATURES_ALWAYS    /** 常に */
} DisableLigature;

#define ERROR_PREFIX "[PARSE ERROR]"
typedef enum MouseTrackingModes {
    NO_TRACKING,
    BUTTON_MODE,
    MOTION_MODE,
    ANY_MODE
} MouseTrackingMode;
typedef enum MouseTrackingProtocols {
    NORMAL_PROTOCOL,
    UTF8_PROTOCOL,
    SGR_PROTOCOL,
    URXVT_PROTOCOL
} MouseTrackingProtocol;
typedef enum MouseShapes {
    BEAM,
    HAND,
    ARROW
} MouseShape;
typedef enum {
    NONE,
    MENUBAR,
    WINDOW,
    ALL
} WindowTitleIn;

#define MAX_CHILDREN             512
#define BLANK_CHAR               0
#define ATTRS_MASK_WITHOUT_WIDTH 0xFFC
#define WIDTH_MASK               3
#define DECORATION_SHIFT         2
#define DECORATION_MASK          3
#define BOLD_SHIFT               4
#define ITALIC_SHIFT             5
#define BI_VAL(attrs)     ((attrs >> 4) & 3)
#define REVERSE_SHIFT            6
#define STRIKE_SHIFT             7
#define DIM_SHIFT                8

/**
 * 色マスク
 */
#define COL_MASK                 0xFFFFFFFF

#define UTF8_ACCEPT              0
#define UTF8_REJECT              1
#define DECORATION_FG_CODE       58
#define CHAR_IS_BLANK(ch) ((ch) == 32 || (ch) == 0)

/**
 * 行属性: 次行継続
 */
#define CONTINUED_MASK           1

/**
 * 行属性: テキストがdirty状態
 */
#define TEXT_DIRTY_MASK          2

#define FG                       1
#define BG                       2

/**
 * カーソルの状態を属性値に変換する
 *
 * \param[in] c Cursorオブジェクト
 * \param[in] w 文字幅
 * \return 属性値
 */
#define CURSOR_TO_ATTRS(c, w) \
    ((w) | (((c->decoration & 3) << DECORATION_SHIFT) | ((c->bold & 1) << BOLD_SHIFT) | \
            ((c->italic & 1) << ITALIC_SHIFT) | ((c->reverse & 1) << REVERSE_SHIFT) | \
            ((c->strikethrough & 1) << STRIKE_SHIFT) | ((c->dim & 1) << DIM_SHIFT)))

#define ATTRS_TO_CURSOR(a, c) \
    (c)->decoration = (a >> DECORATION_SHIFT) & 3; (c)->bold = (a >> BOLD_SHIFT) & 1; (c)->italic = (a >> ITALIC_SHIFT) & 1; \
    (c)->reverse = (a >> REVERSE_SHIFT) & 1; (c)->strikethrough = (a >> STRIKE_SHIFT) & 1; (c)->dim = (a >> DIM_SHIFT) & 1;

#define COPY_CELL(src, s, dest, d) \
    (dest)->cpu_cells[d] = (src)->cpu_cells[s]; (dest)->gpu_cells[d] = (src)->gpu_cells[s];

#define COPY_SELF_CELL(s, d)    COPY_CELL(self, s, self, d)

#define METHOD(name, arg_type)  {#name, (PyCFunction)name, arg_type, name ## _doc},
#define METHODB(name, arg_type) {#name, (PyCFunction)name, arg_type, ""}

#define BOOL_GETSET(type, x) \
    static PyObject *x ## _get(type * self, void UNUSED * closure) {PyObject *ans = self->x ? Py_True : Py_False; \
                                                                    Py_INCREF(ans); return ans;} \
    static int x ## _set(type * self, PyObject * value, void UNUSED * closure) {if (value == NULL) {PyErr_SetString( \
                                                                                                        PyExc_TypeError, \
                                                                                                        "Cannot delete attribute"); \
                                                                                                    return -1; \
                                                                                } self->x = \
                                                                                    PyObject_IsTrue(value) ? true : false; \
                                                                                return 0;}

/**
 * Pythonオブジェクトにgetter/setterを生やすマクロ
 *  - setter: "{name}_set" という名前のメソッド
 *  - getter: "{name}_get" という名前のメソッド
 */
#define GETSET(x) \
    {#x, (getter)x ## _get, (setter)x ## _set, #x, NULL},

#ifndef EXTRA_INIT
#define EXTRA_INIT
#endif
#define INIT_TYPE(type) \
    int init_ ## type(PyObject * module) { \
        if (PyType_Ready(&type ## _Type) < 0) return 0; \
        if (PyModule_AddObject(module, #type, (PyObject *)&type ## _Type) != 0) return 0; \
        Py_INCREF(&type ## _Type); \
        EXTRA_INIT; \
        return 1; \
    }

#define RICHCMP(type) \
    static PyObject *richcmp(PyObject * obj1, PyObject * obj2, int op) { \
        PyObject *result = NULL; \
        int eq; \
        if (op != Py_EQ && op != Py_NE) {Py_RETURN_NOTIMPLEMENTED;} \
        if (!PyObject_TypeCheck(obj1, &type ## _Type)) {Py_RETURN_FALSE;} \
        if (!PyObject_TypeCheck(obj2, &type ## _Type)) {Py_RETURN_FALSE;} \
        eq = __eq__((type *)obj1, (type *)obj2); \
        if (op == Py_NE) result = eq ? Py_False : Py_True; \
        else result = eq ? Py_True : Py_False; \
        Py_INCREF(result); \
        return result; \
    }

#ifdef __clang__
#define START_ALLOW_CASE_RANGE  _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wpedantic\"")
#define END_ALLOW_CASE_RANGE    _Pragma("clang diagnostic pop")
#define ALLOW_UNUSED_RESULT     _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wunused-result\"")
#define END_ALLOW_UNUSED_RESULT _Pragma("clang diagnostic pop")
#else
#define START_ALLOW_CASE_RANGE  _Pragma("GCC diagnostic ignored \"-Wpedantic\"")
#define END_ALLOW_CASE_RANGE    _Pragma("GCC diagnostic pop")
#define ALLOW_UNUSED_RESULT     _Pragma("GCC diagnostic ignored \"-Wunused-result\"")
#define END_ALLOW_UNUSED_RESULT _Pragma("GCC diagnostic pop")
#endif

typedef struct {
    uint32_t left, top, right, bottom;
} Region;

/**
 * GPUセル
 */
typedef struct {
    color_type fg;  /** 前景色 */
    color_type bg;  /** 背景色 */
    color_type decoration_fg;   /** 装飾前景色 */
    sprite_index sprite_x; /** スプライト位置 x */
    sprite_index sprite_y; /** スプライト位置 y */
    sprite_index sprite_z; /** スプライト位置 z */
    attrs_type attrs;   /** 属性 */
} GPUCell;

/**
 * CPUセル
 */
typedef struct {
    /**
     * 文字コード
     *
     * \note char_type = uint32_t
     */
    char_type ch;

    /**
     * 結合文字 (Combining Character) の記号部分
     *
     * ここでは、記号のコードポイントをインデックスに変換した値(マークと呼称し
     * ている)を格納している点に注意。
     * メンバ名が `_idx` で終わっているのもこれが理由だろう。
     *
     * なお、UNICODE仕様の語録では "記号" は "Mark" と呼称されている。
     * このメンバの値に関するコードで "mark" と言う単語がよく使われるのはこれが
     * 理由だろう。
     *
     * このメンバに代入する箇所:
     * - line.c の line_add_combining_char 関数
     * - screen.c の screen_tab 関数
     *
     * \note combining_type = uint16_t
     * \note ch がタブ文字の場合はタブ位置までの桁数が格納される(screen.cのscreen_tabを参照のこと)
     */
    combining_type cc_idx[2];
} CPUCell;

/**
 * 行
 */
typedef struct {
    PyObject_HEAD

    GPUCell *gpu_cells; /** GPUセル配列 */
    CPUCell *cpu_cells; /** CPUセル配列 */
    index_type xnum;
    index_type ynum; /** 行インデックス */
    bool continued; /** 次行へ継続している */
    bool needs_free; /** 解放が必要 */
    bool has_dirty_text; /** dirtyなテキストを保持している */
} Line;

/**
 * 行バッファ
 */
typedef struct {
    PyObject_HEAD

    GPUCell *gpu_cell_buf; /** GPUセル配列 */
    CPUCell *cpu_cell_buf; /** CPUセル配列 */
    index_type xnum;
    index_type ynum;  /** 行インデックス */
    index_type *line_map;
    index_type *scratch;
    line_attrs_type *line_attrs; /** 行属性 */
    Line *line; /** 行バッファ */
} LineBuf;

/**
 * 履歴バッファセグメント
 */
typedef struct {
    GPUCell *gpu_cells;
    CPUCell *cpu_cells;
    line_attrs_type *line_attrs;
} HistoryBufSegment;

/**
 * 履歴バッファページャー
 */
typedef struct {
    index_type bufsize, maxsz;
    Py_UCS4 *buffer;
    index_type start, end;
    index_type bufend;
    bool rewrap_needed;
} PagerHistoryBuf;

/**
 * 履歴バッファ
 */
typedef struct {
    PyObject_HEAD

    index_type xnum, ynum, num_segments;
    HistoryBufSegment *segments;
    PagerHistoryBuf *pagerhist;
    Line *line;
    index_type start_of_data, count;
} HistoryBuf;

/**
 * カーソル
 */
typedef struct {
    PyObject_HEAD

    /**
     * スタイル
     *  - ボールド
     *  - イタリック
     *  - 反転
     *  - 取り消し線
     *  - 点滅
     *  - 薄暗い
     */
    bool bold, italic, reverse, strikethrough, blink, dim;

    /**
     * 位置
     */
    unsigned int x, y;

    /**
     * デコレーション
     *  TODO: 謎
     */
    uint8_t decoration;

    /**
     * 形状
     */
    CursorShape shape;

    /**
     * 色
     *  - 前景色
     *  - 背景色
     *  - 装飾色
     */
    color_type fg, bg, decoration_fg;
} Cursor;

typedef struct {
    bool is_visible, is_focused;
    CursorShape shape;
    unsigned int x, y;
    color_type color;
} CursorRenderInfo;

typedef struct {
    color_type default_fg, default_bg, cursor_color, cursor_text_color, cursor_text_uses_bg, highlight_fg, highlight_bg;
} DynamicColor;

typedef struct {
    PyObject_HEAD

    bool dirty;
    uint32_t color_table[256];
    uint32_t orig_color_table[256];
    DynamicColor dynamic_color_stack[10];
    size_t dynamic_color_stack_idx;
    DynamicColor configured, overridden;
} ColorProfile;

typedef struct {
    unsigned int width, height;
} CellPixelSize;

typedef struct {int x;
} *SPRITE_MAP_HANDLE;

#define FONTS_DATA_HEAD \
    SPRITE_MAP_HANDLE sprite_map; \
    double logical_dpi_x, logical_dpi_y; \
    double font_sz_in_pts; \
    unsigned int cell_width, cell_height;

/**
 * フォントデータハンドル型
 *
 * - sprite_map: スプライトマップ
 * - logical_dpi_x: 論理DPIのx
 * - logical_dpi_x: 論理DPIのx
 * - font_sz_in_pts: フォントポイントサイズ
 * - cell_width: セル幅
 * - cell_height: セル高
 */
typedef struct {FONTS_DATA_HEAD} *FONTS_DATA_HANDLE;

#define PARSER_BUF_SZ   (8 * 1024)
#define READ_BUF_SZ     (1024 *1024)

/**
 * GPUセルのスプライト座標をクリアする
 */
#define clear_sprite_position(cell) \
    (cell).sprite_x = 0; \
    (cell).sprite_y = 0; \
    (cell).sprite_z = 0;

#define left_shift_line(line, at, num)       { \
        for (index_type __i__ = (at); __i__ < (line)->xnum - (num); __i__ ++) { \
            COPY_CELL(line, __i__ + (num), line, __i__) \
        } \
        if ((((line)->gpu_cells[(at)].attrs) & WIDTH_MASK) != 1) { \
            (line)->cpu_cells[(at)].ch = BLANK_CHAR; \
            (line)->gpu_cells[(at)].attrs = BLANK_CHAR ? 1 : 0; \
            clear_sprite_position((line)->gpu_cells[(at)]); \
        } \
}

#define ensure_space_for(base, array, type, num, capacity, initial_cap, zero_clear) do { \
    if ((base)->capacity < num) { \
        size_t _newcap = MAX((size_t)initial_cap, MAX(2 * (base)->capacity, (size_t)num)); \
        (base)->array = realloc((base)->array, sizeof(type) *_newcap); \
        if ((base)->array == NULL) \
            fatal("Out of memory while ensuring space for %zu elements in array of %s", (size_t)num, #type); \
        if (zero_clear) \
            memset((base)->array + (base)->capacity, 0, sizeof(type) *(_newcap - (base)->capacity)); \
        (base)->capacity = _newcap; \
    } \
} while (false)

#define remove_i_from_array(array, i, count) \
    do { \
        (count)--; \
        if ((i) < (count)) { \
            memmove((array) + (i), (array) + (i) + 1, sizeof((array)[0]) *((count) - (i))); \
        } \
    } while (false)

// Global functions
const char * base64_decode(const uint32_t *src, size_t src_sz, uint8_t *dest, size_t dest_capacity, size_t *dest_sz);
Line * alloc_line(void);
Cursor * alloc_cursor(void);
LineBuf * alloc_linebuf(unsigned int, unsigned int);
HistoryBuf * alloc_historybuf(unsigned int, unsigned int, unsigned int);
ColorProfile * alloc_color_profile(void);
void copy_color_profile(ColorProfile *, ColorProfile *);
PyObject * create_256_color_table(void);
PyObject * parse_bytes_dump(PyObject UNUSED *, PyObject *);
PyObject * parse_bytes(PyObject UNUSED *, PyObject *);
void cursor_reset(Cursor *);
Cursor * cursor_copy(Cursor *);
void cursor_copy_to(Cursor *src, Cursor *dest);
void cursor_reset_display_attrs(Cursor *);
void cursor_from_sgr(Cursor *self, unsigned int *params, unsigned int count);
void apply_sgr_to_cells(GPUCell *first_cell, unsigned int cell_count, unsigned int *params, unsigned int count);
const char * cell_as_sgr(const GPUCell *, const GPUCell *);
const char * cursor_as_sgr(const Cursor *);

PyObject * cm_thread_write(PyObject *self, PyObject *args);
bool schedule_write_to_child(unsigned long id, unsigned int num, ...);
bool set_iutf8(int, bool);

color_type colorprofile_to_color(ColorProfile *self, color_type entry, color_type defval);
float cursor_text_as_bg(ColorProfile *self);
void copy_color_table_to_buffer(ColorProfile *self, color_type *address, int offset, size_t stride);
void colorprofile_push_dynamic_colors(ColorProfile *);
void colorprofile_pop_dynamic_colors(ColorProfile *);

void set_mouse_cursor(MouseShape);
void enter_event(void);
void mouse_event(int, int, int);
void focus_in_event(void);
void scroll_event(double, double, int);
void fake_scroll(int, bool);
void set_special_key_combo(int glfw_key, int mods, bool is_native);
void on_key_input(GLFWkeyevent *ev);
void request_window_attention(id_type, bool);

#ifndef __APPLE__
void play_canberra_sound(const char *which_sound, const char *event_id);

#endif
SPRITE_MAP_HANDLE alloc_sprite_map(unsigned int, unsigned int);
SPRITE_MAP_HANDLE free_sprite_map(SPRITE_MAP_HANDLE);

static inline void safe_close(int fd) {
    while (close(fd) != 0 && errno == EINTR) {
        ;
    }
}

void log_event(const char *format, ...) __attribute__((format(printf, 1, 2)));
