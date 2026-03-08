#ifndef PAL_GX_DIAG_CONTEXT_H
#define PAL_GX_DIAG_CONTEXT_H

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern s32 pal_diag_current_mat_index;
extern u32 pal_diag_current_material_mode;
extern const void* pal_diag_current_material_ptr;
extern const void* pal_diag_current_model_ptr;

#ifdef __cplusplus
}
#endif

#endif /* PAL_GX_DIAG_CONTEXT_H */
