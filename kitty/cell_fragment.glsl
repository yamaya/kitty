#version GLSL_VERSION
#define WHICH_PROGRAM
#define NOT_TRANSPARENT

#if defined(SIMPLE) || defined(BACKGROUND) || defined(SPECIAL)
#define NEEDS_BACKROUND
#endif

#if defined(SIMPLE) || defined(FOREGROUND)
#define NEEDS_FOREGROUND
#endif

#ifdef NEEDS_BACKROUND
in vec3 background;
#if defined(TRANSPARENT) || defined(SPECIAL)
in float bg_alpha;
#endif
#endif

#ifdef NEEDS_FOREGROUND
uniform sampler2DArray sprites;
in float effective_text_alpha;
in vec3 sprite_pos;
in vec3 underline_pos;
in vec3 cursor_pos;
in vec3 strike_pos;
in vec3 foreground;
in vec4 cursor_color_vec;
in vec3 decoration_fg;
in float colored_sprite;
#endif

out vec4 final_color;

// 便利関数 {{{
vec4 alpha_blend(vec3 over, float over_alpha, vec3 under, float under_alpha) {
    // アルファは、2つの色をブレンドして、アルファとアルファで事前に乗算された
    // 結果の色を返します。
    // See https://en.wikipedia.org/wiki/Alpha_compositing
    float alpha = mix(under_alpha, 1.0f, over_alpha);
    vec3 combined_color = mix(under * under_alpha, over, over_alpha);
    return vec4(combined_color, alpha);
}

vec3 premul_blend(vec3 over, float over_alpha, vec3 under) {
    return over + (1.0f - over_alpha) * under;
}

vec4 alpha_blend_premul(vec3 over, float over_alpha, vec3 under, float under_alpha) {
    // alpha_blend()と同じですが、overとunderの両方が事前乗算されていると想定し
    // ています。
    float alpha = mix(under_alpha, 1.0f, over_alpha);
    return vec4(premul_blend(over, over_alpha, under), alpha);
}

vec4 blend_onto_opaque_premul(vec3 over, float over_alpha, vec3 under) {
    // under_alpha = 1のalpha_blend_premulと同じです。alphaが1であるため、実質
    // 的に事前乗算されたalpha 1の混合色を出力します。
    return vec4(premul_blend(over, over_alpha, under), 1.0);
}


// }}}


/*
 * レンダリングの説明:
 *
 * 複雑さが増す順に、いくつかのケースがあります:
 *
 * 1) Simple -- このパスは、画像がない場合、または、すべての画像がテキストの上
 *    に描画され、背景が不透明な場合に使用されます。
 *    この場合、このシェーダーには、セルの前景色と背景色が直接ブレンドされた単
 *    一のパスがあります。
 *    期待する出力は、アルファが事前に乗算された色であり、アルファも指定されて
 *    います。
 *
 * 2) Interleaved -- このパスは、背景が不透明ではなく画像がある場合、または背景
 *    が不透明でテキストの下に画像がある場合に使用されます。
 *    レンダリングは、背景と前景を別々に描画してブレンドする複数のパスで行われ
 *    ます。
 *    2a) テキストの下に画像がある不透明背景
 *        複数あり、各パスは不透明ブレンド関数（アルファ、1-アルファ）を使用して前のパスにブレンドされます。
 *        1) 背景のみ描画 -- 期待される出力はalphaが1の色です
 *        2) テキストの下にあるはずの画像を描画します。これはグラフィックシェーダで発生する。
 *        3) 特殊セルを描画します（選択/カーソル）。出力はステップ1と同じで、bg_alphaは特別なセルの場合は1、それ以外の場合は0です。
 *        4) 前景を描画 -- 期待される出力は、不透明ブレンド関数を使用してブレンドされるアルファ付きの色です
 *        5) グラフィックシェーダでテキストの上にあるはずの画像を再度描画します
 *
 *    2b) 画像付きの透明な背景
 *        最初にすべてがフレームバッファにレンダリングされ、次にフレームバフフ
 *        ァーが画面にブレンドされます。
 *        フレームバッファは、背景色ピクセルにアクセスして画像ピクセルとブレン
 *        ドできるため、必要です。
 *        手順は基本的に2aと同じです。
 *
 * このシェーダーでは、上からの適切なレンダリングパスに対応して、SIMPLE、
 * SPECIAL、FOREGROUND、またはBackgroundのどれかが（プリプロセッサディレクティ
 * ブとして）定義されます。
 */
#ifdef NEEDS_FOREGROUND
vec4 calculate_foreground() {
    // 有効な前景色を事前乗算形式で返します
    vec4 text_fg = texture(sprites, sprite_pos);
    vec3 fg = mix(foreground, text_fg.rgb, colored_sprite);
    float text_alpha = text_fg.a;
    float underline_alpha = texture(sprites, underline_pos).a;
    float strike_alpha = texture(sprites, strike_pos).a;
    float cursor_alpha = texture(sprites, cursor_pos).a;

    // 取り消しとテキストは同じ色なので、アルファ値を追加するだけです
    float combined_alpha = min(text_alpha + strike_alpha, 1.0f);

    // 下線の色が異なる場合があるため、アルファブレンドする
    vec4 ans = alpha_blend(fg, combined_alpha * effective_text_alpha, decoration_fg, underline_alpha * effective_text_alpha);
    return mix(ans, cursor_color_vec, cursor_alpha);
}
#endif

void main() {
#ifdef SIMPLE
    vec4 fg = calculate_foreground();
#ifdef TRANSPARENT
    final_color = alpha_blend_premul(fg.rgb, fg.a, background.rgb * bg_alpha, bg_alpha);
#else
    final_color = blend_onto_opaque_premul(fg.rgb, fg.a, background.rgb);
#endif
#endif // SIMPLE

#ifdef SPECIAL
#ifdef TRANSPARENT
    final_color = vec4(background.rgb * bg_alpha, bg_alpha);
#else
    final_color = vec4(background.rgb, bg_alpha);
#endif
#endif // SPECIAL

#ifdef BACKGROUND
#ifdef TRANSPARENT
    final_color = vec4(background.rgb * bg_alpha, bg_alpha);
#else
    final_color = vec4(background.rgb, 1.0f);
#endif
#endif // BACKGROUND

#ifdef FOREGROUND
    vec4 fg = calculate_foreground();  // 事前乗算された前景
#ifdef TRANSPARENT
    final_color = fg;
#else
    final_color = vec4(fg.rgb / fg.a, fg.a);
#endif

#endif // FOREGROUND

}
