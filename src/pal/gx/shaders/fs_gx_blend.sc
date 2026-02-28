/*
 * fs_gx_blend.sc - BLEND fragment shader
 *
 * Multi-stage TEV: mix(u_tevReg0, u_tevReg1, texColor)
 * Used for J2DPicture mBlack/mWhite tinting (e.g. red Nintendo logo).
 * Stage 0 outputs texture color (REPLACE), stage 1 lerps between
 * TEV register C0 (mBlack) and C1 (mWhite) using the texture as factor.
 */

$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

uniform vec4 u_tevReg0;
uniform vec4 u_tevReg1;

void main()
{
    vec4 texColor = texture2D(s_texColor, v_texcoord0);
    vec3 blended = mix(u_tevReg0.rgb, u_tevReg1.rgb, texColor.rgb);
    float alpha = mix(u_tevReg0.a, u_tevReg1.a, texColor.a);
    gl_FragColor = vec4(blended, alpha) * v_color0;
}
