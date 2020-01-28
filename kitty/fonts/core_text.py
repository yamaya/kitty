#!/usr/bin/env python3
# vim:fileencoding=utf-8
# License: GPL v3 Copyright: 2017, Kovid Goyal <kovid at kovidgoyal.net>

import re

from kitty.fast_data_types import coretext_all_fonts
from kitty.utils import log_error

attr_map = {(False, False): 'font_family',
            (True, False): 'bold_font',
            (False, True): 'italic_font',
            (True, True): 'bold_italic_font'}


# フォントマップは以下の辞書でkey-valueは以下:
#   - family_map: キーがファミリ名で値がフォント情報の辞書
#   - ps_map: キーがPostscript名で値がフォント情報の辞書
#   - full_map: キーが"ファミリ名 Postscript名"で値がフォントの辞書
def create_font_map(all_fonts):
    ans = {'family_map': {}, 'ps_map': {}, 'full_map': {}}
    for font in all_fonts:
        f = (font['family'] or '').lower()
        s = (font['style'] or '').lower()
        ps = (font['postscript_name'] or '').lower()
        ans['family_map'].setdefault(f, []).append(font)
        ans['ps_map'].setdefault(ps, []).append(font)
        ans['full_map'].setdefault(f + ' ' + s, []).append(font)
    return ans


def all_fonts_map():
    # この関数の`ans`属性にcreate_font_mapの結果を突っ込んでおく
    ans = getattr(all_fonts_map, 'ans', None)
    if ans is None:
        ans = all_fonts_map.ans = create_font_map(coretext_all_fonts())
    return ans


def list_fonts():
    for fd in coretext_all_fonts():
        f = fd['family']
        if f:
            fn = (f + ' ' + (fd['style'] or '')).strip()
            is_mono = bool(fd['monospace'])
            yield {'family': f, 'full_name': fn, 'is_monospace': is_mono}

# ファミリ名を基準にboldとitalic指示にマッチするフォントを返す
def find_best_match(family, bold=False, italic=False):
    # フォントファミリ名において連続する空白を単一の0x20に置換する
    normalized_family = re.sub(r'\s+', ' ', family.lower())

    # アクセス可能な全てのフォントについてフォントマップ(フォント情報の辞書)を生成する
    font_map = all_fonts_map()

    def score(candidate):
        style_match = 1 if candidate['bold'] == bold and candidate['italic'] == italic else 0
        monospace_match = 1 if candidate['monospace'] else 0
        return style_match, monospace_match

    # 最初に完全一致を探します
    for selector in ('ps_map', 'full_map'):
        candidates = font_map[selector].get(normalized_family)
        if candidates:
            candidates.sort(key=score)
            return candidates[-1]

    # ファミリが存在する場合はCoreTextにフォントを選択させ、そうでなければMenlo
    # にフォールバックさせます
    if normalized_family not in font_map['family_map']:
        log_error('The font {} was not found, falling back to Menlo'.format(family))
        family = 'Menlo'
    return {
        'monospace': True,
        'bold': bold,
        'italic': italic,
        'family': family
    }



# .confのフォント指定を解決する
#
#   フォント指定のautoとmonospaceを置き換える
#   - auto: mediumのファミリ名に置換する
#   - monospace: Menloに置換する
def resolve_family(f, main_family, bold=False, italic=False):
    if (bold or italic) and f == 'auto':
        f = main_family
    if f.lower() == 'monospace':
        f = 'Menlo'
    return f

# optsはkitty.confに定義された値
# - return: 辞書。キーが (bold, italic) というタプル、値がfaceオブジェクト(?)
def get_font_files(opts):
    ans = {}
    # bold, italicはbool
    for (bold, italic), name in attr_map.items():
        face = find_best_match(resolve_family(getattr(opts, name), opts.font_family, bold, italic), bold, italic)
        key = {(False, False): 'medium',
               (True, False): 'bold',
               (False, True): 'italic',
               (True, True): 'bi'}[(bold, italic)]
        ans[key] = face
        if key == 'medium':
            get_font_files.medium_family = face['family']
    return ans


def font_for_family(family):
    ans = find_best_match(resolve_family(family, get_font_files.medium_family))
    return ans, ans['bold'], ans['italic']
