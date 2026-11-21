/*
 * QEMU BIOS e820 routines
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "e820_memory_layout.h"

static size_t e820_entries;
struct e820_entry *e820_table;

int e820_add_entry(uint64_t address, uint64_t length, uint32_t type)
{
    if (type == E820_RAM) {
        if (address < 0x800000 && address + length > 0x800000) {
            // Split it to reserve 0x800000 - 0xA00000
            uint64_t end = address + length;
            e820_add_entry(address, 0x800000 - address, E820_RAM);
            e820_add_entry(0x800000, 0x200000, E820_RESERVED);
            if (end > 0xA00000) {
                e820_add_entry(0xA00000, end - 0xA00000, E820_RAM);
            }
            return e820_get_num_entries();
        }
    }

    /* new "etc/e820" file -- include ram and reserved entries */
    e820_table = g_renew(struct e820_entry, e820_table, e820_entries + 1);
    e820_table[e820_entries].address = cpu_to_le64(address);
    e820_table[e820_entries].length = cpu_to_le64(length);
    e820_table[e820_entries].type = cpu_to_le32(type);
    e820_entries++;

    return e820_entries;
}

int e820_get_num_entries(void)
{
    return e820_entries;
}

bool e820_get_entry(int idx, uint32_t type, uint64_t *address, uint64_t *length)
{
    if (idx < e820_entries && e820_table[idx].type == cpu_to_le32(type)) {
        *address = le64_to_cpu(e820_table[idx].address);
        *length = le64_to_cpu(e820_table[idx].length);
        return true;
    }
    return false;
}
