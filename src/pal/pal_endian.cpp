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
    if (loadedSize >= hdr->header_length + sizeof(SArcDataInfo)) {
        SArcDataInfo* info = (SArcDataInfo*)((u8*)hdr + hdr->header_length);
        info->num_nodes           = pal_swap32(info->num_nodes);
        info->node_offset         = pal_swap32(info->node_offset);
        info->num_file_entries    = pal_swap32(info->num_file_entries);
        info->file_entry_offset   = pal_swap32(info->file_entry_offset);
        info->string_table_length = pal_swap32(info->string_table_length);
        info->string_table_offset = pal_swap32(info->string_table_offset);
        info->next_free_file_id   = pal_swap16(info->next_free_file_id);

        /* Swap SDIDirEntry nodes */
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

        /* Swap SDIFileEntry entries */
        u8* filesBase = (u8*)&info->num_nodes + info->file_entry_offset;
        for (u32 i = 0; i < info->num_file_entries; i++) {
            JKRArchive::SDIFileEntry* entry = (JKRArchive::SDIFileEntry*)(filesBase + i * 20); /* sizeof on-disc entry = 20 bytes (no data pointer) */
            if ((u8*)entry + 20 > (u8*)arcData + loadedSize) break;
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
    /* Yaz0 header: 4-byte magic "Yaz0", 4-byte decompressed size (big-endian), 8 bytes reserved */
    u32* words = (u32*)data;
    /* Don't swap magic — it's a string. Only swap the decompressed size. */
    words[1] = pal_swap32(words[1]);
}

} /* extern "C" */

#endif /* PLATFORM_PC */
