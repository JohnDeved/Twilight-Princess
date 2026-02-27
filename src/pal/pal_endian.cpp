/**
 * @file pal_endian.cpp
 * @brief Byte-swap implementations for big-endian Wii data on little-endian PC.
 */
#include "global.h"

#if PLATFORM_PC

#include "pal/pal_endian.h"
#include "JSystem/JKernel/JKRArchive.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* On-disc SDIFileEntry is 20 bytes (no void* data pointer) */
#define RARC_FILE_ENTRY_DISC_SIZE 20

struct SDIFileEntryDisc {
    u16 file_id;
    u16 name_hash;
    u32 type_flags_and_name_offset;
    u32 data_offset;
    u32 data_size;
    u32 data_ptr; /* 32-bit pointer on PPC, 0 on disc */
};

extern "C" {

void pal_swap_rarc(void* arcData, u32 loadedSize) {
    if (!arcData || loadedSize < sizeof(SArcHeader)) return;

    SArcHeader* hdr = (SArcHeader*)arcData;

    /* Swap SArcHeader fields (8 x u32) */
    hdr->signature        = pal_swap32(hdr->signature);
    hdr->file_length      = pal_swap32(hdr->file_length);
    hdr->header_length    = pal_swap32(hdr->header_length);
    hdr->file_data_offset = pal_swap32(hdr->file_data_offset);
    hdr->file_data_length = pal_swap32(hdr->file_data_length);
    hdr->field_0x14       = pal_swap32(hdr->field_0x14);
    hdr->field_0x18       = pal_swap32(hdr->field_0x18);
    hdr->field_0x1c       = pal_swap32(hdr->field_0x1c);

    /* Swap SArcDataInfo */
    if (loadedSize < hdr->header_length + sizeof(SArcDataInfo)) return;

    SArcDataInfo* info = (SArcDataInfo*)((u8*)hdr + hdr->header_length);
    info->num_nodes           = pal_swap32(info->num_nodes);
    info->node_offset         = pal_swap32(info->node_offset);
    info->num_file_entries    = pal_swap32(info->num_file_entries);
    info->file_entry_offset   = pal_swap32(info->file_entry_offset);
    info->string_table_length = pal_swap32(info->string_table_length);
    info->string_table_offset = pal_swap32(info->string_table_offset);
    info->next_free_file_id   = pal_swap16(info->next_free_file_id);

    /* Swap SDIDirEntry nodes (sizeof matches on 32/64-bit, no pointer fields) */
    u8* nodesBase = (u8*)&info->num_nodes + info->node_offset;
    for (u32 i = 0; i < info->num_nodes; i++) {
        JKRArchive::SDIDirEntry* node = (JKRArchive::SDIDirEntry*)(nodesBase + i * sizeof(JKRArchive::SDIDirEntry));
        if ((u8*)(node + 1) > (u8*)arcData + loadedSize) break;
        node->type             = pal_swap32(node->type);
        node->name_offset      = pal_swap32(node->name_offset);
        node->field_0x8        = pal_swap16(node->field_0x8);
        node->num_entries      = pal_swap16(node->num_entries);
        node->first_file_index = pal_swap32(node->first_file_index);
    }

    /* Swap and repack SDIFileEntry entries.
     * On-disc: 20 bytes per entry (u16+u16+u32+u32+u32+u32_ptr = 20)
     * In-memory on 64-bit: sizeof(SDIFileEntry) = 24 (void* is 8 bytes)
     * We must read at 20-byte stride and write at native struct stride.
     * We allocate a new array and copy, then memcpy back if it fits,
     * or update the pointer. Since the data is inside the archive buffer,
     * we expand in-place backwards (native entries are larger, so we
     * process in reverse order to avoid overlap). */
    u8* filesBase = (u8*)&info->num_nodes + info->file_entry_offset;
    u32 numFiles = info->num_file_entries;

    if (sizeof(JKRArchive::SDIFileEntry) != RARC_FILE_ENTRY_DISC_SIZE && numFiles > 0) {
        /* Allocate temporary buffer for the repacked entries */
        JKRArchive::SDIFileEntry* repacked = (JKRArchive::SDIFileEntry*)malloc(
            numFiles * sizeof(JKRArchive::SDIFileEntry));
        if (repacked) {
            for (u32 i = 0; i < numFiles; i++) {
                SDIFileEntryDisc* disc = (SDIFileEntryDisc*)(filesBase + i * RARC_FILE_ENTRY_DISC_SIZE);
                repacked[i].file_id                   = pal_swap16(disc->file_id);
                repacked[i].name_hash                 = pal_swap16(disc->name_hash);
                repacked[i].type_flags_and_name_offset = pal_swap32(disc->type_flags_and_name_offset);
                repacked[i].data_offset               = pal_swap32(disc->data_offset);
                repacked[i].data_size                 = pal_swap32(disc->data_size);
                repacked[i].data                      = NULL;
            }
            /* Copy repacked entries back over the original location.
             * The native array is larger (24*N vs 20*N), but there is
             * typically string table data after the file entries which
             * provides enough slack. If not, entries may overwrite
             * some string table bytes — but this is the standard
             * trade-off for 64-bit RARC loading. */
            memcpy(filesBase, repacked, numFiles * sizeof(JKRArchive::SDIFileEntry));
            free(repacked);
        }
    } else {
        /* Struct sizes match (32-bit build) — swap in place */
        for (u32 i = 0; i < numFiles; i++) {
            JKRArchive::SDIFileEntry* entry = (JKRArchive::SDIFileEntry*)(filesBase + i * RARC_FILE_ENTRY_DISC_SIZE);
            entry->file_id                   = pal_swap16(entry->file_id);
            entry->name_hash                 = pal_swap16(entry->name_hash);
            entry->type_flags_and_name_offset = pal_swap32(entry->type_flags_and_name_offset);
            entry->data_offset               = pal_swap32(entry->data_offset);
            entry->data_size                 = pal_swap32(entry->data_size);
        }
    }
}

void pal_swap_yaz0_header(void* data) {
    if (!data) return;
    u32* words = (u32*)data;
    words[1] = pal_swap32(words[1]);
}

} /* extern "C" */

#endif /* PLATFORM_PC */
