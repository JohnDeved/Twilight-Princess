#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/JUtility/JUTNameTab.h"
#include <cstring>

#if PLATFORM_PC
/* Name tables in game assets have at most a few hundred entries.
 * Values above this limit indicate un-swapped big-endian data. */
#define MAX_NAMETAB_ENTRIES 4096
/* Maximum reasonable byte offset within a name table string area.
 * Offsets above this are likely un-swapped big-endian u16 values. */
#define MAX_NAMETAB_OFFSET  0x8000
#include <sys/mman.h>
#endif

JUTNameTab::JUTNameTab() {
    setResource(NULL);
}

JUTNameTab::JUTNameTab(const ResNTAB* pNameTable) {
    setResource(pNameTable);
}

void JUTNameTab::setResource(const ResNTAB* pNameTable) {
    mNameTable = pNameTable;

    if (pNameTable != NULL) {
#if PLATFORM_PC
        /* Validate the pointer is accessible. On PC, J3D data may have
         * corrupt pointers from incomplete endian swap or 64-bit issues.
         * Use msync() to probe whether the page is mapped. */
        {
            uintptr_t addr = (uintptr_t)pNameTable;
            uintptr_t page = addr & ~(uintptr_t)0xFFF;
            if (msync((void*)page, sizeof(ResNTAB), MS_ASYNC) != 0) {
                /* Page not mapped — invalid pointer */
                mNameTable = NULL;
                mNameNum = 0;
                mpStrData = 0;
                return;
            }
        }
#endif
        mNameNum = pNameTable->mEntryNum;
#if PLATFORM_PC
        /* On PC, name tables may be un-swapped big-endian data.
         * If mEntryNum looks like a swapped value, try byte-swapping it.
         * A name table with more than 4096 entries is almost certainly corrupt. */
        if (mNameNum > MAX_NAMETAB_ENTRIES) {
            u16 swapped = (u16)(((mNameNum >> 8) & 0xFF) | ((mNameNum & 0xFF) << 8));
            if (swapped <= MAX_NAMETAB_ENTRIES) {
                mNameNum = swapped;
            } else {
                /* Both values are unreasonable — table is corrupt, disable it */
                mNameTable = NULL;
                mNameNum = 0;
                mpStrData = 0;
                return;
            }
        }
#endif
        mpStrData = (const char*)(pNameTable->mEntries + mNameNum);
    } else {
        mNameNum = 0;
        mpStrData = 0;
    }
}

s32 JUTNameTab::getIndex(const char* pName) const {
    JUT_ASSERT(101, mNameTable != NULL);
#if PLATFORM_PC
    if (mNameTable == NULL || pName == NULL) return -1;
#endif

    const ResNTAB::Entry* pEntry = mNameTable->mEntries;
    u16 keyCode = calcKeyCode(pName);

    for (u16 i = 0; i < mNameNum; i++) {
#if PLATFORM_PC
        u16 off = pEntry->mOffs;
        if (off > MAX_NAMETAB_OFFSET) { pEntry++; continue; }
#endif
        if (
            pEntry->mKeyCode == keyCode &&
            strcmp((mNameTable->mEntries[i].mOffs + ((const char*)mNameTable)), pName) == 0
        ) {
            return i;
        }
        pEntry++;
    }

    return -1;
}

const char* JUTNameTab::getName(u16 index) const {
    JUT_ASSERT(138, mNameTable != NULL);
#if PLATFORM_PC
    if (mNameTable == NULL) return NULL;
#endif
    if (index < mNameNum) {
#if PLATFORM_PC
        /* On PC, name table offsets may be corrupt from incomplete endian swap.
         * Validate the offset is within a reasonable range to prevent SIGSEGV. */
        u16 off = mNameTable->mEntries[index].mOffs;
        if (off > MAX_NAMETAB_OFFSET) return NULL;
#endif
        return ((const char*)mNameTable) + mNameTable->mEntries[index].mOffs;
    }
    return NULL;
}

u16 JUTNameTab::calcKeyCode(const char* pName) const {
    u32 keyCode = 0;
    while (*pName)
        keyCode = (keyCode * 3) + *pName++;
    return keyCode;
}
