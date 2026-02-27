/*
 * fs_gx_replace.sc - REPLACE fragment shader
 *
 * GX_REPLACE: Output = texture color
 * TEV: color = TEXC, alpha = TEXA
 */

$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

void main()
{
    gl_FragColor = texture2D(s_texColor, v_texcoord0);
}
