#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/JUtility/JUTNameTab.h"
#include <string>

JUTNameTab::JUTNameTab() {
    setResource(NULL);
}

JUTNameTab::JUTNameTab(const ResNTAB* pNameTable) {
    setResource(pNameTable);
}

void JUTNameTab::setResource(const ResNTAB* pNameTable) {
    mNameTable = pNameTable;

    if (pNameTable != NULL) {
        mNameNum = pNameTable->mEntryNum;
#if PLATFORM_PC
        /* On PC, name tables may be un-swapped big-endian data.
         * If mEntryNum looks like a swapped value, try byte-swapping it.
         * A name table with more than 4096 entries is almost certainly corrupt. */
        if (mNameNum > 4096) {
            u16 swapped = (u16)(((mNameNum >> 8) & 0xFF) | ((mNameNum & 0xFF) << 8));
            if (swapped <= 4096) {
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
        if (off > 0x8000) { pEntry++; continue; }
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
        if (off > 0x8000) return NULL;  /* offset too large → likely un-swapped big-endian */
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
