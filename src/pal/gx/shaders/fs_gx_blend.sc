/*
 * fs_gx_blend.sc - BLEND fragment shader
 *
 * GX_BLEND: Output = mix(vertex color, texture color, texture alpha)
 * TEV: color = TEXC * RASC + (1-TEXC) * RASC blend, alpha = TEXA * RASA
 */

$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

void main()
{
    vec4 texColor = texture2D(s_texColor, v_texcoord0);
    vec3 blended = mix(v_color0.rgb, texColor.rgb, texColor.a);
    gl_FragColor = vec4(blended, texColor.a * v_color0.a);
}
