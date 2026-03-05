/**
 * c_node_iter.cpp
 *
 */

#include "SSystem/SComponent/c_node_iter.h"
#include "SSystem/SComponent/c_node.h"
#include <dolphin/types.h>
#include "global.h"
#if PLATFORM_PC || PLATFORM_NX_HB
#include <stdio.h>
#endif

int cNdIt_Method(node_class* node, cNdIt_MethodFunc method, void* data) {
    int ret = 1;
    node_class* pNext = NODE_GET_NEXT(node);

#if PLATFORM_PC || PLATFORM_NX_HB
    /* Hard iteration cap: guard against circular linked-list corruption that
     * can occur after a SIGSEGV-recovered actor Execute crash.  The actor
     * framework uses singly-linked lists (cLsNode / cNdNode); if a crash
     * occurs mid-modification the next-pointer can form a cycle, causing
     * cNdIt_Method to spin forever.  10000 iterations is well above any
     * real actor-list length (max observed: ~500) so this never fires on
     * clean game state. */
    int nd_iter = 0;
    while (node) {
        if (++nd_iter > 10000) {
            fprintf(stderr, "{\"cNdIt_cycle\":{\"iter\":%d,\"node\":\"%p\"}}\n",
                    nd_iter, (void *)node);
            break;
        }
        if (!method(node, data))
            ret = 0;
        node = pNext;
        pNext = NODE_GET_NEXT(pNext);
    }
#else
    while (node) {
        if (!method(node, data))
            ret = 0;
        node = pNext;
        pNext = NODE_GET_NEXT(pNext);
    }
#endif

    return ret;
}

void* cNdIt_Judge(node_class* node, cNdIt_JudgeFunc judge, void* data) {
    node_class* pNext = NODE_GET_NEXT(node);

#if PLATFORM_PC || PLATFORM_NX_HB
    int nd_iter = 0;
    while (node) {
        if (++nd_iter > 10000) {
            fprintf(stderr, "{\"cNdIt_judge_cycle\":{\"iter\":%d,\"node\":\"%p\"}}\n",
                    nd_iter, (void *)node);
            break;
        }
        void* ret = judge(node, data);
        if (ret != NULL)
            return ret;
        node = pNext;
        pNext = NODE_GET_NEXT(pNext);
    }
#else
    while (node) {
        void* ret = judge(node, data);
        if (ret != NULL)
            return ret;
        node = pNext;
        pNext = NODE_GET_NEXT(pNext);
    }
#endif

    return NULL;
}
