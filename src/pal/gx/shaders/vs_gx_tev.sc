/*
 * vs_gx_tev.sc - Vertex shader for all GX TEV presets
 *
 * Transforms position by model-view-projection matrix.
 * Passes vertex color and texture coordinates to fragment shader.
 */

$input a_position, a_color0, a_texcoord0
$output v_color0, v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_color0 = a_color0;
    v_texcoord0 = a_texcoord0;
}
