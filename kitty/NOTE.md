## ~/SandBox/vim/kitty/kitty/core_text.m

### hb_glyph_info_t *info,

hb_glyph_info_t

見てるのは下の一つだけっぽい。

- codepoint

### hb_glyph_position_t *hb_positions,

hb_glyph_position_t 見てるのは下の二つだけっぽい。

- x_offset
- y_offset

どちらも font.c group_state static変数に格納されている値が渡り渡って来ており、
group_stateへは、shape 関数がセットする。

