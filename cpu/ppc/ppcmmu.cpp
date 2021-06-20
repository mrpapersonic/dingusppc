/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-20 divingkatae and maximum
                      (theweirdo)     spatium

(Contact divingkatae#1017 or powermax#2286 on Discord for more info)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/** @file PowerPC Memory management unit emulation. */

/* TODO:
    - implement TLB
    - implement 601-style BATs
    - add proper error and exception handling
 */

#include "ppcmmu.h"
#include "devices/memctrlbase.h"
#include "memaccess.h"
#include "ppcemu.h"
#include <array>
#include <cinttypes>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thirdparty/loguru/loguru.hpp>

/* pointer to exception handler to be called when a MMU exception is occured. */
void (*mmu_exception_handler)(Except_Type exception_type, uint32_t srr1_bits);

/** PowerPC-style MMU BAT arrays (NULL initialization isn't prescribed). */
PPC_BAT_entry ibat_array[4] = {{0}};
PPC_BAT_entry dbat_array[4] = {{0}};

//#define MMU_PROFILING // enable MMU profiling

/* MMU profiling */
#ifdef MMU_PROFILING

/* global variables for lightweight MMU profiling */
uint64_t    dmem_reads_total   = 0; // counts reads from data memory
uint64_t    iomem_reads_total  = 0; // counts I/O memory reads
uint64_t    dmem_writes_total  = 0; // counts writes to data memory
uint64_t    iomem_writes_total = 0; // counts I/O memory writes
uint64_t    exec_reads_total   = 0; // counts reads from executable memory
uint64_t    bat_transl_total   = 0; // counts BAT translations
uint64_t    ptab_transl_total  = 0; // counts page table translations
uint64_t    unaligned_reads    = 0; // counts unaligned reads
uint64_t    unaligned_writes   = 0; // counts unaligned writes
uint64_t    unaligned_crossp_r = 0; // counts unaligned crosspage reads
uint64_t    unaligned_crossp_w = 0; // counts unaligned crosspage writes

#include "utils/profiler.h"
#include <memory>

class MMUProfile : public BaseProfile {
public:
    MMUProfile() : BaseProfile("PPC_MMU") {};

    void populate_variables(std::vector<ProfileVar>& vars) {
        vars.clear();

        vars.push_back({.name = "Data Memory Reads Total",
                        .format = ProfileVarFmt::DEC,
                        .value = dmem_reads_total});

        vars.push_back({.name = "I/O Memory Reads Total",
                        .format = ProfileVarFmt::DEC,
                        .value = iomem_reads_total});

        vars.push_back({.name = "Data Memory Writes Total",
                        .format = ProfileVarFmt::DEC,
                        .value = dmem_writes_total});

        vars.push_back({.name = "I/O Memory Writes Total",
                        .format = ProfileVarFmt::DEC,
                        .value = iomem_writes_total});

        vars.push_back({.name = "Reads from Executable Memory",
                        .format = ProfileVarFmt::DEC,
                        .value = exec_reads_total});

        vars.push_back({.name = "BAT Translations Total",
                        .format = ProfileVarFmt::DEC,
                        .value = bat_transl_total});

        vars.push_back({.name = "Page Table Translations Total",
                        .format = ProfileVarFmt::DEC,
                        .value = ptab_transl_total});

        vars.push_back({.name = "Unaligned Reads Total",
                        .format = ProfileVarFmt::DEC,
                        .value = unaligned_reads});

        vars.push_back({.name = "Unaligned Writes Total",
                        .format = ProfileVarFmt::DEC,
                        .value = unaligned_writes});

        vars.push_back({.name = "Unaligned Crosspage Reads Total",
                        .format = ProfileVarFmt::DEC,
                        .value = unaligned_crossp_r});

        vars.push_back({.name = "Unaligned Crosspage Writes Total",
                        .format = ProfileVarFmt::DEC,
                        .value = unaligned_crossp_w});
    };

    void reset() {
        dmem_reads_total   = 0;
        iomem_reads_total  = 0;
        dmem_writes_total  = 0;
        iomem_writes_total = 0;
        exec_reads_total   = 0;
        bat_transl_total   = 0;
        ptab_transl_total  = 0;
        unaligned_reads    = 0;
        unaligned_writes   = 0;
        unaligned_crossp_r = 0;
        unaligned_crossp_w = 0;
    };
};
#endif

/** Temporary TLB test variables. */
bool        MemAccessType; // true - memory, false - I/O
uint64_t    MemAddr = 0;
MMIODevice  *Device = 0;
uint32_t    DevOffset = 0;


/** remember recently used physical memory regions for quicker translation. */
AddressMapEntry last_read_area  = {0xFFFFFFFF, 0xFFFFFFFF};
AddressMapEntry last_write_area = {0xFFFFFFFF, 0xFFFFFFFF};
AddressMapEntry last_exec_area  = {0xFFFFFFFF, 0xFFFFFFFF};
AddressMapEntry last_ptab_area  = {0xFFFFFFFF, 0xFFFFFFFF};
AddressMapEntry last_dma_area   = {0xFFFFFFFF, 0xFFFFFFFF};


template <class T, const bool is_aligned>
static inline T read_phys_mem(AddressMapEntry *mru_rgn, uint32_t addr)
{
    if (addr < mru_rgn->start || (addr + sizeof(T)) > mru_rgn->end) {
        AddressMapEntry* entry = mem_ctrl_instance->find_range(addr);
        if (entry) {
            *mru_rgn = *entry;
        } else {
            LOG_F(ERROR, "Read from unmapped memory at 0x%08X!\n", addr);
            return (-1ULL ? sizeof(T) == 8 : -1UL);
        }
    }

    if (mru_rgn->type & (RT_ROM | RT_RAM)) {
#ifdef MMU_PROFILING
        dmem_reads_total++;
#endif

        if (!MemAccessType) {
            LOG_F(ERROR, "TLB real memory access expected!");
        }

        if ((mru_rgn->mem_ptr + (addr - mru_rgn->start)) != (uint8_t *)MemAddr) {
            LOG_F(ERROR, "TLB address mismatch! Expected: 0x%llx, got: 0x%llx",
                  (uint64_t)(mru_rgn->mem_ptr + (addr - mru_rgn->start)),
                  (uint64_t)MemAddr);
        }

        switch(sizeof(T)) {
        case 1:
            return *(mru_rgn->mem_ptr + (addr - mru_rgn->start));
        case 2:
            if (is_aligned) {
                return READ_WORD_BE_A(mru_rgn->mem_ptr + (addr - mru_rgn->start));
            } else {
                return READ_WORD_BE_U(mru_rgn->mem_ptr + (addr - mru_rgn->start));
            }
        case 4:
            if (is_aligned) {
                return READ_DWORD_BE_A(mru_rgn->mem_ptr + (addr - mru_rgn->start));
            } else {
                return READ_DWORD_BE_U(mru_rgn->mem_ptr + (addr - mru_rgn->start));
            }
        case 8:
            if (is_aligned) {
                return READ_QWORD_BE_A(mru_rgn->mem_ptr + (addr - mru_rgn->start));
            }
        default:
            LOG_F(ERROR, "READ_PHYS: invalid size %lu passed\n", sizeof(T));
            return (-1ULL ? sizeof(T) == 8 : -1UL);
        }
    } else if (mru_rgn->type & RT_MMIO) {
#ifdef MMU_PROFILING
        iomem_reads_total++;
#endif
        if (MemAccessType) {
            LOG_F(ERROR, "TLB I/O memory access expected!");
        }

        if (mru_rgn->devobj != Device || (addr - mru_rgn->start) != DevOffset) {
            LOG_F(ERROR, "TLB MMIO access mismatch! Expected: 0x%X, got: 0x%X",
                  addr - mru_rgn->start, DevOffset);
        }

        return (mru_rgn->devobj->read(mru_rgn->start,
                addr - mru_rgn->start, sizeof(T)));
    } else {
        LOG_F(ERROR, "READ_PHYS: invalid region type!\n");
        return (-1ULL ? sizeof(T) == 8 : -1UL);
    }
}

template <class T, const bool is_aligned>
static inline void write_phys_mem(AddressMapEntry *mru_rgn, uint32_t addr, T value)
{
    if (addr < mru_rgn->start || (addr + sizeof(T)) > mru_rgn->end) {
        AddressMapEntry* entry = mem_ctrl_instance->find_range(addr);
        if (entry) {
            *mru_rgn = *entry;
        } else {
            LOG_F(ERROR, "Write to unmapped memory at 0x%08X!\n", addr);
            return;
        }
    }

    if (mru_rgn->type & RT_RAM) {
#ifdef MMU_PROFILING
        dmem_writes_total++;
#endif
        switch(sizeof(T)) {
        case 1:
            *(mru_rgn->mem_ptr + (addr - mru_rgn->start)) = value;
            break;
        case 2:
            if (is_aligned) {
                WRITE_WORD_BE_A(mru_rgn->mem_ptr + (addr - mru_rgn->start), value);
            } else {
                WRITE_WORD_BE_U(mru_rgn->mem_ptr + (addr - mru_rgn->start), value);
            }
            break;
        case 4:
            if (is_aligned) {
                WRITE_DWORD_BE_A(mru_rgn->mem_ptr + (addr - mru_rgn->start), value);
            } else {
                WRITE_DWORD_BE_U(mru_rgn->mem_ptr + (addr - mru_rgn->start), value);
            }
            break;
        case 8:
            if (is_aligned) {
                WRITE_QWORD_BE_A(mru_rgn->mem_ptr + (addr - mru_rgn->start), value);
            }
            break;
        default:
            LOG_F(ERROR, "WRITE_PHYS: invalid size %lu passed\n", sizeof(T));
            return;
        }
    } else if (mru_rgn->type & RT_MMIO) {
#ifdef MMU_PROFILING
        iomem_writes_total++;
#endif
        mru_rgn->devobj->write(mru_rgn->start, addr - mru_rgn->start, value,
                               sizeof(T));
    } else {
        LOG_F(ERROR, "WRITE_PHYS: invalid region type!\n");
    }
}

uint8_t* mmu_get_dma_mem(uint32_t addr, uint32_t size) {
    if (addr >= last_dma_area.start && (addr + size) <= last_dma_area.end) {
        return last_dma_area.mem_ptr + (addr - last_dma_area.start);
    } else {
        AddressMapEntry* entry = mem_ctrl_instance->find_range(addr);
        if (entry && entry->type & (RT_ROM | RT_RAM)) {
            last_dma_area.start   = entry->start;
            last_dma_area.end     = entry->end;
            last_dma_area.mem_ptr = entry->mem_ptr;
            return last_dma_area.mem_ptr + (addr - last_dma_area.start);
        } else {
            LOG_F(ERROR, "SOS: DMA access to unmapped memory %08X!\n", addr);
            exit(-1);    // FIXME: ugly error handling, must be the proper exception!
        }
    }
}

void ppc_set_cur_instruction(const uint8_t* ptr) {
    ppc_cur_instruction = READ_DWORD_BE_A(ptr);
}

void ibat_update(uint32_t bat_reg) {
    int upper_reg_num;
    uint32_t bl, hi_mask;
    PPC_BAT_entry* bat_entry;

    upper_reg_num = bat_reg & 0xFFFFFFFE;

    if (ppc_state.spr[upper_reg_num] & 3) {    // is that BAT pair valid?
        bat_entry = &ibat_array[(bat_reg - 528) >> 1];
        bl        = (ppc_state.spr[upper_reg_num] >> 2) & 0x7FF;
        hi_mask   = ~((bl << 17) | 0x1FFFF);

        bat_entry->access  = ppc_state.spr[upper_reg_num] & 3;
        bat_entry->prot    = ppc_state.spr[upper_reg_num + 1] & 3;
        bat_entry->hi_mask = hi_mask;
        bat_entry->phys_hi = ppc_state.spr[upper_reg_num + 1] & hi_mask;
        bat_entry->bepi    = ppc_state.spr[upper_reg_num] & hi_mask;
    }
}

void dbat_update(uint32_t bat_reg) {
    int upper_reg_num;
    uint32_t bl, hi_mask;
    PPC_BAT_entry* bat_entry;

    upper_reg_num = bat_reg & 0xFFFFFFFE;

    if (ppc_state.spr[upper_reg_num] & 3) {    // is that BAT pair valid?
        bat_entry = &dbat_array[(bat_reg - 536) >> 1];
        bl        = (ppc_state.spr[upper_reg_num] >> 2) & 0x7FF;
        hi_mask   = ~((bl << 17) | 0x1FFFF);

        bat_entry->access  = ppc_state.spr[upper_reg_num] & 3;
        bat_entry->prot    = ppc_state.spr[upper_reg_num + 1] & 3;
        bat_entry->hi_mask = hi_mask;
        bat_entry->phys_hi = ppc_state.spr[upper_reg_num + 1] & hi_mask;
        bat_entry->bepi    = ppc_state.spr[upper_reg_num] & hi_mask;
    }
}

/** PowerPC-style block address translation. */
template <const BATType type>
static BATResult ppc_block_address_translation(uint32_t la)
{
    uint32_t pa;    // translated physical address
    uint8_t  prot;  // protection bits for the translated address
    PPC_BAT_entry *bat_array;

    bool bat_hit    = false;
    unsigned msr_pr = !!(ppc_state.msr & 0x4000);

    bat_array = (type == BATType::Instruction) ? ibat_array : dbat_array;

    // Format: %XY
    // X - supervisor access bit, Y - problem/user access bit
    // Those bits are mutually exclusive
    unsigned access_bits = ((msr_pr ^ 1) << 1) | msr_pr;

    for (int bat_index = 0; bat_index < 4; bat_index++) {
        PPC_BAT_entry* bat_entry = &bat_array[bat_index];

        if ((bat_entry->access & access_bits) && ((la & bat_entry->hi_mask) == bat_entry->bepi)) {
            bat_hit = true;

#ifdef MMU_PROFILING
            bat_transl_total++;
#endif
            // logical to physical translation
            pa = bat_entry->phys_hi | (la & ~bat_entry->hi_mask);
            prot = bat_entry->prot;
            break;
        }
    }

    return BATResult{bat_hit, prot, pa};
}

static inline uint8_t* calc_pteg_addr(uint32_t hash) {
    uint32_t sdr1_val, pteg_addr;

    sdr1_val = ppc_state.spr[SPR::SDR1];

    pteg_addr = sdr1_val & 0xFE000000;
    pteg_addr |= (sdr1_val & 0x01FF0000) | (((sdr1_val & 0x1FF) << 16) & ((hash & 0x7FC00) << 6));
    pteg_addr |= (hash & 0x3FF) << 6;

    if (pteg_addr >= last_ptab_area.start && pteg_addr <= last_ptab_area.end) {
        return last_ptab_area.mem_ptr + (pteg_addr - last_ptab_area.start);
    } else {
        AddressMapEntry* entry = mem_ctrl_instance->find_range(pteg_addr);
        if (entry && entry->type & (RT_ROM | RT_RAM)) {
            last_ptab_area.start   = entry->start;
            last_ptab_area.end     = entry->end;
            last_ptab_area.mem_ptr = entry->mem_ptr;
            return last_ptab_area.mem_ptr + (pteg_addr - last_ptab_area.start);
        } else {
            LOG_F(ERROR, "SOS: no page table region was found at %08X!\n", pteg_addr);
            exit(-1);    // FIXME: ugly error handling, must be the proper exception!
        }
    }
}

static bool search_pteg(
    uint8_t* pteg_addr, uint8_t** ret_pte_addr, uint32_t vsid, uint16_t page_index, uint8_t pteg_num) {
    /* construct PTE matching word */
    uint32_t pte_check = 0x80000000 | (vsid << 7) | (pteg_num << 6) | (page_index >> 10);

#ifdef MMU_INTEGRITY_CHECKS
    /* PTEG integrity check that ensures that all matching PTEs have
       identical RPN, WIMG and PP bits (PPC PEM 32-bit 7.6.2, rule 5). */
    uint32_t pte_word2_check;
    bool match_found = false;

    for (int i = 0; i < 8; i++, pteg_addr += 8) {
        if (pte_check == READ_DWORD_BE_A(pteg_addr)) {
            if (match_found) {
                if ((READ_DWORD_BE_A(pteg_addr) & 0xFFFFF07B) != pte_word2_check) {
                    LOG_F(ERROR, "Multiple PTEs with different RPN/WIMG/PP found!\n");
                    exit(-1);
                }
            } else {
                /* isolate RPN, WIMG and PP fields */
                pte_word2_check = READ_DWORD_BE_A(pteg_addr) & 0xFFFFF07B;
                *ret_pte_addr   = pteg_addr;
            }
        }
    }
#else
    for (int i = 0; i < 8; i++, pteg_addr += 8) {
        if (pte_check == READ_DWORD_BE_A(pteg_addr)) {
            *ret_pte_addr = pteg_addr;
            return true;
        }
    }
#endif

    return false;
}

static uint32_t page_address_translate(uint32_t la, bool is_instr_fetch, unsigned msr_pr, int is_write) {
    uint32_t sr_val, page_index, pteg_hash1, vsid, pte_word2;
    unsigned key, pp;
    uint8_t* pte_addr;

    sr_val = ppc_state.sr[(la >> 28) & 0x0F];
    if (sr_val & 0x80000000) {
        LOG_F(ERROR, "Direct-store segments not supported, LA=%0xX\n", la);
        exit(-1);    // FIXME: ugly error handling, must be the proper exception!
    }

    /* instruction fetch from a no-execute segment will cause ISI exception */
    if ((sr_val & 0x10000000) && is_instr_fetch) {
        mmu_exception_handler(Except_Type::EXC_ISI, 0x10000000);
    }

    page_index = (la >> 12) & 0xFFFF;
    pteg_hash1 = (sr_val & 0x7FFFF) ^ page_index;
    vsid       = sr_val & 0x0FFFFFF;

    if (!search_pteg(calc_pteg_addr(pteg_hash1), &pte_addr, vsid, page_index, 0)) {
        if (!search_pteg(calc_pteg_addr(~pteg_hash1), &pte_addr, vsid, page_index, 1)) {
            if (is_instr_fetch) {
                mmu_exception_handler(Except_Type::EXC_ISI, 0x40000000);
            } else {
                ppc_state.spr[SPR::DSISR] = 0x40000000 | (is_write << 25);
                ppc_state.spr[SPR::DAR]   = la;
                mmu_exception_handler(Except_Type::EXC_DSI, 0);
            }
        }
    }

    pte_word2 = READ_DWORD_BE_A(pte_addr + 4);

    key = (((sr_val >> 29) & 1) & msr_pr) | (((sr_val >> 30) & 1) & (msr_pr ^ 1));

    /* check page access */
    pp = pte_word2 & 3;

    // the following scenarios cause DSI/ISI exception:
    // any access with key = 1 and PP = %00
    // write access with key = 1 and PP = %01
    // write access with PP = %11
    if ((key && (!pp || (pp == 1 && is_write))) || (pp == 3 && is_write)) {
        if (is_instr_fetch) {
            mmu_exception_handler(Except_Type::EXC_ISI, 0x08000000);
        } else {
            ppc_state.spr[SPR::DSISR] = 0x08000000 | (is_write << 25);
            ppc_state.spr[SPR::DAR]   = la;
            mmu_exception_handler(Except_Type::EXC_DSI, 0);
        }
    }

    /* update R and C bits */
    /* For simplicity, R is set on each access, C is set only for writes */
    pte_addr[6] |= 0x01;
    if (is_write) {
        pte_addr[7] |= 0x80;
    }

    /* return physical address */
    return ((pte_word2 & 0xFFFFF000) | (la & 0x00000FFF));
}

/** PowerPC-style MMU instruction address translation. */
static uint32_t ppc_mmu_instr_translate(uint32_t la) {
    uint32_t pa; /* translated physical address */

    bool bat_hit    = false;
    unsigned msr_pr = !!(ppc_state.msr & 0x4000);

    // Format: %XY
    // X - supervisor access bit, Y - problem/user access bit
    // Those bits are mutually exclusive
    unsigned access_bits = ((msr_pr ^ 1) << 1) | msr_pr;

    for (int bat_index = 0; bat_index < 4; bat_index++) {
        PPC_BAT_entry* bat_entry = &ibat_array[bat_index];

        if ((bat_entry->access & access_bits) && ((la & bat_entry->hi_mask) == bat_entry->bepi)) {
            bat_hit = true;

#ifdef MMU_PROFILING
            bat_transl_total++;
#endif

            if (!bat_entry->prot) {
                mmu_exception_handler(Except_Type::EXC_ISI, 0x08000000);
            }

            // logical to physical translation
            pa = bat_entry->phys_hi | (la & ~bat_entry->hi_mask);
            break;
        }
    }

    /* page address translation */
    if (!bat_hit) {
        pa = page_address_translate(la, true, msr_pr, 0);

#ifdef MMU_PROFILING
        ptab_transl_total++;
#endif
    }

    return pa;
}

/** PowerPC-style MMU data address translation. */
static uint32_t ppc_mmu_addr_translate(uint32_t la, int is_write) {
    uint32_t pa; /* translated physical address */

    bool bat_hit    = false;
    unsigned msr_pr = !!(ppc_state.msr & 0x4000);

    // Format: %XY
    // X - supervisor access bit, Y - problem/user access bit
    // Those bits are mutually exclusive
    unsigned access_bits = ((msr_pr ^ 1) << 1) | msr_pr;

    for (int bat_index = 0; bat_index < 4; bat_index++) {
        PPC_BAT_entry* bat_entry = &dbat_array[bat_index];

        if ((bat_entry->access & access_bits) && ((la & bat_entry->hi_mask) == bat_entry->bepi)) {
            bat_hit = true;

#ifdef MMU_PROFILING
            bat_transl_total++;
#endif

            if (!bat_entry->prot || ((bat_entry->prot & 1) && is_write)) {
                ppc_state.spr[SPR::DSISR] = 0x08000000 | (is_write << 25);
                ppc_state.spr[SPR::DAR]   = la;
                mmu_exception_handler(Except_Type::EXC_DSI, 0);
            }

            // logical to physical translation
            pa = bat_entry->phys_hi | (la & ~bat_entry->hi_mask);
            break;
        }
    }

    /* page address translation */
    if (!bat_hit) {
        pa = page_address_translate(la, false, msr_pr, is_write);

#ifdef MMU_PROFILING
        ptab_transl_total++;
#endif
    }

    return pa;
}

static void mem_write_unaligned(uint32_t addr, uint32_t value, uint32_t size) {
#ifdef MMU_DEBUG
    LOG_F(WARNING, "Attempt to write unaligned %d bytes to 0x%08X\n", size, addr);
#endif

    if (((addr & 0xFFF) + size) > 0x1000) {
        // Special case: unaligned cross-page writes
#ifdef MMU_PROFILING
        unaligned_crossp_w++;
#endif

        uint32_t phys_addr;
        uint32_t shift = (size - 1) * 8;

        // Break misaligned memory accesses into multiple, bytewise accesses
        // and retranslate on page boundary.
        // Because such accesses suffer a performance penalty, they will be
        // presumably very rare so don't care much about performance.
        for (int i = 0; i < size; shift -= 8, addr++, phys_addr++, i++) {
            if ((ppc_state.msr & 0x10) && (!i || !(addr & 0xFFF))) {
                phys_addr = ppc_mmu_addr_translate(addr, 1);
            }

            write_phys_mem<uint8_t, false>(&last_write_area, phys_addr,
                                          (value >> shift) & 0xFF);
        }
    } else {
        // data address translation if enabled
        if (ppc_state.msr & 0x10) {
            addr = ppc_mmu_addr_translate(addr, 1);
        }

        if (size == 2) {
            write_phys_mem<uint16_t, false>(&last_write_area, addr, value);
        } else {
            write_phys_mem<uint32_t, false>(&last_write_area, addr, value);
        }

#ifdef MMU_PROFILING
        unaligned_writes++;
#endif
    }
}

void mem_write_byte(uint32_t addr, uint8_t value) {
    /* data address translation if enabled */
    if (ppc_state.msr & 0x10) {
        addr = ppc_mmu_addr_translate(addr, 1);
    }

    write_phys_mem<uint8_t, true>(&last_write_area, addr, value);
}

void mem_write_word(uint32_t addr, uint16_t value) {
    if (addr & 1) {
        mem_write_unaligned(addr, value, 2);
        return;
    }

    /* data address translation if enabled */
    if (ppc_state.msr & 0x10) {
        addr = ppc_mmu_addr_translate(addr, 1);
    }

    write_phys_mem<uint16_t, true>(&last_write_area, addr, value);
}

void mem_write_dword(uint32_t addr, uint32_t value) {
    if (addr & 3) {
        mem_write_unaligned(addr, value, 4);
        return;
    }

    /* data address translation if enabled */
    if (ppc_state.msr & 0x10) {
        addr = ppc_mmu_addr_translate(addr, 1);
    }

    write_phys_mem<uint32_t, true>(&last_write_area, addr, value);
}

void mem_write_qword(uint32_t addr, uint64_t value) {
    if (addr & 7) {
        LOG_F(ERROR, "SOS! Attempt to write unaligned QWORD to 0x%08X\n", addr);
        exit(-1);    // FIXME!
    }

    /* data address translation if enabled */
    if (ppc_state.msr & 0x10) {
        addr = ppc_mmu_addr_translate(addr, 1);
    }

    write_phys_mem<uint64_t, true>(&last_write_area, addr, value);
}


#define PAGE_SIZE_BITS      12
#define TLB_SIZE            4096
#define TLB2_WAYS           4
#define TLB_INVALID_TAG     0xFFFFFFFF

typedef struct TLBEntry {
    uint32_t    tag;
    uint16_t    flags;
    uint16_t    lru_bits;
    union {
        int64_t             host_va_offset;
        AddressMapEntry*    reg_desc;
    };
} TLBEntry;

// primary TLB for all MMU modes
static std::array<TLBEntry, TLB_SIZE> mode1_tlb1;
static std::array<TLBEntry, TLB_SIZE> mode2_tlb1;
static std::array<TLBEntry, TLB_SIZE> mode3_tlb1;

// secondary TLB for all MMU modes
static std::array<TLBEntry, TLB_SIZE*TLB2_WAYS> mode1_tlb2;
static std::array<TLBEntry, TLB_SIZE*TLB2_WAYS> mode2_tlb2;
static std::array<TLBEntry, TLB_SIZE*TLB2_WAYS> mode3_tlb2;

TLBEntry *pCurTLB1; // current primary TLB
TLBEntry *pCurTLB2; // current secondary TLB

uint32_t tlb_size_mask = TLB_SIZE - 1;

// fake TLB entry for handling of unmapped memory accesses
uint64_t    UnmappedVal = -1ULL;
TLBEntry    UnmappedMem = {TLB_INVALID_TAG, 0, 0, 0};

uint8_t     MMUMode = {0xFF};

void mmu_change_mode()
{
    uint8_t mmu_mode = ((ppc_state.msr >> 3) & 0x2) | ((ppc_state.msr >> 14) & 1);

    if (MMUMode != mmu_mode) {
        switch(mmu_mode) {
        case 0: // real address mode
            pCurTLB1 = &mode1_tlb1[0];
            pCurTLB2 = &mode1_tlb2[0];
            break;
        case 2: // supervisor mode with data translation enabled
            pCurTLB1 = &mode2_tlb1[0];
            pCurTLB2 = &mode2_tlb2[0];
            break;
        case 3: // user mode with data translation enabled
            pCurTLB1 = &mode3_tlb1[0];
            pCurTLB2 = &mode3_tlb2[0];
            break;
        }
        MMUMode = mmu_mode;
    }
}

static TLBEntry* tlb2_target_entry(uint32_t gp_va)
{
    TLBEntry *tlb_entry;

    tlb_entry = &pCurTLB2[((gp_va >> PAGE_SIZE_BITS) & tlb_size_mask) * TLB2_WAYS];

    // select the target from invalid blocks first
    if (tlb_entry[0].tag == TLB_INVALID_TAG) {
        // update LRU bits
        tlb_entry[0].lru_bits  = 0x3;
        tlb_entry[1].lru_bits  = 0x2;
        tlb_entry[2].lru_bits &= 0x1;
        tlb_entry[3].lru_bits &= 0x1;
        return tlb_entry;
    } else if (tlb_entry[1].tag == TLB_INVALID_TAG) {
        // update LRU bits
        tlb_entry[0].lru_bits  = 0x2;
        tlb_entry[1].lru_bits  = 0x3;
        tlb_entry[2].lru_bits &= 0x1;
        tlb_entry[3].lru_bits &= 0x1;
        return &tlb_entry[1];
    } else if (tlb_entry[2].tag == TLB_INVALID_TAG) {
        // update LRU bits
        tlb_entry[0].lru_bits &= 0x1;
        tlb_entry[1].lru_bits &= 0x1;
        tlb_entry[2].lru_bits  = 0x3;
        tlb_entry[3].lru_bits  = 0x2;
        return &tlb_entry[2];
    } else if (tlb_entry[3].tag == TLB_INVALID_TAG) {
        // update LRU bits
        tlb_entry[0].lru_bits &= 0x1;
        tlb_entry[1].lru_bits &= 0x1;
        tlb_entry[2].lru_bits  = 0x2;
        tlb_entry[3].lru_bits  = 0x3;
        return &tlb_entry[3];
    } else { // no invalid blocks, replace an existing one according with the hLRU policy
        if (tlb_entry[0].lru_bits == 0) {
            // update LRU bits
            tlb_entry[0].lru_bits  = 0x3;
            tlb_entry[1].lru_bits  = 0x2;
            tlb_entry[2].lru_bits &= 0x1;
            tlb_entry[3].lru_bits &= 0x1;
            return tlb_entry;
        } else if (tlb_entry[1].lru_bits == 0) {
            // update LRU bits
            tlb_entry[0].lru_bits  = 0x2;
            tlb_entry[1].lru_bits  = 0x3;
            tlb_entry[2].lru_bits &= 0x1;
            tlb_entry[3].lru_bits &= 0x1;
            return &tlb_entry[1];
        } else if (tlb_entry[2].lru_bits == 0) {
            // update LRU bits
            tlb_entry[0].lru_bits &= 0x1;
            tlb_entry[1].lru_bits &= 0x1;
            tlb_entry[2].lru_bits  = 0x3;
            tlb_entry[3].lru_bits  = 0x2;
            return &tlb_entry[2];
        } else {
            // update LRU bits
            tlb_entry[0].lru_bits &= 0x1;
            tlb_entry[1].lru_bits &= 0x1;
            tlb_entry[2].lru_bits  = 0x2;
            tlb_entry[3].lru_bits  = 0x3;
            return &tlb_entry[3];
        }
    }
}

static TLBEntry* tlb2_refill(uint32_t guest_va, int is_write)
{
    uint32_t phys_addr;
    TLBEntry *tlb_entry;

    const uint32_t tag = guest_va & ~0xFFFUL;

    /* data address translation if enabled */
    if (ppc_state.msr & 0x10) {
        // attempt block address translation first
        BATResult bat_res = ppc_block_address_translation<BATType::Data>(guest_va);
        if (bat_res.hit) {
            // check block protection
            if (!bat_res.prot || ((bat_res.prot & 1) && is_write)) {
                ppc_state.spr[SPR::DSISR] = 0x08000000 | (is_write << 25);
                ppc_state.spr[SPR::DAR]   = guest_va;
                mmu_exception_handler(Except_Type::EXC_DSI, 0);
            }
            phys_addr = bat_res.phys;
        } else {
            // page address translation
            phys_addr = page_address_translate(guest_va, false,
                                    !!(ppc_state.msr & 0x4000), is_write);
        }
    } else {
        phys_addr = guest_va;
    }

    // look up host virtual address
    AddressMapEntry* reg_desc = mem_ctrl_instance->find_range(phys_addr);
    if (reg_desc) {
        // refill the secondary TLB
        tlb_entry = tlb2_target_entry(tag);
        tlb_entry->tag = tag;
        if (reg_desc->type & RT_MMIO) {
            tlb_entry->flags = 2; // MMIO region
            tlb_entry->reg_desc = reg_desc;
        } else {
            tlb_entry->flags = 1; // memory region backed by host memory
            tlb_entry->host_va_offset = (int64_t)reg_desc->mem_ptr - guest_va +
                                        (phys_addr - reg_desc->start);
        }
        return tlb_entry;
    } else {
        LOG_F(ERROR, "Read from unmapped memory at 0x%08X!\n", phys_addr);
        UnmappedMem.tag = tag;
        UnmappedMem.host_va_offset = (int64_t)(&UnmappedVal) - guest_va;
        return &UnmappedMem;
    }
}

void flush_tlb_entry(uint32_t ea)
{
    TLBEntry *tlb_entry, *tlb1, *tlb2;

    const uint32_t tag = ea & ~0xFFFUL;

    for (int m = 0; m < 3; m++) {
        switch (m) {
        case 0:
            tlb1 = &mode1_tlb1[0];
            tlb2 = &mode1_tlb2[0];
            break;
        case 1:
            tlb1 = &mode1_tlb1[0];
            tlb2 = &mode1_tlb2[0];
            break;
        case 2:
            tlb1 = &mode1_tlb1[0];
            tlb2 = &mode1_tlb2[0];
            break;
        }

        // flush primary TLB
        tlb_entry = &tlb1[(ea >> PAGE_SIZE_BITS) & tlb_size_mask];
        if (tlb_entry->tag == tag) {
            tlb_entry->tag = TLB_INVALID_TAG;
            //LOG_F(INFO, "Invalidated primary TLB entry at 0x%X", ea);
        }

        // flush secondary TLB
        tlb_entry = &tlb2[((ea >> PAGE_SIZE_BITS) & tlb_size_mask) * TLB2_WAYS];
        for (int i = 0; i < TLB2_WAYS; i++) {
            if (tlb_entry[i].tag == tag) {
                tlb_entry[i].tag = TLB_INVALID_TAG;
                //LOG_F(INFO, "Invalidated secondary TLB entry at 0x%X", ea);
            }
        }
    }
}

static inline uint64_t tlb_translate_addr(uint32_t guest_va)
{
    TLBEntry *tlb1_entry, *tlb2_entry;

    const uint32_t tag = guest_va & ~0xFFFUL;

    // look up address in the primary TLB
    tlb1_entry = &pCurTLB1[(guest_va >> PAGE_SIZE_BITS) & tlb_size_mask];
    if (tlb1_entry->tag == tag) { // primary TLB hit -> fast path
        MemAccessType = true;
        MemAddr = tlb1_entry->host_va_offset + guest_va;
        return tlb1_entry->host_va_offset + guest_va;
    } else { // primary TLB miss -> look up address in the secondary TLB
        tlb2_entry = &pCurTLB2[((guest_va >> PAGE_SIZE_BITS) & tlb_size_mask) * TLB2_WAYS];
        if (tlb2_entry->tag == tag) {
            // update LRU bits
            tlb2_entry[0].lru_bits  = 0x3;
            tlb2_entry[1].lru_bits  = 0x2;
            tlb2_entry[2].lru_bits &= 0x1;
            tlb2_entry[3].lru_bits &= 0x1;
        } else if (tlb2_entry[1].tag == tag) {
            tlb2_entry = &tlb2_entry[1];
            // update LRU bits
            tlb2_entry[0].lru_bits  = 0x2;
            tlb2_entry[1].lru_bits  = 0x3;
            tlb2_entry[2].lru_bits &= 0x1;
            tlb2_entry[3].lru_bits &= 0x1;
        } else if (tlb2_entry[2].tag == tag) {
            tlb2_entry = &tlb2_entry[2];
            // update LRU bits
            tlb2_entry[0].lru_bits &= 0x1;
            tlb2_entry[1].lru_bits &= 0x1;
            tlb2_entry[2].lru_bits  = 0x3;
            tlb2_entry[3].lru_bits  = 0x2;
        } else if (tlb2_entry[3].tag == tag) {
            tlb2_entry = &tlb2_entry[3];
            // update LRU bits
            tlb2_entry[0].lru_bits &= 0x1;
            tlb2_entry[1].lru_bits &= 0x1;
            tlb2_entry[2].lru_bits  = 0x2;
            tlb2_entry[3].lru_bits  = 0x3;
        } else { // secondary TLB miss ->
            // perform full address translation and refill the secondary TLB
            tlb2_entry = tlb2_refill(guest_va, 0);
        }

        if (tlb2_entry->flags & 1) { // is it a real memory region?
            // refill the primary TLB
            tlb1_entry->tag = tag;
            tlb1_entry->flags = 1;
            tlb1_entry->host_va_offset = tlb2_entry->host_va_offset;
            MemAccessType = true;
            MemAddr = tlb1_entry->host_va_offset + guest_va;
            return tlb1_entry->host_va_offset + guest_va;
        } else { // an attempt to access a memory-mapped device
            MemAccessType = false;
            Device = tlb2_entry->reg_desc->devobj;
            DevOffset = guest_va - tlb2_entry->reg_desc->start;
            return guest_va - tlb2_entry->reg_desc->start;
        }
    }
}

static uint32_t mem_grab_unaligned(uint32_t addr, uint32_t size) {
    uint32_t ret = 0;

#ifdef MMU_DEBUG
    LOG_F(WARNING, "Attempt to read unaligned %d bytes from 0x%08X\n", size, addr);
#endif

    if (((addr & 0xFFF) + size) > 0x1000) {
        // Special case: misaligned cross-page reads
#ifdef MMU_PROFILING
        unaligned_crossp_r++;
#endif

        uint32_t phys_addr;
        uint32_t res = 0;

        // Break misaligned memory accesses into multiple, bytewise accesses
        // and retranslate on page boundary.
        // Because such accesses suffer a performance penalty, they will be
        // presumably very rare so don't care much about performance.
        for (int i = 0; i < size; addr++, phys_addr++, i++) {
            tlb_translate_addr(addr);
            if ((ppc_state.msr & 0x10) && (!i || !(addr & 0xFFF))) {
                phys_addr = ppc_mmu_addr_translate(addr, 0);
            }

            res = (res << 8) |
                read_phys_mem<uint8_t, false>(&last_read_area, phys_addr);
        }
        return res;

    } else {
        /* data address translation if enabled */
        if (ppc_state.msr & 0x10) {
            addr = ppc_mmu_addr_translate(addr, 0);
        }

        if (size == 2) {
            return read_phys_mem<uint16_t, false>(&last_read_area, addr);
        } else {
            return read_phys_mem<uint32_t, false>(&last_read_area, addr);
        }

#ifdef MMU_PROFILING
        unaligned_reads++;
#endif
    }

    return ret;
}

static inline TLBEntry * lookup_secondary_tlb(uint32_t guest_va, uint32_t tag) {
    TLBEntry *tlb_entry;

    tlb_entry = &pCurTLB2[((guest_va >> PAGE_SIZE_BITS) & tlb_size_mask) * TLB2_WAYS];
    if (tlb_entry->tag == tag) {
        // update LRU bits
        tlb_entry[0].lru_bits  = 0x3;
        tlb_entry[1].lru_bits  = 0x2;
        tlb_entry[2].lru_bits &= 0x1;
        tlb_entry[3].lru_bits &= 0x1;
    } else if (tlb_entry[1].tag == tag) {
        tlb_entry = &tlb_entry[1];
        // update LRU bits
        tlb_entry[0].lru_bits  = 0x2;
        tlb_entry[1].lru_bits  = 0x3;
        tlb_entry[2].lru_bits &= 0x1;
        tlb_entry[3].lru_bits &= 0x1;
    } else if (tlb_entry[2].tag == tag) {
        tlb_entry = &tlb_entry[2];
        // update LRU bits
        tlb_entry[0].lru_bits &= 0x1;
        tlb_entry[1].lru_bits &= 0x1;
        tlb_entry[2].lru_bits  = 0x3;
        tlb_entry[3].lru_bits  = 0x2;
    } else if (tlb_entry[3].tag == tag) {
        tlb_entry = &tlb_entry[3];
        // update LRU bits
        tlb_entry[0].lru_bits &= 0x1;
        tlb_entry[1].lru_bits &= 0x1;
        tlb_entry[2].lru_bits  = 0x2;
        tlb_entry[3].lru_bits  = 0x3;
    } else {
        return nullptr;
    }
    return tlb_entry;
}

static uint32_t read_unaligned(uint32_t guest_va, uint8_t *host_va, uint32_t size);

template <class T>
inline T mmu_read_vmem(uint32_t guest_va) {
    TLBEntry *tlb1_entry, *tlb2_entry;
    uint8_t *host_va;

    const uint32_t tag = guest_va & ~0xFFFUL;

    // look up guest virtual address in the primary TLB
    tlb1_entry = &pCurTLB1[(guest_va >> PAGE_SIZE_BITS) & tlb_size_mask];
    if (tlb1_entry->tag == tag) { // primary TLB hit -> fast path
        host_va = (uint8_t *)(tlb1_entry->host_va_offset + guest_va);
    } else {
        // primary TLB miss -> look up address in the secondary TLB
        tlb2_entry = lookup_secondary_tlb(guest_va, tag);
        if (tlb2_entry == nullptr) {
            // secondary TLB miss ->
            // perform full address translation and refill the secondary TLB
            tlb2_entry = tlb2_refill(guest_va, 0);
        }

        if (tlb2_entry->flags & 1) { // is it a real memory region?
            // refill the primary TLB
            tlb1_entry->tag = tag;
            tlb1_entry->flags = 1;
            tlb1_entry->host_va_offset = tlb2_entry->host_va_offset;
            host_va = (uint8_t *)(tlb1_entry->host_va_offset + guest_va);
        } else { // otherwise, it's an access to a memory-mapped device
            return (
                tlb2_entry->reg_desc->devobj->read(tlb2_entry->reg_desc->start,
                    guest_va - tlb2_entry->reg_desc->start, sizeof(T))
            );
        }
    }

    // handle unaligned memory accesses
    if (sizeof(T) > 1 && (guest_va & (sizeof(T) - 1))) {
        return read_unaligned(guest_va, host_va, sizeof(T));
    }

    // handle aligned memory accesses
    switch(sizeof(T)) {
    case 1:
        return *host_va;
    case 2:
        return READ_WORD_BE_A(host_va);
    case 4:
        return READ_DWORD_BE_A(host_va);
    case 8:
        return READ_QWORD_BE_A(host_va);
    }
}

// explicitely instantiate all required mmu_read_vmem variants
// to avoid linking errors
template uint8_t  mmu_read_vmem<uint8_t>(uint32_t guest_va);
template uint16_t mmu_read_vmem<uint16_t>(uint32_t guest_va);
template uint32_t mmu_read_vmem<uint32_t>(uint32_t guest_va);
template uint64_t mmu_read_vmem<uint64_t>(uint32_t guest_va);

static uint32_t read_unaligned(uint32_t guest_va, uint8_t *host_va, uint32_t size)
{
    uint32_t result = 0;

    // is it a misaligned cross-page read?
    if (((guest_va & 0xFFF) + size) > 0x1000) {
        // Break such a memory access into multiple, bytewise accesses.
        // Because such accesses suffer a performance penalty, they will be
        // presumably very rare so don't waste time optimizing the code below.
        for (int i = 0; i < size; guest_va++, i++) {
            result = (result << 8) | mmu_read_vmem<uint8_t>(guest_va);
        }
    } else {
        switch(size) {
        case 2:
            return READ_WORD_BE_U(host_va);
        case 4:
            return READ_DWORD_BE_U(host_va);
        case 8: // FIXME: should we raise alignment exception here?
            return READ_QWORD_BE_U(host_va);
        }
    }
    return result;
}

/** Grab a value from memory into a register */
uint8_t mem_grab_byte(uint32_t addr) {
    tlb_translate_addr(addr);

    /* data address translation if enabled */
    if (ppc_state.msr & 0x10) {
        addr = ppc_mmu_addr_translate(addr, 0);
    }

    return read_phys_mem<uint8_t, true>(&last_read_area, addr);
}

uint16_t mem_grab_word(uint32_t addr) {
    tlb_translate_addr(addr);

    if (addr & 1) {
        return mem_grab_unaligned(addr, 2);
    }

    /* data address translation if enabled */
    if (ppc_state.msr & 0x10) {
        addr = ppc_mmu_addr_translate(addr, 0);
    }

    return read_phys_mem<uint16_t, true>(&last_read_area, addr);
}

uint32_t mem_grab_dword(uint32_t addr) {
    tlb_translate_addr(addr);

    if (addr & 3) {
        return mem_grab_unaligned(addr, 4);
    }

    /* data address translation if enabled */
    if (ppc_state.msr & 0x10) {
        addr = ppc_mmu_addr_translate(addr, 0);
    }

    return read_phys_mem<uint32_t, true>(&last_read_area, addr);
}

uint64_t mem_grab_qword(uint32_t addr) {
    tlb_translate_addr(addr);

    if (addr & 7) {
        LOG_F(ERROR, "SOS! Attempt to read unaligned QWORD at 0x%08X\n", addr);
        exit(-1);    // FIXME!
    }

    /* data address translation if enabled */
    if (ppc_state.msr & 0x10) {
        addr = ppc_mmu_addr_translate(addr, 0);
    }

    return read_phys_mem<uint64_t, true>(&last_read_area, addr);
}

uint8_t* quickinstruction_translate(uint32_t addr) {
    uint8_t* real_addr;

#ifdef MMU_PROFILING
    exec_reads_total++;
#endif

    /* perform instruction address translation if enabled */
    if (ppc_state.msr & 0x20) {
        addr = ppc_mmu_instr_translate(addr);
    }

    if (addr >= last_exec_area.start && addr <= last_exec_area.end) {
        real_addr = last_exec_area.mem_ptr + (addr - last_exec_area.start);
        ppc_set_cur_instruction(real_addr);
    } else {
        AddressMapEntry* entry = mem_ctrl_instance->find_range(addr);
        if (entry && entry->type & (RT_ROM | RT_RAM)) {
            last_exec_area.start   = entry->start;
            last_exec_area.end     = entry->end;
            last_exec_area.mem_ptr = entry->mem_ptr;
            real_addr              = last_exec_area.mem_ptr + (addr - last_exec_area.start);
            ppc_set_cur_instruction(real_addr);
        } else {
            LOG_F(WARNING, "attempt to execute code at %08X!\n", addr);
            exit(-1);    // FIXME: ugly error handling, must be the proper exception!
        }
    }

    return real_addr;
}

uint64_t mem_read_dbg(uint32_t virt_addr, uint32_t size) {
    uint32_t save_dsisr, save_dar;
    uint64_t ret_val;

    /* save MMU-related CPU state */
    save_dsisr            = ppc_state.spr[SPR::DSISR];
    save_dar              = ppc_state.spr[SPR::DAR];
    mmu_exception_handler = dbg_exception_handler;

    try {
        switch (size) {
        case 1:
            ret_val = mem_grab_byte(virt_addr);
            break;
        case 2:
            ret_val = mem_grab_word(virt_addr);
            break;
        case 4:
            ret_val = mem_grab_dword(virt_addr);
            break;
        case 8:
            ret_val = mem_grab_qword(virt_addr);
            break;
        default:
            ret_val = mem_grab_byte(virt_addr);
        }
    } catch (std::invalid_argument& exc) {
        /* restore MMU-related CPU state */
        mmu_exception_handler     = ppc_exception_handler;
        ppc_state.spr[SPR::DSISR] = save_dsisr;
        ppc_state.spr[SPR::DAR]   = save_dar;

        /* rethrow MMU exception */
        throw exc;
    }

    /* restore MMU-related CPU state */
    mmu_exception_handler     = ppc_exception_handler;
    ppc_state.spr[SPR::DSISR] = save_dsisr;
    ppc_state.spr[SPR::DAR]   = save_dar;

    return ret_val;
}

void ppc_mmu_init() {
    mmu_exception_handler = ppc_exception_handler;

    // invalidate all TLB entries
    for(auto &tlb_el : mode1_tlb1) {
        tlb_el.tag = TLB_INVALID_TAG;
        tlb_el.flags = 0;
        tlb_el.lru_bits = 0;
        tlb_el.host_va_offset = 0;
    }

    for(auto &tlb_el : mode2_tlb1) {
        tlb_el.tag = TLB_INVALID_TAG;
        tlb_el.flags = 0;
        tlb_el.lru_bits = 0;
        tlb_el.host_va_offset = 0;
    }

    for(auto &tlb_el : mode3_tlb1) {
        tlb_el.tag = TLB_INVALID_TAG;
        tlb_el.flags = 0;
        tlb_el.lru_bits = 0;
        tlb_el.host_va_offset = 0;
    }

    for(auto &tlb_el : mode1_tlb2) {
        tlb_el.tag = TLB_INVALID_TAG;
        tlb_el.flags = 0;
        tlb_el.lru_bits = 0;
        tlb_el.host_va_offset = 0;
    }

    for(auto &tlb_el : mode2_tlb2) {
        tlb_el.tag = TLB_INVALID_TAG;
        tlb_el.flags = 0;
        tlb_el.lru_bits = 0;
        tlb_el.host_va_offset = 0;
    }

    for(auto &tlb_el : mode3_tlb2) {
        tlb_el.tag = TLB_INVALID_TAG;
        tlb_el.flags = 0;
        tlb_el.lru_bits = 0;
        tlb_el.host_va_offset = 0;
    }

    mmu_change_mode();

#ifdef MMU_PROFILING
    gProfilerObj->register_profile("PPC_MMU",
        std::unique_ptr<BaseProfile>(new MMUProfile()));
#endif
}
