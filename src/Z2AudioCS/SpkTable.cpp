#include "Z2AudioCS/SpkTable.h"

#include <stdint.h>

SpkTable::SpkTable(void) {
    mIsInitialized = false;
    mNumOfSound = 0;
    mEntryOffset = 0;
    mDataOffsets = 0;
}

struct SpkTableHeader {
    s32 resourceCount;
    s32 entryOff;
    s32 dataOffsetsStartOff;
    BOOL isDataOffsetsInitialized;
};

void SpkTable::setResource(void* res) {
    mIsInitialized = false;

    s32* cursor = (s32*)res;
    uintptr_t base = (uintptr_t)res;

    s32 resourceCount = *cursor++;
    s32 entryOff = *cursor++;
    s32 dataOffsetsStartOff = *cursor++;
    s32* pIsDataOffsetsInitialized = cursor;
    BOOL isDataOffsetsInitialized = *cursor++;

    mNumOfSound = resourceCount;

    uintptr_t entryOffset = base + entryOff;
    mEntryOffset = entryOffset;
    s32* dataOffsets = (s32*)(base + dataOffsetsStartOff);
#if !PLATFORM_PC
    if (!isDataOffsetsInitialized) {
        for (s32 i = 0; i < mNumOfSound; i++) {
            dataOffsets[i] += (s32)base;
        }
    }
#endif

    s32* dataOffsetsCopy = dataOffsets;
    mDataOffsets = dataOffsetsCopy;
#if !PLATFORM_PC
    *pIsDataOffsetsInitialized = TRUE;
#else
    (void)pIsDataOffsetsInitialized;
    (void)isDataOffsetsInitialized;
#endif

    mIsInitialized = true;
}
