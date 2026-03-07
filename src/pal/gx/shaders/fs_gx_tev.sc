/*
 * fs_gx_tev.sc - Generic TEV uber-shader (Recipe 2 + Recipe 4)
 *
 * Evaluates the GX TEV combiner as one of 6 modes selected by uniform.
 * Also applies alpha compare/discard per GXSetAlphaCompare state.
 *
 * Uniforms:
 *   u_tevConfig.x  = TEV mode: 0=PASSCLR 1=REPLACE 2=MODULATE 3=BLEND 4=DECAL 5=GENERIC
 *   u_tevReg0      = TEV color register 0 (C0/mBlack)
 *   u_tevReg1      = TEV color register 1 (C1/mWhite)
 *   u_alphaTest.x  = ref0 (0.0-1.0)
 *   u_alphaTest.y  = comp0 (float: 0=NEVER..7=ALWAYS)
 *   u_alphaTest.z  = comp1 (float: 0=NEVER..7=ALWAYS)
 *   u_alphaTest.w  = ref1 (0.0-1.0)
 *   u_alphaOp.x    = op (float: 0=AND 1=OR 2=XOR 3=XNOR)
 *   u_alphaOp.y    = enable (0=off, 1=on)
 */

$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

uniform vec4 u_tevConfig;
uniform vec4 u_tevReg0;
uniform vec4 u_tevReg1;
uniform vec4 u_alphaTest;
uniform vec4 u_alphaOp;

/* Tolerance for alpha comparison equality checks.
 * 0.004 ≈ 1/255, compensating for float precision loss when
 * converting u8 alpha values (0-255) to normalized floats. */
#define ALPHA_CMP_EPSILON 0.004

bool alphaComp(float val, float ref, float comp) {
    if (comp < 0.5) return false;
    if (comp > 6.5) return true;
    if (comp < 1.5) return val < ref;
    if (comp < 2.5) return abs(val - ref) < ALPHA_CMP_EPSILON;
    if (comp < 3.5) return val < ref + ALPHA_CMP_EPSILON;
    if (comp < 4.5) return val > ref;
    if (comp < 5.5) return abs(val - ref) >= ALPHA_CMP_EPSILON;
    return val > ref - ALPHA_CMP_EPSILON;
}

void main()
{
    float mode = u_tevConfig.x;
    vec4 texColor = texture2D(s_texColor, v_texcoord0);
    vec4 result;

    if (mode < 0.5) {
        result = v_color0;
    } else if (mode < 1.5) {
        result = texColor;
    } else if (mode < 2.5) {
        result = texColor * v_color0;
    } else if (mode < 3.5) {
        vec3 blended = mix(u_tevReg0.rgb, u_tevReg1.rgb, texColor.rgb);
        float a = mix(u_tevReg0.a, u_tevReg1.a, texColor.a);
        result = vec4(blended, a);
    } else if (mode < 4.5) {
        vec3 decaled = mix(v_color0.rgb, texColor.rgb, texColor.a);
        result = vec4(decaled, v_color0.a);
    } else {
        result = texColor * v_color0;
    }

    if (u_alphaOp.y > 0.5) {
        bool pass0 = alphaComp(result.a, u_alphaTest.x, u_alphaTest.y);
        bool pass1 = alphaComp(result.a, u_alphaTest.w, u_alphaTest.z);

        bool pass;
        float op = u_alphaOp.x;
        if (op < 0.5) pass = pass0 && pass1;
        else if (op < 1.5) pass = pass0 || pass1;
        else if (op < 2.5) pass = (pass0 != pass1);
        else pass = (pass0 == pass1);

        if (!pass) discard;
    }

    gl_FragColor = result;
}
