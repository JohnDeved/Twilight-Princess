/*
 * fs_gx_decal.sc - DECAL fragment shader
 *
 * GX_DECAL: Output = blend vertex color under texture using texture alpha
 * TEV: color = TEXC * TEXA + RASC * (1-TEXA), alpha = RASA
 */

$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

void main()
{
    vec4 texColor = texture2D(s_texColor, v_texcoord0);
    vec3 decaled = mix(v_color0.rgb, texColor.rgb, texColor.a);
    gl_FragColor = vec4(decaled, v_color0.a);
}
