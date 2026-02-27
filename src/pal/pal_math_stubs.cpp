/**
 * pal_math_stubs.cpp
 * Implementations for Dolphin/Revolution math functions (PSMTXxx, PSVECxx,
 * C_MTXxx, C_VECxx) and miscellaneous hardware stubs (DCZeroRange, PPC*, etc.).
 *
 * PSMTX/PSVEC functions have REAL implementations since they are critical for
 * the game's math pipeline.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <cstring>
#include <cmath>

#include "dolphin/types.h"

/* Dolphin math types - matches include/dolphin/mtx.h */
typedef f32 Mtx[3][4];
typedef f32 Mtx33[3][3];
typedef f32 Mtx44[4][4];
typedef f32 (*MtxPtr)[4];
typedef f32 (*Mtx44Ptr)[4];

typedef struct {
    f32 x, y, z;
} Vec;

typedef struct {
    f32 x, y;
} Vec2;

typedef struct {
    s16 x, y, z;
} S16Vec;

typedef struct {
    f32 x, y, z, w;
} Quaternion;

extern "C" {

/* ================================================================ */
/* PSMTXIdentity / PSMTXCopy / PSMTXConcat                          */
/* ================================================================ */

void PSMTXIdentity(Mtx m) {
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
}

void PSMTXCopy(const Mtx src, Mtx dst) {
    memcpy(dst, src, sizeof(Mtx));
}

void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    Mtx tmp;
    int i;
    for (i = 0; i < 3; i++) {
        tmp[i][0] = a[i][0]*b[0][0] + a[i][1]*b[1][0] + a[i][2]*b[2][0];
        tmp[i][1] = a[i][0]*b[0][1] + a[i][1]*b[1][1] + a[i][2]*b[2][1];
        tmp[i][2] = a[i][0]*b[0][2] + a[i][1]*b[1][2] + a[i][2]*b[2][2];
        tmp[i][3] = a[i][0]*b[0][3] + a[i][1]*b[1][3] + a[i][2]*b[2][3] + a[i][3];
    }
    memcpy(ab, tmp, sizeof(Mtx));
}

void PSMTXConcatArray(const Mtx a, const Mtx* srcBase, Mtx* dstBase, u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        PSMTXConcat(a, srcBase[i], dstBase[i]);
    }
}

/* ================================================================ */
/* PSMTXTranspose / PSMTXInverse / PSMTXInvXpose                    */
/* ================================================================ */

void PSMTXTranspose(const Mtx src, Mtx xPose) {
    Mtx tmp;
    tmp[0][0] = src[0][0]; tmp[0][1] = src[1][0]; tmp[0][2] = src[2][0]; tmp[0][3] = 0.0f;
    tmp[1][0] = src[0][1]; tmp[1][1] = src[1][1]; tmp[1][2] = src[2][1]; tmp[1][3] = 0.0f;
    tmp[2][0] = src[0][2]; tmp[2][1] = src[1][2]; tmp[2][2] = src[2][2]; tmp[2][3] = 0.0f;
    memcpy(xPose, tmp, sizeof(Mtx));
}

u32 PSMTXInverse(const Mtx src, Mtx inv) {
    f32 det;
    Mtx tmp;

    tmp[0][0] = src[1][1]*src[2][2] - src[1][2]*src[2][1];
    tmp[0][1] = src[0][2]*src[2][1] - src[0][1]*src[2][2];
    tmp[0][2] = src[0][1]*src[1][2] - src[0][2]*src[1][1];
    tmp[1][0] = src[1][2]*src[2][0] - src[1][0]*src[2][2];
    tmp[1][1] = src[0][0]*src[2][2] - src[0][2]*src[2][0];
    tmp[1][2] = src[0][2]*src[1][0] - src[0][0]*src[1][2];
    tmp[2][0] = src[1][0]*src[2][1] - src[1][1]*src[2][0];
    tmp[2][1] = src[0][1]*src[2][0] - src[0][0]*src[2][1];
    tmp[2][2] = src[0][0]*src[1][1] - src[0][1]*src[1][0];

    det = src[0][0]*tmp[0][0] + src[0][1]*tmp[1][0] + src[0][2]*tmp[2][0];
    if (det == 0.0f) {
        PSMTXIdentity(inv);
        return 0;
    }

    f32 invDet = 1.0f / det;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            tmp[i][j] *= invDet;
        }
    }
    /* Compute translation part */
    tmp[0][3] = -(tmp[0][0]*src[0][3] + tmp[0][1]*src[1][3] + tmp[0][2]*src[2][3]);
    tmp[1][3] = -(tmp[1][0]*src[0][3] + tmp[1][1]*src[1][3] + tmp[1][2]*src[2][3]);
    tmp[2][3] = -(tmp[2][0]*src[0][3] + tmp[2][1]*src[1][3] + tmp[2][2]*src[2][3]);

    memcpy(inv, tmp, sizeof(Mtx));
    return 1;
}

u32 PSMTXInvXpose(const Mtx src, Mtx invX) {
    Mtx tmp;
    u32 ret = PSMTXInverse(src, tmp);
    if (ret) {
        PSMTXTranspose(tmp, invX);
    }
    return ret;
}

/* ================================================================ */
/* PSMTXRot / PSMTXQuat                                             */
/* ================================================================ */

void PSMTXRotRad(Mtx m, u8 axis, f32 rad) {
    f32 sinA = sinf(rad);
    f32 cosA = cosf(rad);
    PSMTXIdentity(m);

    switch (axis) {
    case 'x': case 'X':
        m[1][1] = cosA;  m[1][2] = -sinA;
        m[2][1] = sinA;  m[2][2] = cosA;
        break;
    case 'y': case 'Y':
        m[0][0] = cosA;  m[0][2] = sinA;
        m[2][0] = -sinA; m[2][2] = cosA;
        break;
    case 'z': case 'Z':
        m[0][0] = cosA;  m[0][1] = -sinA;
        m[1][0] = sinA;  m[1][1] = cosA;
        break;
    }
}

void PSMTXRotTrig(Mtx m, u8 axis, f32 sinA, f32 cosA) {
    PSMTXIdentity(m);
    switch (axis) {
    case 'x': case 'X':
        m[1][1] = cosA;  m[1][2] = -sinA;
        m[2][1] = sinA;  m[2][2] = cosA;
        break;
    case 'y': case 'Y':
        m[0][0] = cosA;  m[0][2] = sinA;
        m[2][0] = -sinA; m[2][2] = cosA;
        break;
    case 'z': case 'Z':
        m[0][0] = cosA;  m[0][1] = -sinA;
        m[1][0] = sinA;  m[1][1] = cosA;
        break;
    }
}

void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) {
    f32 sinA = sinf(rad);
    f32 cosA = cosf(rad);
    f32 t = 1.0f - cosA;
    f32 x = axis->x, y = axis->y, z = axis->z;

    m[0][0] = t*x*x + cosA;   m[0][1] = t*x*y - sinA*z; m[0][2] = t*x*z + sinA*y; m[0][3] = 0.0f;
    m[1][0] = t*x*y + sinA*z; m[1][1] = t*y*y + cosA;   m[1][2] = t*y*z - sinA*x; m[1][3] = 0.0f;
    m[2][0] = t*x*z - sinA*y; m[2][1] = t*y*z + sinA*x; m[2][2] = t*z*z + cosA;   m[2][3] = 0.0f;
}

void PSMTXQuat(Mtx m, const Quaternion* q) {
    f32 xx = q->x * q->x;
    f32 yy = q->y * q->y;
    f32 zz = q->z * q->z;
    f32 xy = q->x * q->y;
    f32 xz = q->x * q->z;
    f32 yz = q->y * q->z;
    f32 wx = q->w * q->x;
    f32 wy = q->w * q->y;
    f32 wz = q->w * q->z;

    m[0][0] = 1.0f - 2.0f*(yy+zz); m[0][1] = 2.0f*(xy-wz);        m[0][2] = 2.0f*(xz+wy);        m[0][3] = 0.0f;
    m[1][0] = 2.0f*(xy+wz);        m[1][1] = 1.0f - 2.0f*(xx+zz); m[1][2] = 2.0f*(yz-wx);        m[1][3] = 0.0f;
    m[2][0] = 2.0f*(xz-wy);        m[2][1] = 2.0f*(yz+wx);        m[2][2] = 1.0f - 2.0f*(xx+yy); m[2][3] = 0.0f;
}

/* ================================================================ */
/* PSMTXTrans / PSMTXTransApply / PSMTXScale / PSMTXScaleApply      */
/* ================================================================ */

void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) {
    PSMTXIdentity(m);
    m[0][3] = xT;
    m[1][3] = yT;
    m[2][3] = zT;
}

void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT) {
    if (src != dst) memcpy(dst, src, sizeof(Mtx));
    dst[0][3] += xT;
    dst[1][3] += yT;
    dst[2][3] += zT;
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS) {
    PSMTXIdentity(m);
    m[0][0] = xS;
    m[1][1] = yS;
    m[2][2] = zS;
}

void PSMTXScaleApply(const Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS) {
    int i;
    for (i = 0; i < 4; i++) {
        dst[0][i] = src[0][i] * xS;
        dst[1][i] = src[1][i] * yS;
        dst[2][i] = src[2][i] * zS;
    }
}

/* ================================================================ */
/* PSMTXMultVec / PSMTXMultVecArray / PSMTXMultVecSR                */
/* ================================================================ */

void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst) {
    Vec tmp;
    tmp.x = m[0][0]*src->x + m[0][1]*src->y + m[0][2]*src->z + m[0][3];
    tmp.y = m[1][0]*src->x + m[1][1]*src->y + m[1][2]*src->z + m[1][3];
    tmp.z = m[2][0]*src->x + m[2][1]*src->y + m[2][2]*src->z + m[2][3];
    *dst = tmp;
}

void PSMTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        PSMTXMultVec(m, &srcBase[i], &dstBase[i]);
    }
}

void PSMTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) {
    Vec tmp;
    tmp.x = m[0][0]*src->x + m[0][1]*src->y + m[0][2]*src->z;
    tmp.y = m[1][0]*src->x + m[1][1]*src->y + m[1][2]*src->z;
    tmp.z = m[2][0]*src->x + m[2][1]*src->y + m[2][2]*src->z;
    *dst = tmp;
}

/* ================================================================ */
/* PSVECAdd / PSVECSubtract / PSVECScale / PSVECNormalize           */
/* ================================================================ */

void PSVECAdd(const Vec* a, const Vec* b, Vec* ab) {
    ab->x = a->x + b->x;
    ab->y = a->y + b->y;
    ab->z = a->z + b->z;
}

void PSVECSubtract(const Vec* a, const Vec* b, Vec* ab) {
    ab->x = a->x - b->x;
    ab->y = a->y - b->y;
    ab->z = a->z - b->z;
}

void PSVECScale(const Vec* src, Vec* dst, f32 scale) {
    dst->x = src->x * scale;
    dst->y = src->y * scale;
    dst->z = src->z * scale;
}

void PSVECNormalize(const Vec* src, Vec* unit) {
    f32 mag = sqrtf(src->x*src->x + src->y*src->y + src->z*src->z);
    if (mag > 0.0f) {
        f32 invMag = 1.0f / mag;
        unit->x = src->x * invMag;
        unit->y = src->y * invMag;
        unit->z = src->z * invMag;
    } else {
        unit->x = unit->y = unit->z = 0.0f;
    }
}

f32 PSVECMag(const Vec* v) {
    return sqrtf(v->x*v->x + v->y*v->y + v->z*v->z);
}

f32 PSVECSquareMag(const Vec* v) {
    return v->x*v->x + v->y*v->y + v->z*v->z;
}

f32 PSVECDotProduct(const Vec* a, const Vec* b) {
    return a->x*b->x + a->y*b->y + a->z*b->z;
}

void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb) {
    Vec tmp;
    tmp.x = a->y*b->z - a->z*b->y;
    tmp.y = a->z*b->x - a->x*b->z;
    tmp.z = a->x*b->y - a->y*b->x;
    *axb = tmp;
}

f32 PSVECDistance(const Vec* a, const Vec* b) {
    f32 dx = a->x - b->x;
    f32 dy = a->y - b->y;
    f32 dz = a->z - b->z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

f32 PSVECSquareDistance(const Vec* a, const Vec* b) {
    f32 dx = a->x - b->x;
    f32 dy = a->y - b->y;
    f32 dz = a->z - b->z;
    return dx*dx + dy*dy + dz*dz;
}

/* ================================================================ */
/* C_MTX* (C-callable matrix utilities)                             */
/* ================================================================ */

void C_MTXLookAt(Mtx m, const Vec* camPos, const Vec* camUp, const Vec* target) {
    Vec look, right, up;

    look.x = camPos->x - target->x;
    look.y = camPos->y - target->y;
    look.z = camPos->z - target->z;
    PSVECNormalize(&look, &look);

    PSVECCrossProduct(camUp, &look, &right);
    PSVECNormalize(&right, &right);

    PSVECCrossProduct(&look, &right, &up);

    m[0][0] = right.x; m[0][1] = right.y; m[0][2] = right.z;
    m[0][3] = -(camPos->x*right.x + camPos->y*right.y + camPos->z*right.z);
    m[1][0] = up.x;    m[1][1] = up.y;    m[1][2] = up.z;
    m[1][3] = -(camPos->x*up.x + camPos->y*up.y + camPos->z*up.z);
    m[2][0] = look.x;  m[2][1] = look.y;  m[2][2] = look.z;
    m[2][3] = -(camPos->x*look.x + camPos->y*look.y + camPos->z*look.z);
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f) {
    f32 cot = 1.0f / tanf(fovY * 0.5f * 3.14159265f / 180.0f);
    memset(m, 0, sizeof(Mtx44));
    m[0][0] = cot / aspect;
    m[1][1] = cot;
    m[2][2] = -(n + f) / (f - n);
    m[2][3] = -(2.0f * f * n) / (f - n);
    m[3][2] = -1.0f;
}

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    memset(m, 0, sizeof(Mtx44));
    m[0][0] = 2.0f / (r - l);
    m[0][3] = -(r + l) / (r - l);
    m[1][1] = 2.0f / (t - b);
    m[1][3] = -(t + b) / (t - b);
    m[2][2] = -1.0f / (f - n);
    m[2][3] = -n / (f - n);
    m[3][3] = 1.0f;
}

void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    memset(m, 0, sizeof(Mtx44));
    m[0][0] = (2.0f * n) / (r - l);
    m[0][2] = (r + l) / (r - l);
    m[1][1] = (2.0f * n) / (t - b);
    m[1][2] = (t + b) / (t - b);
    m[2][2] = -(n + f) / (f - n);
    m[2][3] = -(2.0f * f * n) / (f - n);
    m[3][2] = -1.0f;
}

void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT,
                           f32 transS, f32 transT) {
    f32 cot = 1.0f / tanf(fovY * 0.5f * 3.14159265f / 180.0f);
    memset(m, 0, sizeof(Mtx));
    m[0][0] = (cot / aspect) * scaleS;
    m[0][3] = transS;
    m[1][1] = cot * scaleT;
    m[1][3] = transT;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void C_MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r,
                     f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    memset(m, 0, sizeof(Mtx));
    m[0][0] = 2.0f / (r - l) * scaleS;
    m[0][3] = (-(r + l) / (r - l)) * scaleS + transS;
    m[1][1] = 2.0f / (t - b) * scaleT;
    m[1][3] = (-(t + b) / (t - b)) * scaleT + transT;
    m[2][2] = 0.0f;
    m[2][3] = 1.0f;
}

void C_MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n,
                       f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    memset(m, 0, sizeof(Mtx));
    m[0][0] = ((2.0f * n) / (r - l)) * scaleS;
    m[0][2] = ((r + l) / (r - l)) * scaleS - transS;
    m[1][1] = ((2.0f * n) / (t - b)) * scaleT;
    m[1][2] = ((t + b) / (t - b)) * scaleT - transT;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void C_MTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) {
    PSMTXRotAxisRad(m, axis, rad);
}

/* ================================================================ */
/* C_VEC* (C-callable vector utilities)                             */
/* ================================================================ */

void C_VECHalfAngle(const Vec* a, const Vec* b, Vec* half) {
    Vec an, bn;
    PSVECNormalize(a, &an);
    PSVECNormalize(b, &bn);
    half->x = an.x + bn.x;
    half->y = an.y + bn.y;
    half->z = an.z + bn.z;
    if (half->x == 0.0f && half->y == 0.0f && half->z == 0.0f) {
        half->x = 0.0f; half->y = 1.0f; half->z = 0.0f;
    } else {
        PSVECNormalize(half, half);
    }
}

void C_VECReflect(const Vec* src, const Vec* normal, Vec* dst) {
    f32 dot = 2.0f * PSVECDotProduct(src, normal);
    dst->x = src->x - dot * normal->x;
    dst->y = src->y - dot * normal->y;
    dst->z = src->z - dot * normal->z;
}

/* ================================================================ */
/* Quaternion functions                                              */
/* ================================================================ */

void QUATAdd(const Quaternion* p, const Quaternion* q, Quaternion* r) {
    r->x = p->x + q->x;
    r->y = p->y + q->y;
    r->z = p->z + q->z;
    r->w = p->w + q->w;
}

void QUATSubtract(const Quaternion* p, const Quaternion* q, Quaternion* r) {
    r->x = p->x - q->x;
    r->y = p->y - q->y;
    r->z = p->z - q->z;
    r->w = p->w - q->w;
}

void QUATMultiply(const Quaternion* p, const Quaternion* q, Quaternion* pq) {
    Quaternion tmp;
    tmp.w = p->w*q->w - p->x*q->x - p->y*q->y - p->z*q->z;
    tmp.x = p->w*q->x + p->x*q->w + p->y*q->z - p->z*q->y;
    tmp.y = p->w*q->y - p->x*q->z + p->y*q->w + p->z*q->x;
    tmp.z = p->w*q->z + p->x*q->y - p->y*q->x + p->z*q->w;
    *pq = tmp;
}

void QUATScale(const Quaternion* q, Quaternion* r, f32 scale) {
    r->x = q->x * scale;
    r->y = q->y * scale;
    r->z = q->z * scale;
    r->w = q->w * scale;
}

f32 QUATDotProduct(const Quaternion* p, const Quaternion* q) {
    return p->x*q->x + p->y*q->y + p->z*q->z + p->w*q->w;
}

void QUATNormalize(const Quaternion* src, Quaternion* unit) {
    f32 mag = sqrtf(QUATDotProduct(src, src));
    if (mag > 0.0f) {
        f32 inv = 1.0f / mag;
        unit->x = src->x * inv;
        unit->y = src->y * inv;
        unit->z = src->z * inv;
        unit->w = src->w * inv;
    } else {
        unit->x = unit->y = unit->z = 0.0f;
        unit->w = 1.0f;
    }
}

void QUATInverse(const Quaternion* src, Quaternion* inv) {
    f32 dot = QUATDotProduct(src, src);
    if (dot > 0.0f) {
        f32 invDot = 1.0f / dot;
        inv->x = -src->x * invDot;
        inv->y = -src->y * invDot;
        inv->z = -src->z * invDot;
        inv->w =  src->w * invDot;
    } else {
        inv->x = inv->y = inv->z = 0.0f;
        inv->w = 1.0f;
    }
}

void QUATDivide(const Quaternion* p, const Quaternion* q, Quaternion* r) {
    Quaternion qinv;
    QUATInverse(q, &qinv);
    QUATMultiply(p, &qinv, r);
}

void QUATLerp(const Quaternion* p, const Quaternion* q, Quaternion* r, f32 t) {
    r->x = p->x + t * (q->x - p->x);
    r->y = p->y + t * (q->y - p->y);
    r->z = p->z + t * (q->z - p->z);
    r->w = p->w + t * (q->w - p->w);
}

void QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* r, f32 t) {
    f32 dot = QUATDotProduct(p, q);
    Quaternion q2;
    if (dot < 0.0f) {
        dot = -dot;
        q2.x = -q->x; q2.y = -q->y; q2.z = -q->z; q2.w = -q->w;
    } else {
        q2 = *q;
    }

    if (dot > 0.9999f) {
        QUATLerp(p, &q2, r, t);
        QUATNormalize(r, r);
        return;
    }

    f32 theta = acosf(dot);
    f32 sinT = sinf(theta);
    f32 s0 = sinf((1.0f - t) * theta) / sinT;
    f32 s1 = sinf(t * theta) / sinT;

    r->x = s0 * p->x + s1 * q2.x;
    r->y = s0 * p->y + s1 * q2.y;
    r->z = s0 * p->z + s1 * q2.z;
    r->w = s0 * p->w + s1 * q2.w;
}

void QUATMtx(Quaternion* r, const Mtx m) {
    f32 trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f) {
        f32 s = sqrtf(trace + 1.0f);
        r->w = s * 0.5f;
        s = 0.5f / s;
        r->x = (m[2][1] - m[1][2]) * s;
        r->y = (m[0][2] - m[2][0]) * s;
        r->z = (m[1][0] - m[0][1]) * s;
    } else {
        int i = 0;
        if (m[1][1] > m[0][0]) i = 1;
        if (m[2][2] > m[i][i]) i = 2;
        int j = (i + 1) % 3;
        int k = (i + 2) % 3;
        f32 s = sqrtf(m[i][i] - m[j][j] - m[k][k] + 1.0f);
        f32 qt[4];
        qt[i] = s * 0.5f;
        s = 0.5f / s;
        qt[3] = (m[k][j] - m[j][k]) * s;
        qt[j] = (m[j][i] + m[i][j]) * s;
        qt[k] = (m[k][i] + m[i][k]) * s;
        r->x = qt[0]; r->y = qt[1]; r->z = qt[2]; r->w = qt[3];
    }
}

/* ================================================================ */
/* MTX44 functions                                                  */
/* ================================================================ */

void PSMTX44Identity(Mtx44 m) {
    memset(m, 0, sizeof(Mtx44));
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

void PSMTX44Copy(const Mtx44 src, Mtx44 dst) {
    memcpy(dst, src, sizeof(Mtx44));
}

void PSMTX44Trans(Mtx44 m, f32 xT, f32 yT, f32 zT) {
    PSMTX44Identity(m);
    m[0][3] = xT;
    m[1][3] = yT;
    m[2][3] = zT;
}

void PSMTX44Scale(Mtx44 m, f32 xS, f32 yS, f32 zS) {
    PSMTX44Identity(m);
    m[0][0] = xS;
    m[1][1] = yS;
    m[2][2] = zS;
}

void PSMTX44Concat(const Mtx44 a, const Mtx44 b, Mtx44 ab) {
    Mtx44 tmp;
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j] + a[i][3]*b[3][j];
        }
    }
    memcpy(ab, tmp, sizeof(Mtx44));
}

void PSMTX44Transpose(const Mtx44 src, Mtx44 xPose) {
    Mtx44 tmp;
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            tmp[i][j] = src[j][i];
    memcpy(xPose, tmp, sizeof(Mtx44));
}

u32 PSMTX44Inverse(const Mtx44 src, Mtx44 inv) {
    /* TODO: Full 4x4 inverse needed for projection transforms */
    memset(inv, 0, sizeof(Mtx44));
    inv[0][0] = inv[1][1] = inv[2][2] = inv[3][3] = 1.0f;
    (void)src;
    return 0;
}

/* ================================================================ */
/* Hardware / PowerPC stubs                                         */
/* ================================================================ */

void DCZeroRange(void* addr, u32 nBytes) {
    if (addr && nBytes > 0) memset(addr, 0, nBytes);
}

void LCDisable(void) {}
void LCEnable(void) {}
void LCLoadBlocks(void* destAddr, void* srcAddr, u32 numBlocks) {
    (void)destAddr; (void)srcAddr; (void)numBlocks;
}
void LCStoreBlocks(void* destAddr, void* srcAddr, u32 numBlocks) {
    (void)destAddr; (void)srcAddr; (void)numBlocks;
}

void PPCHalt(void) { abort(); }
u32 PPCMfmsr(void) { return 0; }
void PPCMtmsr(u32 val) { (void)val; }
void PPCSync(void) {}
void PPCMtwpar(u32 val) { (void)val; }

/* ================================================================ */
/* J3D Matrix functions                                             */
/* ================================================================ */

void J3DPSMtxArrayConcat(f32 (*dst)[4], f32 (*a)[4], f32 (*b)[4], u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        Mtx tmp;
        PSMTXConcat((const f32(*)[4])&a[i*3], (const f32(*)[4])&b[i*3], tmp);
        memcpy(&dst[i*3], tmp, sizeof(Mtx));
    }
}

void J3DGQRSetup7(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }

void J3DCalcBBoardMtx(f32 (*m)[4]) { (void)m; }
void J3DCalcYBBoardMtx(f32 (*m)[4]) { (void)m; }

void J3DPSCalcInverseTranspose(f32 (*src)[4], f32 (*dst)[3]) {
    /* Compute 3x3 inverse transpose of the upper-left 3x3 of src */
    f32 det = src[0][0]*(src[1][1]*src[2][2] - src[1][2]*src[2][1])
            - src[0][1]*(src[1][0]*src[2][2] - src[1][2]*src[2][0])
            + src[0][2]*(src[1][0]*src[2][1] - src[1][1]*src[2][0]);
    if (det == 0.0f) {
        memset(dst, 0, 9 * sizeof(f32));
        dst[0][0] = dst[1][1] = dst[2][2] = 1.0f;
        return;
    }
    f32 invDet = 1.0f / det;
    /* Inverse then transpose = cofactor / det */
    dst[0][0] = (src[1][1]*src[2][2] - src[1][2]*src[2][1]) * invDet;
    dst[1][0] = (src[0][2]*src[2][1] - src[0][1]*src[2][2]) * invDet;
    dst[2][0] = (src[0][1]*src[1][2] - src[0][2]*src[1][1]) * invDet;
    dst[0][1] = (src[1][2]*src[2][0] - src[1][0]*src[2][2]) * invDet;
    dst[1][1] = (src[0][0]*src[2][2] - src[0][2]*src[2][0]) * invDet;
    dst[2][1] = (src[0][2]*src[1][0] - src[0][0]*src[1][2]) * invDet;
    dst[0][2] = (src[1][0]*src[2][1] - src[1][1]*src[2][0]) * invDet;
    dst[1][2] = (src[0][1]*src[2][0] - src[0][0]*src[2][1]) * invDet;
    dst[2][2] = (src[0][0]*src[1][1] - src[0][1]*src[1][0]) * invDet;
}

/* ================================================================ */
/* MTXStack functions                                               */
/* ================================================================ */

typedef struct {
    u32 numMtx;
    Mtx* stackBase;
    Mtx* stackPtr;
} MTXStack;

void MTXInitStack(MTXStack* sPtr, Mtx* stackBase, u32 numMtx) {
    sPtr->numMtx = numMtx;
    sPtr->stackBase = stackBase;
    sPtr->stackPtr = stackBase;
}

void MTXPush(MTXStack* sPtr, const Mtx m) {
    PSMTXCopy(m, *sPtr->stackPtr);
    sPtr->stackPtr++;
}

void MTXPop(MTXStack* sPtr, Mtx m) {
    sPtr->stackPtr--;
    PSMTXCopy(*sPtr->stackPtr, m);
}

} /* extern "C" */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */
