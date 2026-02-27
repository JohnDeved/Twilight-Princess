/*
 * fs_gx_passclr.sc - PASSCLR fragment shader
 *
 * GX_PASSCLR: Output = vertex color (no texture)
 * TEV: color = RASC, alpha = RASA
 */

$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    gl_FragColor = v_color0;
}
