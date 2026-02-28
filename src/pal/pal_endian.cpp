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

static void* s_last_repacked_files = NULL;

void* pal_swap_rarc_get_repacked_files(void) {
    return s_last_repacked_files;
}

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

    /* Validate RARC signature */
    if (hdr->signature != 0x52415243 /* 'RARC' */) {
        fprintf(stderr, "pal_swap_rarc: bad signature 0x%08X (expected RARC)\n", hdr->signature);
        return;
    }

    /* Validate header length */
    if (hdr->header_length > loadedSize || hdr->header_length < 0x20) {
        fprintf(stderr, "pal_swap_rarc: invalid header_length %u (loaded %u)\n",
                hdr->header_length, loadedSize);
        return;
    }

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

    /* Validate node and file counts against reasonable bounds */
    if (info->num_nodes > 4096) {
        fprintf(stderr, "pal_swap_rarc: invalid num_nodes %u, aborting\n", info->num_nodes);
        return;
    }
    if (info->num_file_entries > 65536) {
        fprintf(stderr, "pal_swap_rarc: invalid num_file_entries %u, aborting\n",
                info->num_file_entries);
        return;
    }

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
     * We keep the repacked array as a SEPARATE allocation to avoid
     * overwriting the string table that follows the file entries. */
    u8* filesBase = (u8*)&info->num_nodes + info->file_entry_offset;
    u32 numFiles = info->num_file_entries;
    u8* arcEnd = (u8*)arcData + loadedSize;

    /* Validate that the source file entry range is within the loaded buffer */
    u8* filesEnd = filesBase + numFiles * RARC_FILE_ENTRY_DISC_SIZE;
    if (filesBase < (u8*)arcData || filesEnd > arcEnd || numFiles == 0) {
        fprintf(stderr, "pal_swap_rarc: invalid file entries (base=%p end=%p arcEnd=%p numFiles=%u)\n",
                (void*)filesBase, (void*)filesEnd, (void*)arcEnd, numFiles);
        return;  /* corrupt or empty — skip file entry processing */
    }

    /* Track the last repacked array so JKRMemArchive can use it */
    s_last_repacked_files = NULL;

    if (sizeof(JKRArchive::SDIFileEntry) != RARC_FILE_ENTRY_DISC_SIZE && numFiles > 0) {
        /* Allocate persistent buffer for the repacked entries.
         * Do NOT copy back in-place — that overwrites the string table. */
        JKRArchive::SDIFileEntry* repacked = (JKRArchive::SDIFileEntry*)malloc(
            numFiles * sizeof(JKRArchive::SDIFileEntry));
        if (repacked) {
            for (u32 i = 0; i < numFiles; i++) {
                SDIFileEntryDisc* disc = (SDIFileEntryDisc*)(filesBase + i * RARC_FILE_ENTRY_DISC_SIZE);
                /* Bounds check each entry read */
                if ((u8*)(disc + 1) > arcEnd) break;
                repacked[i].file_id                   = pal_swap16(disc->file_id);
                repacked[i].name_hash                 = pal_swap16(disc->name_hash);
                repacked[i].type_flags_and_name_offset = pal_swap32(disc->type_flags_and_name_offset);
                repacked[i].data_offset               = pal_swap32(disc->data_offset);
                repacked[i].data_size                 = pal_swap32(disc->data_size);
                repacked[i].data                      = NULL;
            }
            s_last_repacked_files = repacked;
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

void pal_swap_bmg(void* data, u32 dataSize) {
    if (!data || dataSize < 0x20) return;

    u8* base = (u8*)data;
    /* Check magic: "MESGbmg1" */
    if (base[0] != 'M' || base[1] != 'E' || base[2] != 'S' || base[3] != 'G') return;

    /* bmg_header_t: magic[8] at 0x00, size(u32) at 0x08, n_sections(u32) at 0x0C, encoding(u8) at 0x10 */
    u32* pSize = (u32*)(base + 0x08);
    u32* pSections = (u32*)(base + 0x0C);

    /* Detect if already swapped: n_sections should be small (1-10) */
    u32 rawSections = *pSections;
    if (rawSections > 0 && rawSections < 100) return; /* Already native byte order */

    *pSize = pal_swap32(*pSize);
    *pSections = pal_swap32(*pSections);
    u32 nSections = *pSections;

    /* Walk sections starting after the 0x20-byte header */
    u32 offset = 0x20;
    for (u32 s = 0; s < nSections && offset + 8 <= dataSize; s++) {
        u32* secMagic = (u32*)(base + offset);
        u32* secSize  = (u32*)(base + offset + 4);

        u32 magic = pal_swap32(*secMagic);
        u32 size  = pal_swap32(*secSize);
        *secMagic = magic;
        *secSize  = size;

        /* INF1 section: swap entry metadata */
        if (magic == 0x494E4631 /* 'INF1' */) {
            if (offset + 0x10 <= dataSize) {
                u16* entryNum  = (u16*)(base + offset + 0x08);
                u16* entrySize = (u16*)(base + offset + 0x0A);
                *entryNum  = pal_swap16(*entryNum);
                *entrySize = pal_swap16(*entrySize);

                u16 nEntries = *entryNum;
                u16 eSize    = *entrySize;
                if (eSize == 0) eSize = 0x14; /* default entry size */

                /* Swap each entry's u32/u16 fields */
                u32 entryBase = offset + 0x10; /* entries start after INF1 header */
                for (u16 i = 0; i < nEntries && entryBase + eSize <= dataSize; i++) {
                    u8* e = base + entryBase;
                    /* string_offset (u32 at +0x00) */
                    *(u32*)(e + 0x00) = pal_swap32(*(u32*)(e + 0x00));
                    /* message_id (u16 at +0x04) */
                    *(u16*)(e + 0x04) = pal_swap16(*(u16*)(e + 0x04));
                    /* event_label_id (u16 at +0x06) */
                    *(u16*)(e + 0x06) = pal_swap16(*(u16*)(e + 0x06));
                    /* u8 fields at +0x08..+0x11 don't need swapping */
                    /* unk_0x12 (u16 at +0x12) */
                    if (eSize >= 0x14) {
                        *(u16*)(e + 0x12) = pal_swap16(*(u16*)(e + 0x12));
                    }
                    entryBase += eSize;
                }
            }
        }
        /* DAT1/STR1 sections: only header swap needed, string data is raw bytes */

        /* Advance to next section */
        if (size < 8) break; /* invalid section size */
        offset += size;
    }
}

} /* extern "C" */

#endif /* PLATFORM_PC */
