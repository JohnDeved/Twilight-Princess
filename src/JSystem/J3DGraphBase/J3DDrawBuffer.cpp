/**
 * J3DDrawBuffer.cpp
 *
 */

#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/JKernel/JKRHeap.h"
#if PLATFORM_PC
#include <stdio.h>
#endif

void J3DDrawBuffer::calcZRatio() {
    mZRatio = (mZFar - mZNear) / (f32)mEntryTableSize;
}

void J3DDrawBuffer::initialize() {
    mDrawMode = J3DDrawBufDrawMode_Head;
    mSortMode = J3DDrawBufSortMode_Mat;
    mZNear = 1.0f;
    mZFar = 10000.0f;
    mpZMtx = NULL;
    mpCallBackPacket = NULL;
    mEntryTableSize = 0x20;
    calcZRatio();
}

int J3DDrawBuffer::allocBuffer(u32 size) {
    mpBuffer = new (0x20) J3DPacket*[size];
    if (mpBuffer == NULL)
        return kJ3DError_Alloc;

    mEntryTableSize = size;

    frameInit();
    calcZRatio();
    return kJ3DError_Success;
}

J3DDrawBuffer::~J3DDrawBuffer() {
    frameInit();

    delete[] mpBuffer;
    mpBuffer = NULL;
}

void J3DDrawBuffer::frameInit() {
    u32 bufSize = mEntryTableSize;
    for (u32 i = 0; i < bufSize; i++)
        mpBuffer[i] = NULL;

    mpCallBackPacket = NULL;
}

int J3DDrawBuffer::entryMatSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(122, pMatPacket != NULL);

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (pMatPacket->isChanged()) {
        pMatPacket->setNextPacket(mpBuffer[0]);
        mpBuffer[0] = pMatPacket;
        return 1;
    }

    J3DTexture* pTexture = j3dSys.getTexture();
    u16 texNo = pMatPacket->getMaterial()->getTexNo(0);
    J3D_ASSERT_NULLPTR(150, pTexture != NULL);

    u32 hash;
    if (texNo == 0xFFFF) {
        hash = 0;
    } else {
        hash = ((uintptr_t)pTexture->getResTIMG(texNo) + pTexture->getResTIMG(texNo)->imageOffset) >> 5;
    }
    u32 slot = hash & (mEntryTableSize - 1);

    if (mpBuffer[slot] == NULL) {
        mpBuffer[slot] = pMatPacket;
        return 1;
    }

    J3DMatPacket* packet;
    for (packet = (J3DMatPacket*)mpBuffer[slot]; packet != NULL; packet = (J3DMatPacket*)packet->getNextPacket())
    {
        if (packet->isSame(pMatPacket)) {
            packet->addShapePacket(pMatPacket->getShapePacket());
            return 0;
        }
    }

    pMatPacket->setNextPacket(mpBuffer[slot]);
    mpBuffer[slot] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryMatAnmSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(199, pMatPacket != NULL);

    J3DMaterialAnm* pMaterialAnm = pMatPacket->mpMaterialAnm;
    u32 slot = (uintptr_t)pMaterialAnm & (mEntryTableSize - 1);

    if (pMaterialAnm == NULL) {
        return entryMatSort(pMatPacket);
    }

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (mpBuffer[slot] == NULL) {
        mpBuffer[slot] = pMatPacket;
        return 1;
    }

    J3DMatPacket* packet;
    for (packet = (J3DMatPacket*)mpBuffer[slot]; packet != NULL; packet = (J3DMatPacket*)packet->getNextPacket())
    {
        if (packet->mpMaterialAnm == pMaterialAnm) {
            packet->addShapePacket(pMatPacket->getShapePacket());
            return 0;
        }
    }

    pMatPacket->setNextPacket(mpBuffer[slot]);
    mpBuffer[slot] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryZSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(257, pMatPacket != NULL);

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    Vec tmp;
    tmp.x = mpZMtx[0][3];
    tmp.y = mpZMtx[1][3];
    tmp.z = mpZMtx[2][3];

    f32 value = -J3DCalcZValue(j3dSys.getViewMtx(), tmp);

    u32 index;
    if (mZNear + mZRatio < value) {
        if (mZFar - mZRatio > value) {
            index = value / mZRatio;
        } else {
            index = mEntryTableSize - 1;
        }
    } else {
        index = 0;
    }

    index = (mEntryTableSize - 1) - index;
    pMatPacket->setNextPacket(mpBuffer[index]);
    mpBuffer[index] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryModelSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(316, pMatPacket != NULL);

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (mpCallBackPacket != NULL) {
        mpCallBackPacket->addChildPacket(pMatPacket);
        return 1;
    }

    return 0;
}

int J3DDrawBuffer::entryInvalidSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(343, pMatPacket != NULL);

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (mpCallBackPacket != NULL) {
        mpCallBackPacket->addChildPacket(pMatPacket->getShapePacket());
        return 1;
    }

    return 0;
}

int J3DDrawBuffer::entryNonSort(J3DMatPacket* pMatPacket) {
    J3D_ASSERT_NULLPTR(370, pMatPacket != NULL);

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    pMatPacket->setNextPacket(mpBuffer[0]);
    mpBuffer[0] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryImm(J3DPacket* pPacket, u16 index) {
    J3D_ASSERT_NULLPTR(394, pPacket != NULL);
    J3D_ASSERT_RANGE(395, index < mEntryTableSize);

#if PLATFORM_PC
    {
        static int s_entry_probe_count = 0;
        if (s_entry_probe_count < 80) {
            const void* vptr = *(const void* const*)pPacket;
            fprintf(stderr,
                    "{\"j3d_entry_probe\":{\"idx\":%u,\"probe\":%d,"
                    "\"packet\":\"%p\",\"vptr\":\"%p\",\"prev_head\":\"%p\"}}\n",
                    (unsigned)index, s_entry_probe_count,
                    (void*)pPacket, vptr, (void*)mpBuffer[index]);
            s_entry_probe_count++;
        }
    }
#endif
    pPacket->setNextPacket(mpBuffer[index]);
    mpBuffer[index] = pPacket;
    return 1;
}

J3DDrawBuffer::sortFunc J3DDrawBuffer::sortFuncTable[6] = {
    &J3DDrawBuffer::entryMatSort,   &J3DDrawBuffer::entryMatAnmSort,  &J3DDrawBuffer::entryZSort,
    &J3DDrawBuffer::entryModelSort, &J3DDrawBuffer::entryInvalidSort, &J3DDrawBuffer::entryNonSort,
};

J3DDrawBuffer::drawFunc J3DDrawBuffer::drawFuncTable[2] = {
    &J3DDrawBuffer::drawHead,
    &J3DDrawBuffer::drawTail,
};

int J3DDrawBuffer::entryNum;

void J3DDrawBuffer::draw() const {
#if PLATFORM_PC
    if (mDrawMode >= 2 || mpBuffer == NULL) return;
#endif
    J3D_ASSERT_RANGE(411, mDrawMode < J3DDrawBufDrawMode_MAX);

    drawFunc func = drawFuncTable[mDrawMode];
    (this->*func)();
}

void J3DDrawBuffer::drawHead() const {
    u32 size = mEntryTableSize;
    J3DPacket** buf = mpBuffer;
#if PLATFORM_PC
    /* Cap TOTAL probe output to avoid stderr flood in CI; 80 samples across
     * the run is enough to cover >2 frames of a 38-entry packet chain and
     * capture the repeated corruption pattern without log spam. */
    enum { MAX_PACKET_PROBES = 80 };
    /* entryTableSize should never exceed 0x10000 (default is 0x20); a larger
     * value indicates corruption.  buf==NULL means allocBuffer was never called. */
    if (buf == NULL || size == 0 || size > 0x10000) return;
#endif

    for (u32 i = 0; i < size; i++) {
#if PLATFORM_PC
        if (buf[i] != NULL) {
            /* Validate chain shape before virtual dispatch to isolate list/link
             * corruption without relying on signal recovery. */
            const J3DPacket* cursor = buf[i];
            int chain_len = 0;
            int invalid_ptr = 0;
            int self_loop = 0;
            int vptr_low = 0;
            while (cursor != NULL && chain_len < 512) {
                if ((uintptr_t)cursor < 0x1000) {
                    invalid_ptr = 1;
                    break;
                }
                const void* vptr = *(const void* const*)cursor;
                if ((uintptr_t)vptr < 0x100000)
                    vptr_low = 1;
                const J3DPacket* next = cursor->getNextPacket();
                if (next == cursor) {
                    self_loop = 1;
                    break;
                }
                cursor = next;
                chain_len++;
            }
            if (chain_len >= 512)
                self_loop = 1;
            if (invalid_ptr || self_loop || vptr_low) {
                fprintf(stderr,
                        "{\"j3d_chain_invalid\":{\"slot\":%u,\"len\":%d,"
                        "\"invalid_ptr\":%d,\"self_loop\":%d,\"vptr_low\":%d,"
                        "\"head\":\"%p\"}}\n",
                        i, chain_len, invalid_ptr, self_loop, vptr_low, (void*)buf[i]);
                continue;
            }
        }
#endif
        for (J3DPacket* packet = buf[i]; packet != NULL; packet = packet->getNextPacket()) {
#if PLATFORM_PC
            /* Addresses below 4KB fall within the OS NULL-page guard region
             * and indicate a corrupted linked-list pointer. */
            if ((uintptr_t)packet < 0x1000) break;
            /* Packet-chain probe: sample packet pointer, vptr, next pointer and
             * first bytes before virtual dispatch to diagnose vtable corruption. */
            {
                static int s_packet_probe_count = 0;
                if (s_packet_probe_count < MAX_PACKET_PROBES) {
                    const void* vptr = *(const void* const*)packet;
                    const J3DPacket* next = packet->getNextPacket();
                    uintptr_t packet_header = *(const uintptr_t*)packet;
                    fprintf(stderr,
                            "{\"j3d_packet_probe\":{\"slot\":%u,\"probe\":%d,"
                            "\"packet\":\"%p\",\"vptr\":\"%p\",\"next\":\"%p\","
                            "\"vptr_low\":%d,\"packet_header\":\"0x%llx\"}}\n",
                            i, s_packet_probe_count, (void*)packet, vptr, (void*)next,
                            ((uintptr_t)vptr < 0x100000) ? 1 : 0,
                            (unsigned long long)packet_header);
                    s_packet_probe_count++;
                }
            }
#endif
            packet->draw();
        }
    }
}

void J3DDrawBuffer::drawTail() const {
#if PLATFORM_PC
    if (mpBuffer == NULL || mEntryTableSize == 0 || mEntryTableSize > 0x10000) return;
#endif
    for (int i = mEntryTableSize - 1; i >= 0; i--) {
        for (J3DPacket* packet = mpBuffer[i]; packet != NULL; packet = packet->getNextPacket()) {
#if PLATFORM_PC
            if ((uintptr_t)packet < 0x1000) break;
#endif
            packet->draw();
        }
    }
}
