/*
 * core_text.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "state.h"
#include "fonts.h"
#include "unicode-data.h"
#include <structmember.h>
#include <stdint.h>
#include <math.h>
#include <hb-coretext.h>
#include <hb-ot.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreGraphics/CGBitmapContext.h>
#import <CoreText/CTFont.h>
#include <Foundation/Foundation.h>
#include <CoreText/CoreText.h>
#import <Foundation/NSString.h>
#import <Foundation/NSDictionary.h>

/**
 * CTFace構造体
 */
typedef struct {
    PyObject_HEAD

    /*
     * em平方の座標単位
     */
    unsigned int units_per_em;

    /*
     * アセント
     * デセント
     * レッディング
     * アンダーライン位置
     * アンダーライン太さ
     * ポイントサイズ
     * スケール後のポイントサイズ
     */
    float ascent, descent, leading, underline_position, underline_thickness, point_sz, scaled_point_sz;

    /*
     * CoreText フォントオブジェクト
     */
    CTFontRef ct_font;

    /*
     * HarfBuzzフォントオブジェクト
     */
    hb_font_t *hb_font;

    /*
     * ファミリ名
     * フル名
     * ポストスクリプト名
     * パス
     */
    PyObject *family_name, *full_name, *postscript_name, *path;
} CTFace;
PyTypeObject CTFace_Type;

/**
 * CFStringをUTF-8なC文字列に変換する
 *  staticなバッファを返す点に注意
 *
 * @param src CFStringオブジェクト
 * @param free_src src引数を解放するかどうか
 * @return UTF-8なC文字列
 */
static inline char *
convert_cfstring(CFStringRef src, int free_src) {
#define SZ 4094
    static char buf[SZ + 2] = {0};
    bool ok = false;
    if (!CFStringGetCString(src, buf, SZ, kCFStringEncodingUTF8)) {
        PyErr_SetString(PyExc_ValueError, "Failed to convert CFString");
    }
    else {
        ok = true;
    }
    if (free_src) {
        CFRelease(src);
    }
    return ok ? buf : NULL;
#undef SZ
}

/**
 * CTFaceオブジェクトを初期化する
 *  ぶっちゃけCTFontから値をコピーしてるだけ
 *  初期化後にhb_fontはNULL止めされる
 *
 * @param self 初期化対象
 * @param font 属性値のコピー元
 */
static inline void
init_face(CTFace *self, CTFontRef font) {
    // hb_fontを解放する
    if (self->hb_font) {
        hb_font_destroy(self->hb_font);
    }
    self->hb_font = NULL;

    // CTFontを解放する
    if (self->ct_font) {
        CFRelease(self->ct_font);
    }
    self->ct_font = font;
    self->units_per_em = CTFontGetUnitsPerEm(self->ct_font);
    self->ascent = CTFontGetAscent(self->ct_font);
    self->descent = CTFontGetDescent(self->ct_font);
    self->leading = CTFontGetLeading(self->ct_font);
    self->underline_position = CTFontGetUnderlinePosition(self->ct_font);
    self->underline_thickness = CTFontGetUnderlineThickness(self->ct_font);
    self->scaled_point_sz = CTFontGetSize(self->ct_font);
}

/**
 * CTFaceを生成する
 *
 * @param font CTFontオブジェクト
 * @return 初期化済のCTFaceオブジェクト
 */
static inline CTFace *
ct_face(CTFontRef font) {
    // CTFaceの割当て
    CTFace *self = (CTFace *)CTFace_Type.tp_alloc(&CTFace_Type, 0);

    if (self) {
        // 初期化する
        init_face(self, font);
        // フォント名群のコピー
        self->family_name = Py_BuildValue("s", convert_cfstring(CTFontCopyFamilyName(self->ct_font), true));
        self->full_name = Py_BuildValue("s", convert_cfstring(CTFontCopyFullName(self->ct_font), true));
        self->postscript_name = Py_BuildValue("s", convert_cfstring(CTFontCopyPostScriptName(self->ct_font), true));
        // フォントファイルへのURLを設定
        NSURL *url = (NSURL *)CTFontCopyAttribute(self->ct_font, kCTFontURLAttribute);
        self->path = Py_BuildValue("s", [[url path] UTF8String]);
        [url release];
        if (self->family_name == NULL || self->full_name == NULL || self->postscript_name == NULL || self->path == NULL) {
            Py_CLEAR(self);
        }
    }
    return self;
} /* ct_face */

/**
 * CTFaceを解放する
 *
 * @param self 解放対象
 */
static void
dealloc(CTFace *self) {
    // hb_fontを解放
    if (self->hb_font) {
        hb_font_destroy(self->hb_font);
    }
    self->hb_font = NULL;

    // CTFontを解放
    if (self->ct_font) {
        CFRelease(self->ct_font);
    }
    self->ct_font = NULL;

    // フォント名群の解放
    Py_CLEAR(self->family_name);
    Py_CLEAR(self->full_name);
    Py_CLEAR(self->postscript_name);
    Py_CLEAR(self->path);

    // 自身を解放
    Py_TYPE(self)->tp_free((PyObject *)self);
} /* dealloc */

/**
 * CTFontDescriptorをPyObjectに変換する
 */
static PyObject *
font_descriptor_to_python(CTFontDescriptorRef descriptor) {
    NSURL *url = (NSURL *)CTFontDescriptorCopyAttribute(descriptor, kCTFontURLAttribute);
    NSString *psName = (NSString *)CTFontDescriptorCopyAttribute(descriptor, kCTFontNameAttribute);
    NSString *family = (NSString *)CTFontDescriptorCopyAttribute(descriptor, kCTFontFamilyNameAttribute);
    NSString *style = (NSString *)CTFontDescriptorCopyAttribute(descriptor, kCTFontStyleNameAttribute);
    NSDictionary *traits = (NSDictionary *)CTFontDescriptorCopyAttribute(descriptor, kCTFontTraitsAttribute);
    unsigned int straits = [traits[(id)kCTFontSymbolicTrait] unsignedIntValue];
    NSNumber *weightVal = traits[(id)kCTFontWeightTrait];
    NSNumber *widthVal = traits[(id)kCTFontWidthTrait];

    PyObject *ans = Py_BuildValue("{ssssssss sOsOsO sfsfsI}",
                                  "path", [[url path] UTF8String],
                                  "postscript_name", [psName UTF8String],
                                  "family", [family UTF8String],
                                  "style", [style UTF8String],
                                  "bold", (straits & kCTFontBoldTrait) != 0 ? Py_True : Py_False,
                                  "italic", (straits & kCTFontItalicTrait) != 0 ? Py_True : Py_False,
                                  "monospace", (straits & kCTFontMonoSpaceTrait) != 0 ? Py_True : Py_False,
                                  "weight", [weightVal floatValue],
                                  "width", [widthVal floatValue],
                                  "traits", straits);

    [url release];
    [psName release];
    [family release];
    [style release];
    [traits release];
    return ans;
} /* font_descriptor_to_python */

/**
 * PyObjectをCTFontDescriptorに変換する
 */
static CTFontDescriptorRef
font_descriptor_from_python(PyObject *src) {
    CTFontSymbolicTraits symbolic_traits = 0;
    NSMutableDictionary *attrs = [NSMutableDictionary dictionary];
    PyObject *t = PyDict_GetItemString(src, "traits");

    // PyObjectのtraitsからCTFontSymbolicTraitsを構築する
    if (!t) {
        symbolic_traits = (
            (PyDict_GetItemString(src, "bold") == Py_True ? kCTFontBoldTrait : 0) |
            (PyDict_GetItemString(src, "italic") == Py_True ? kCTFontItalicTrait : 0) |
            (PyDict_GetItemString(src, "monospace") == Py_True ? kCTFontMonoSpaceTrait : 0));
    }
    else {
        symbolic_traits = PyLong_AsUnsignedLong(t);
    }
    NSDictionary *traits = @{(id)kCTFontSymbolicTrait:[NSNumber numberWithUnsignedInt:symbolic_traits]};
    attrs[(id)kCTFontTraitsAttribute] = traits;

    // PyObjectからフォント名群をコピーする
#define SET(x, attr) \
    t = PyDict_GetItemString(src, #x); \
    if (t) attrs[(id)attr] = @(PyUnicode_AsUTF8(t));

    SET(family, kCTFontFamilyNameAttribute);
    SET(style, kCTFontStyleNameAttribute);
    SET(postscript_name, kCTFontNameAttribute);
#undef SET

    return CTFontDescriptorCreateWithAttributes((CFDictionaryRef)attrs);
} /* font_descriptor_from_python */

/**
 * CTFontCollectionオブジェクト
 *  キャッシュしておく
 */
static CTFontCollectionRef all_fonts_collection_data = NULL;

static CTFontCollectionRef
all_fonts_collection() {
    if (!all_fonts_collection_data) {
        all_fonts_collection_data = CTFontCollectionCreateFromAvailableFonts(NULL);
    }
    return all_fonts_collection_data;
}

/**
 * 全フォントを取得する
 *
 * @return (font_descriptor_to_pythonで構築された)フォント情報のnタプル
 */
static PyObject *
coretext_all_fonts(PyObject UNUSED *_self) {
    CFArrayRef matches = CTFontCollectionCreateMatchingFontDescriptors(all_fonts_collection());
    const CFIndex count = CFArrayGetCount(matches);
    PyObject *ans = PyTuple_New(count), *temp;

    if (!ans) {
        return PyErr_NoMemory();
    }
    for (CFIndex i = 0; i < count; i++) {
        temp = font_descriptor_to_python((CTFontDescriptorRef)CFArrayGetValueAtIndex(matches, i));
        if (temp == NULL) {
            Py_DECREF(ans);
            return NULL;
        }
        PyTuple_SET_ITEM(ans, i, temp);
        temp = NULL;
    }
    return ans;
}

/**
 * コードポイントに対応するグリフIDを取得する
 *
 * @param ct_font CTFontオブジェクト
 * @param ch 文字コードポイント
 * @return グリフID
 */
static inline unsigned int
glyph_id_for_codepoint_ctfont(CTFontRef ct_font, char_type ch) {
    unichar chars[2] = {0};
    CGGlyph glyphs[2] = {0};
    const int count = CFStringGetSurrogatePairForLongCharacter(ch, chars) ? 2 : 1;

    CTFontGetGlyphsForCharacters(ct_font, chars, glyphs, count);
    return glyphs[0];
}

/**
 * "Last Resort (最後の手段)"フォントかどうか判断する
 *  Last Resort Font: https://blog.antenna.co.jp/PDFTool/archives/2008/07/02/
 */
static inline bool
is_last_resort_font(CTFontRef new_font) {
    CFStringRef name = CTFontCopyPostScriptName(new_font);
    CFComparisonResult cr = CFStringCompare(name, CFSTR("LastResort"), 0);

    CFRelease(name);
    return cr == kCFCompareEqualTo;
}

/**
 * 自力でフォールバックフォントを探す
 *
 * @param current_font 現フォント
 * @param cell CPUセル
 * @return CTFontオブジェクト
 */
static inline CTFontRef
manually_search_fallback_fonts(CTFontRef current_font, CPUCell *cell) {
    CFArrayRef fonts = CTFontCollectionCreateMatchingFontDescriptors(all_fonts_collection());
    CTFontRef ans = NULL;
    const CFIndex count = CFArrayGetCount(fonts);

    for (CFIndex i = 0; i < count; i++) {
        CTFontDescriptorRef descriptor = (CTFontDescriptorRef)CFArrayGetValueAtIndex(fonts, i);
        CTFontRef new_font = CTFontCreateWithFontDescriptor(descriptor, CTFontGetSize(current_font), NULL);
        if (new_font) {
            if (!is_last_resort_font(new_font)) {
                char_type ch = cell->ch ? cell->ch : ' ';
                bool found = true;
                if (!glyph_id_for_codepoint_ctfont(new_font, ch)) {
                    found = false;
                }
                for (unsigned i = 0; i < arraysz(cell->cc_idx) && cell->cc_idx[i] && found; i++) {
                    ch = codepoint_for_mark(cell->cc_idx[i]);
                    if (!glyph_id_for_codepoint_ctfont(new_font, ch)) {
                        found = false;
                    }
                }
                if (found) {
                    ans = new_font;
                    break;
                }
            }
            CFRelease(new_font);
        }
    }
    return ans;
} /* manually_search_fallback_fonts */

/**
 * 指定された文字列に割当てられた代替フォントを探す
 *
 * @param str 文字列
 * @param old_font CTFontオブジェクト
 * @param cpu_cell CPUセル
 * @return 代替フォント
 */
static inline CTFontRef
find_substitute_face(CFStringRef str, CTFontRef old_font, CPUCell *cpu_cell) {
    // CTFontCreateForStringは、発音区別記号がフォントに結合されていて、かつ、
    // 基本文字がオリジナルのフォントにある場合、そのオリジナルのフォントを返し
    // てしまう。よって、各文字を個別に確認する必要がある。
    const CFIndex length = CFStringGetLength(str);
    CFIndex i = 0;
    CFIndex amt = length;

    while (i < length) {
        // iが指す文字のフォントを生成する
        CTFontRef new_font = CTFontCreateForString(old_font, str, CFRangeMake(i, amt));
        if (amt == length && length != 1) {
            amt = 1;
        }
        else {
            i++;
        }
        if (!new_font) {
            PyErr_SetString(PyExc_ValueError, "Failed to find fallback CTFont");
            return NULL;
        }
        // 同一フォントなら次のフォントを試す
        if (new_font == old_font) {
            CFRelease(new_font);
            continue;
        }
        // 頼みの綱フォントか？
        if (is_last_resort_font(new_font)) {
            CFRelease(new_font);
            // コードポイントは私的利用領域内か？
            if (is_private_use(cpu_cell->ch)) {
                // CoreTextのフォントフォールバック機構は私的利用文字では動作しない
                // (なので自力で探す)
                new_font = manually_search_fallback_fonts(old_font, cpu_cell);
                if (new_font) {
                    return new_font;
                }
            }
            PyErr_SetString(PyExc_ValueError, "Failed to find fallback CTFont other than the LastResort font");
            return NULL;
        }
        return new_font;
    }
    PyErr_SetString(PyExc_ValueError, "CoreText returned the same font as a fallback font");
    return NULL;
} /* find_substitute_face */

/**
 * フォールバックフォントのCTFaceオブジェクトを生成する
 *
 * @param base_face 基準CTFace
 * @param cell CPUセル
 * @param bold ボールド
 * @param italic イタリック
 * @param emoji_presentation 絵文字
 * @param fg フォントデータハンドル
 * @return CTFaceのPyObject
 */
PyObject *
create_fallback_face(PyObject *base_face,
                     CPUCell *cell,
                     bool UNUSED bold,
                     bool UNUSED italic,
                     bool emoji_presentation,
                     FONTS_DATA_HANDLE fg UNUSED) {
    CTFace *self = (CTFace *)base_face;
    CTFontRef new_font;

    /*
     * 絵文字であれば無条件に"AppleColorEmoji"を、そうでなければCPUセルの文字コ
     * ードにマッチする代替フォントを返す
     */
    if (emoji_presentation) {
        new_font = CTFontCreateWithName((CFStringRef)@"AppleColorEmoji", self->scaled_point_sz, NULL);
    }
    else {
        char text[64] = {0};
        cell_as_utf8_for_fallback(cell, text);
        CFStringRef str = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
        if (!str) {
            return PyErr_NoMemory();
        }
        new_font = find_substitute_face(str, self->ct_font, cell);
        CFRelease(str);
    }

    if (!new_font) {
        return NULL;
    }
    return (PyObject *)ct_face(new_font);
} /* create_fallback_face */

/**
 * コードポイントに対応するグリフIDを取得する
 *
 * @param s CTFaceなPyObject
 * @param ch 文字コードポイント
 * @return グリフID
 */
unsigned int
glyph_id_for_codepoint(PyObject *s, char_type ch) {
    CTFace *self = (CTFace *)s;

    return glyph_id_for_codepoint_ctfont(self->ct_font, ch);
}

/**
 * グリフが空(= 幅が0以下)かどうかを判定する
 *
 * @param s CTFaceなPyObject
 * @param g グリフのインデックス
 * @return 真偽値
 */
bool
is_glyph_empty(PyObject *s, glyph_index g) {
    CTFace *self = (CTFace *)s;
    CGGlyph gg = g;
    CGRect bounds;

    CTFontGetBoundingRectsForGlyphs(self->ct_font, kCTFontOrientationHorizontal, &gg, &bounds, 1);
    return bounds.size.width <= 0;
}

/**
 * グリフの幅を得る
 *
 * @param s CTFaceなPyObject
 * @param g グリフのインデックス
 * @return 整数値
 */
int
get_glyph_width(PyObject *s, glyph_index g) {
    CTFace *self = (CTFace *)s;
    CGGlyph gg = g;
    CGRect bounds;

    CTFontGetBoundingRectsForGlyphs(self->ct_font, kCTFontOrientationHorizontal, &gg, &bounds, 1);
    return (int)ceil(bounds.size.width);
}

/**
 * スケールしたフォントポイントサイズを得る
 *
 * @param fg フォントデータハンドル
 * @return ポイントサイズ
 */
static inline float
scaled_point_sz(FONTS_DATA_HANDLE fg) {
    return ((fg->logical_dpi_x + fg->logical_dpi_y) / 144.0) * fg->font_sz_in_pts;
}

/**
 * フォントのポイントサイズを設定する
 *
 * @param s CTFaceなPyObject
 * @param desired_height 希望する高さ
 * @param force 強制的に設定するか否か
 * @param fg フォントデータハンドル
 * @return 設定出来たら真
 */
bool
set_size_for_face(PyObject *s, unsigned int UNUSED desired_height, bool force, FONTS_DATA_HANDLE fg) {
    CTFace *self = (CTFace *)s;
    const float sz = scaled_point_sz(fg);

    if (!force && self->scaled_point_sz == sz) {
        return true;
    }

    // 新しいCTFontを生成してCTFaceを再初期化する
    CTFontRef new_font = CTFontCreateCopyWithAttributes(self->ct_font, sz, NULL, NULL);
    if (!new_font) {
        fatal("Out of memory");
    }
    init_face(self, new_font);
    return true;
}

/**
 * CTFace内のhb_fontを取得する
 *
 * @param s CTFaceなPyObject
 * @return hb_fontオブジェクト
 */
hb_font_t *
harfbuzz_font_for_face(PyObject *s) {
    CTFace *self = (CTFace *)s;

    // NULLなら生成する
    if (!self->hb_font) {
        self->hb_font = hb_coretext_font_create(self->ct_font);
        if (!self->hb_font) {
            fatal("Failed to create hb_font");
        }
        hb_ot_font_set_funcs(self->hb_font);
    }
    return self->hb_font;
}

/**
 * セルの寸法を得る
 *
 * @param s CTFaceなPyObject
 * @param cell_width セルの幅 [out]
 * @param cell_height セルの高さ [out]
 * @param baseline ベースライン [out]
 * @param underline_position アンダーライン位置 [out]
 * @param underline_thickness アンダーライン太さ [out]
 */
void
cell_metrics(PyObject *s,
             unsigned int *cell_width,
             unsigned int *cell_height,
             unsigned int *baseline,
             unsigned int *underline_position,
             unsigned int *underline_thickness
) {
    // 参照: https://developer.apple.com/library/content/documentation/StringsTextFonts/Conceptual/TextAndWebiPhoneOS/TypoFeatures/TextSystemFeatures.html
    CTFace *self = (CTFace *)s;

#define count (128 - 32)
    // ASCII文字(128個)の中で最大の幅を cell_widthとする
    unichar chars[count + 1] = {0};
    CGGlyph glyphs[count + 1] = {0};
    unsigned int width = 0;
    for (unsigned int i = 0; i < count; i++) {
        chars[i] = 32 + i;
    }
    CTFontGetGlyphsForCharacters(self->ct_font, chars, glyphs, count);
    for (unsigned i = 0; i < count; i++) {
        if (glyphs[i]) {
            const unsigned int w = (unsigned int)(ceilf(
                                                      CTFontGetAdvancesForGlyphs(
                                                          self->ct_font,
                                                          kCTFontOrientationHorizontal,
                                                          glyphs + i,
                                                          NULL,
                                                          1)));
            if (w > width) {
                width = w;
            }
        }
    }
    *cell_width = MAX(1u, width);

    // アンダーラインの位置と太さ
    *underline_position = (unsigned int)floor(self->ascent - self->underline_position + 0.5);
    *underline_thickness = (unsigned int)ceil(MAX(0.1, self->underline_thickness));

    // ベースラインはAscentそのもの
    *baseline = (unsigned int)self->ascent;

    // float line_height = MAX(1, floor(self->ascent + self->descent + MAX(0, self->leading) + 0.5));
    // CoreTextのレイアウトエンジンで行の高さを求める。遅いけどより正確な値が出る。
#define W "AQWMH_gyl "
    CFStringRef ts = CFSTR(
        W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W W);
#undef W
    CFMutableAttributedStringRef test_string = CFAttributedStringCreateMutable(kCFAllocatorDefault, CFStringGetLength(ts));
    CFAttributedStringReplaceString(test_string, CFRangeMake(0, 0), ts);
    CFAttributedStringSetAttribute(test_string, CFRangeMake(0, CFStringGetLength(ts)), kCTFontAttributeName, self->ct_font);
    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddRect(path, NULL, CGRectMake(10, 10, 200, 200));
    CTFramesetterRef framesetter = CTFramesetterCreateWithAttributedString(test_string);
    CFRelease(test_string);
    CTFrameRef test_frame = CTFramesetterCreateFrame(framesetter, CFRangeMake(0, 0), path, NULL);
    CGPoint origin1, origin2;
    CTFrameGetLineOrigins(test_frame, CFRangeMake(0, 1), &origin1);
    CTFrameGetLineOrigins(test_frame, CFRangeMake(1, 1), &origin2);
    CGFloat line_height = origin1.y - origin2.y;
    CFRelease(test_frame);
    CFRelease(path);
    CFRelease(framesetter);
    *cell_height = MAX(4u, (unsigned int)ceilf(line_height));
#undef count
}

/**
 * フォントデスクリプタからCTFaceを生成する
 *
 * @param descriptor フォントデスクリプタ(PyObject)
 * @param fg フォントデータハンドル
 * @return CTFace(PyObject)
 */
PyObject *
face_from_descriptor(PyObject *descriptor, FONTS_DATA_HANDLE fg) {
    CTFontDescriptorRef desc = font_descriptor_from_python(descriptor);

    if (!desc) {
        return NULL;
    }

    CTFontRef font = CTFontCreateWithFontDescriptor(desc, scaled_point_sz(fg), NULL);
    CFRelease(desc);
    desc = NULL;
    if (!font) {
        PyErr_SetString(PyExc_ValueError, "Failed to create CTFont object");
        return NULL;
    }
    return (PyObject *)ct_face(font);
}

/**
 * フォントファイルからCTFaceを生成する
 *
 * @param path フォントファイルへのパス
 * @param index ？
 * @param fg フォントデータハンドル
 * @return CTFace(PyObject)
 */
PyObject *
face_from_path(const char *path, int UNUSED index, FONTS_DATA_HANDLE fg UNUSED) {
    CFStringRef s = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, s, kCFURLPOSIXPathStyle, false);
    CGDataProviderRef dp = CGDataProviderCreateWithURL(url);
    CGFontRef cg_font = CGFontCreateWithDataProvider(dp);
    CTFontRef ct_font = CTFontCreateWithGraphicsFont(cg_font, 0.0, NULL, NULL);

    CFRelease(cg_font);
    CFRelease(dp);
    CFRelease(url);
    CFRelease(s);
    return (PyObject *)ct_face(ct_font);
}

/**
 * フォントデスクリプタを特殊化する
 *
 * @param base_descriptor 基準となるフォントデスクリプタ
 * @param fg フォントデータハンドル
 * @return フォントデスクリプタ
 */
PyObject *
specialize_font_descriptor(PyObject *base_descriptor, FONTS_DATA_HANDLE fg UNUSED) {
    // CoreText実装では何もしない
    // 参照カウントを増やすだけ
    Py_INCREF(base_descriptor);
    return base_descriptor;
}

/**
 * レンダリングバッファ
 *  なんでstatic...
 */
static uint8_t *render_buf = NULL;

/**
 * レンダリングバッファのサイズ
 *  staticなんだよなぁ
 */
static size_t render_buf_sz = 0;

/**
 * グリフの配列
 *  なんでstatic...
 */
static CGGlyph glyphs[128];

/**
 * 矩形の配列
 *  なんでstatic...
 */
static CGRect boxes[128];

/**
 * グリフの出力位置の配列
 *  なんでstatic...
 */
static CGPoint positions[128];

/**
 * レンダリングの終了処理
 */
static void
finalize(void) {
    // レンダリングバッファとフォントコレクションを解放する
    free(render_buf);
    if (all_fonts_collection_data) {
        CFRelease(all_fonts_collection_data);
    }
}

/**
 * カラーグリフをレンダリングする
 *
 * @param font CTFontオブジェクト
 * @param buf レンダリング出力先バッファ
 * @param glyph_id グリフID
 * @param width レンダリング出力先の幅
 * @param height 同じく高さ
 * @param baseline ベースライン
 */
static inline void
render_color_glyph(CTFontRef font, uint8_t *buf, int glyph_id, unsigned int width, unsigned int height, unsigned int baseline) {
    // デバイスRGBの色空間でビットマップコンテキストを作る
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    if (!color_space) {
        fatal("Out of memory");
    }
    CGContextRef ctx = CGBitmapContextCreate(
        buf,
        width,
        height,
        8,
        4 * width,
        color_space,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault);
    if (!ctx) {
        fatal("Out of memory");
    }

    // グラフィックコンテキストの設定
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, true);  // サブピクセルアンチエイリアス
    CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
    CGContextSetTextDrawingMode(ctx, kCGTextFill);
    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);

    // 絵文字を少し下げて、ベースラインに合わせます
    CGContextSetTextPosition(ctx, -boxes[0].origin.x, MAX(2, height - 1.2f * baseline));

    // グリフを描画する
    const CGGlyph glyph[] = {glyph_id};
    CTFontDrawGlyphs(font, glyph, &CGPointZero, 1, ctx);

    // リソースの解放
    CGContextRelease(ctx);
    CGColorSpaceRelease(color_space);

    // エンディアン変換？
    for (size_t r = 0; r < width; r++) {
        for (size_t c = 0; c < height; c++, buf += 4) {
            uint32_t px = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
            *((pixel *)buf) = px;
        }
    }
}

/**
 * レンダリングバッファを確保する
 *
 *  レンダリングバッファはstaticに確保しているので指定された幅高さが収まらないサ
 *  イズなら再確保する。
 *
 * @param width レンダリング出力先の幅
 * @param height 同じく高さ
 */
static inline void
ensure_render_space(size_t width, size_t height) {
    if (render_buf_sz >= width * height) {
        return;
    }
    free(render_buf);
    render_buf_sz = width * height;
    render_buf = malloc(render_buf_sz);
    if (!render_buf) {
        fatal("Out of memory");
    }
}

/**
 * グリフをレンダリングする
 *
 *  staticな `glyph` 配列の先頭から引数 `num_glyphs` で指定された個数のグリフを
 *  レンダリングする
 *
 * @param font CTFontオブジェクト
 * @param width レンダリング出力先の幅
 * @param height 同じく高さ
 * @param baseline ベースライン
 * @param num_glyphs グリフの数
 */
static inline void
render_glyphs(CTFontRef font, unsigned int width, unsigned int height, unsigned int baseline, unsigned int num_glyphs) {
    // レンダリングバッファのクリア
    memset(render_buf, 0, width * height);
    
    // グレー色空間のビットマップコンテキストを作成する
    CGColorSpaceRef gray_color_space = CGColorSpaceCreateDeviceGray();
    if (!gray_color_space) {
        fatal("Out of memory");
    }
    CGContextRef render_ctx = CGBitmapContextCreate(
            render_buf,
            width,
            height,
            8,
            width,
            gray_color_space,
            kCGBitmapAlphaInfoMask & kCGImageAlphaNone);
    if (!render_ctx) {
        fatal("Out of memory");
    }

    // グラフィックスコンテキストの設定
    CGContextSetShouldAntialias(render_ctx, true);
    CGContextSetShouldSmoothFonts(render_ctx, true);
    CGContextSetGrayFillColor(render_ctx, 1, 1); // 白
    CGContextSetGrayStrokeColor(render_ctx, 1, 1);
    CGContextSetLineWidth(render_ctx, OPT(macos_thicken_font));
    CGContextSetTextDrawingMode(render_ctx, kCGTextFillStroke);
    CGContextSetTextMatrix(render_ctx, CGAffineTransformIdentity);
    CGContextSetTextPosition(render_ctx, 0, height - baseline);

    // グリフの描画
    CTFontDrawGlyphs(font, glyphs, positions, num_glyphs, render_ctx);

    // リソース解放
    CGContextRelease(render_ctx);
    CGColorSpaceRelease(gray_color_space);
}

/**
 * 単純なテキストをレンダリングする
 *
 * @param s CTFaceオブジェクト
 * @param text 文字列
 * @param baseline ベースライン
 * @return StringCanvasオブジェクト
 * \note font.sの render_simple_text_impl から呼ばれる
 */
StringCanvas
render_simple_text_impl(PyObject *s, const char *text, unsigned int baseline) {
    CTFace *self = (CTFace *)s;
    CTFontRef font = self->ct_font;

    // textをunichar配列にコピーする
    const size_t num_chars = strnlen(text, 32);
    unichar chars[num_chars];
    for (size_t i = 0; i < num_chars; i++) {
        chars[i] = text[i];
    }

    // グリフを得る(staticな配列にいれる)
    CTFontGetGlyphsForCharacters(font, chars, glyphs, num_chars);

    // アドバンスを得る
    CGSize advances[num_chars];
    CTFontGetAdvancesForGlyphs(font, kCTFontOrientationDefault, glyphs, advances, num_chars);

    // バウンディングボックスを得る
    CGRect bounding_box = CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationDefault, glyphs, boxes, num_chars);

    // 戻り値を用意する
    StringCanvas ans = {
        .width = 0,
        .height = (size_t)(2 * bounding_box.size.height)
    };

    // staticなpositions配列にグリフ位置をセットする
    // x = アドバンス幅の足し上げ
    // y = 初期値+ アドバンス高さ
    for (size_t i = 0, y = 0; i < num_chars; i++) {
        positions[i] = CGPointMake(ans.width, y);
        ans.width += advances[i].width;
        y += advances[i].height;
    }

    // レンダリング出力先バッファの確保
    ensure_render_space(ans.width, ans.height);

    // グリフをレンダリングする
    render_glyphs(font, ans.width, ans.height, baseline, num_chars);

    // キャンバス領域を確保してレンダリング結果をコピーする
    ans.canvas = malloc(ans.width * ans.height);
    if (ans.canvas) {
        memcpy(ans.canvas, render_buf, ans.width * ans.height);
    }

    return ans;
}

/**
 * レンダリングする
 *
 * @param ct_font CTFontオブジェクト
 * @param bold ボールド
 * @param italic イタリック
 * @param info HarfBuzzグリフ情報 ... これって色付けグリフじゃないと使用しないっぽい
 * @param hb_positions HarfBuzz位置情報
 * @param num_glyphs グリフの数
 * @param canvas レンダリングの出力先ピクセルバッファ
 * @param cell_width セルの幅
 * @param cell_height セルの高さ
 * @param num_cells セルの個数
 * @param baseline ベースライン
 * @param was_colored 色付けされたか否か
 * @param allow_resize リサイズを許すか否か
 * @param fg フォントデータハンドル
 * @param center_glyph レンダリング位置を中央にする
 * @return レンダリング出来たら真 ... だけどこの実装は真固定で返してる
 */
static inline bool
do_render(CTFontRef ct_font,
          bool bold,
          bool italic,
          hb_glyph_info_t *info,
          hb_glyph_position_t *hb_positions,
          unsigned int num_glyphs,
          pixel *canvas,
          unsigned int cell_width,
          unsigned int cell_height,
          unsigned int num_cells,
          unsigned int baseline,
          bool *was_colored,
          bool allow_resize,
          FONTS_DATA_HANDLE fg,
          bool center_glyph
) {
    // キャンバスの幅を求める
    const unsigned int canvas_width = cell_width * num_cells;

    // グリフ群を囲む矩形を求める
    const CGRect br = CTFontGetBoundingRectsForGlyphs(ct_font, kCTFontOrientationHorizontal, glyphs, boxes, num_glyphs);

    if (allow_resize) {
        /*
         * フォントサイズを拡大縮小して、隣接するセルににじむ(?)グリフのサイズを変更します
         */
        float right = 0; // グリフの最右座標
        for (unsigned i = 0; i < num_glyphs; i++) {
            right = MAX(right, boxes[i].origin.x + boxes[i].size.width);
        }
        // グリフの最右がキャンパスをはみ出す場合、フォントサイズを小さくして
        // `do_render` を再実行する
        if (!bold && !italic && right > canvas_width + 1) {
            CGFloat sz = CTFontGetSize(ct_font);
            sz *= canvas_width / right;
            CTFontRef new_font = CTFontCreateCopyWithAttributes(ct_font, sz, NULL, NULL);
            bool ret = do_render(new_font,
                                 bold,
                                 italic,
                                 info,
                                 hb_positions,
                                 num_glyphs,
                                 canvas,
                                 cell_width,
                                 cell_height,
                                 num_cells,
                                 baseline,
                                 was_colored,
                                 false,
                                 fg,
                                 center_glyph); // 再帰だよ
            CFRelease(new_font);
            return ret;
        }
    }

    // staticな位置配列を HarfBuzz位置情報で更新する
    for (unsigned i = 0; i < num_glyphs; i++) {
        positions[i].x = MAX(0, -boxes[i].origin.x) + hb_positions[i].x_offset / 64.f;
        positions[i].y = hb_positions[i].y_offset / 64.f;
    }

    // 色付けグリフかどうかでレンダリング処理を変更する
    if (*was_colored) {
        render_color_glyph(ct_font,
                           (uint8_t *)canvas,
                           info[0].codepoint,
                           cell_width * num_cells,
                           cell_height,
                           baseline);
    }
    else {
        ensure_render_space(canvas_width, cell_height);
        render_glyphs(ct_font, canvas_width, cell_height, baseline, num_glyphs);
        Region src = {
            .bottom = cell_height,
            .right = canvas_width
        };
        Region dest = src;
        render_alpha_mask(render_buf, canvas, &src, &dest, canvas_width, canvas_width);
    }

    // セルから求めた幅がバウンディング矩形より大きい場合、グリフ群をセンタリングする
    if (num_cells > 1) {
        CGFloat delta = canvas_width - br.size.width;
        if (delta > 1) {
            right_shift_canvas(canvas, canvas_width, cell_height, (unsigned)(delta / 2.f));
        }
    }
    return true;
}

/**
 * セルをレンダリングする
 *
 *  HarfBuzz位置情報 `info` にコードポイントが含まれておりそれをレンダリングす
 *  る。なお `info` は配列で件数は `num_glyphs` である( `num_cells` じゃないんか... )。
 *
 * @param s CTFaceオブジェクト
 * @param bold ボールド
 * @param italic イタリック
 * @param info HarfBuzzグリフ情報
 * @param hb_positions HarfBuzz位置情報
 * @param num_glyphs グリフの数
 * @param canvas レンダリング出力先のピクセルバッファ [out]
 * @param cell_width セルの幅
 * @param cell_height セルの高さ
 * @param num_cells セルの個数
 * @param baseline ベースライン
 * @param was_colored 色付けされたか否か
 * @param fg フォントデータハンドル
 * @param center_glyph レンダリング位置を中央にする
 * @return レンダリング出来たら真 ... だけどこの実装は真固定で返してる
 * \note fonts.cのrender_groupから呼ばれる
 */
bool
render_glyphs_in_cells(PyObject *s,
                       bool bold,
                       bool italic,
                       hb_glyph_info_t *info,
                       hb_glyph_position_t *hb_positions,
                       unsigned int num_glyphs,
                       pixel *canvas,
                       unsigned int cell_width,
                       unsigned int cell_height,
                       unsigned int num_cells,
                       unsigned int baseline,
                       bool *was_colored,
                       FONTS_DATA_HANDLE fg,
                       bool center_glyph) {
    CTFace *self = (CTFace *)s;

    // HarfBuzzグリフをstatic配列にコピーする
    for (unsigned i = 0; i < num_glyphs; i++) {
        glyphs[i] = info[i].codepoint;
    }
    return do_render(self->ct_font,
                     bold,
                     italic,
                     info,
                     hb_positions,
                     num_glyphs,
                     canvas,
                     cell_width,
                     cell_height,
                     num_cells,
                     baseline,
                     was_colored,
                     true,
                     fg,
                     center_glyph);
}

// Boilerplate {{{

static PyObject *
display_name(CTFace *self) {
    CFStringRef dn = CTFontCopyDisplayName(self->ct_font);
    const char *d = convert_cfstring(dn, true);

    return Py_BuildValue("s", d);
}

static PyMethodDef methods[] = {
    METHODB(display_name, METH_NOARGS),
    {NULL}    /* Sentinel */
};

const char *
postscript_name_for_face(const PyObject *face_) {
    const CTFace *self = (const CTFace *)face_;

    if (self->postscript_name) {
        return PyUnicode_AsUTF8(self->postscript_name);
    }
    return "";
}

static PyObject *
repr(CTFace *self) {
    char buf[1024] = {0};

    snprintf(buf,
             sizeof (buf) / sizeof (buf[0]),
             "ascent=%.1f, descent=%.1f, leading=%.1f, point_sz=%.1f, scaled_point_sz=%.1f, underline_position=%.1f underline_thickness=%.1f",
             (self->ascent),
             (self->descent),
             (self->leading),
             (self->point_sz),
             (self->scaled_point_sz),
             (self->underline_position),
             (self->underline_thickness));
    return PyUnicode_FromFormat(
        "Face(family=%U, full_name=%U, postscript_name=%U, path=%U, units_per_em=%u, %s)",
        self->family_name, self->full_name, self->postscript_name, self->path, self->units_per_em, buf);
}

static PyMethodDef module_methods[] = {
    METHODB(coretext_all_fonts, METH_NOARGS),
    {NULL,                      NULL,                     0, NULL} /* Sentinel */
};

static PyMemberDef members[] = {
#define MEM(name,            type)            {#name, type, offsetof(CTFace, name), READONLY, #name}
    MEM(units_per_em,        T_UINT),
    MEM(point_sz,            T_FLOAT),
    MEM(scaled_point_sz,     T_FLOAT),
    MEM(ascent,              T_FLOAT),
    MEM(descent,             T_FLOAT),
    MEM(leading,             T_FLOAT),
    MEM(underline_position,  T_FLOAT),
    MEM(underline_thickness, T_FLOAT),
    MEM(family_name,         T_OBJECT),
    MEM(path,                T_OBJECT),
    MEM(full_name,           T_OBJECT),
    MEM(postscript_name,     T_OBJECT),
    {NULL}    /* Sentinel */
};

PyTypeObject CTFace_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fast_data_types.CTFace",
    .tp_basicsize = sizeof (CTFace),
    .tp_dealloc = (destructor)dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "CoreText Font face",
    .tp_methods = methods,
    .tp_members = members,
    .tp_repr = (reprfunc)repr,
};

int
init_CoreText(PyObject *module) {
    if (PyType_Ready(&CTFace_Type) < 0) {
        return 0;
    }
    if (PyModule_AddObject(module, "CTFace", (PyObject *)&CTFace_Type) != 0) {
        return 0;
    }
    if (PyModule_AddFunctions(module, module_methods) != 0) {
        return 0;
    }
    if (Py_AtExit(finalize) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to register the CoreText at exit handler");
        return false;
    }
    return 1;
}

// }}}
