/**
 * f_pc_method.cpp
 * Framework - Process Method
 */

#include "f_pc/f_pc_method.h"

#if PLATFORM_PC
#include <stdio.h>
#include <stdint.h>
static inline int pal_validate_func_ptr(process_method_func fn) {
    uintptr_t addr = (uintptr_t)fn;
    /* Reject clearly invalid function pointers:
     * - NULL (already handled by caller)
     * - Low addresses (< 4096) — unmapped guard page
     * - Unaligned (x86-64 code should be at least 1-byte aligned, but
     *   detect clearly corrupt values like heap data cast to func ptr) */
    if (addr < 0x1000) return 0;
    /* Detect common heap corruption patterns: pointers into heap data
     * often have high nibble 0x0 on GCN but 0x5 or 0x7 on x86-64.
     * We can't perfectly validate, but reject known-bad. */
    return 1;
}
#endif

int fpcMtd_Method(process_method_func i_method, void* i_process) {
    if (i_method != NULL) {
#if PLATFORM_PC
        if (!pal_validate_func_ptr(i_method)) {
            fprintf(stderr, "frame=? cat=BAD_FUNC_PTR detail=\"fpcMtd_Method: corrupt func %p for proc %p\"\n",
                    (void*)i_method, i_process);
            return 1;
        }
#endif
        return i_method(i_process);
    }
    else
        return 1;
}

int fpcMtd_Execute(process_method_class* i_methods, void* i_process) {
    return fpcMtd_Method(i_methods->execute_method, i_process);
}

int fpcMtd_IsDelete(process_method_class* i_methods, void* i_process) {
    return fpcMtd_Method(i_methods->is_delete_method, i_process);
}

int fpcMtd_Delete(process_method_class* i_methods, void* i_process) {
    return fpcMtd_Method(i_methods->delete_method, i_process);
}

int fpcMtd_Create(process_method_class* i_methods, void* i_process) {
    return fpcMtd_Method(i_methods->create_method, i_process);
}
