/* USF Maker extensions to lazyusf2 - see usfx.h.
 *
 * Distributed under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "usf.h"
#include "usf_internal.h"
#include "usfx.h"

#include "api/callbacks.h"
#include "main/main.h"
#include "memory/memory.h"
#include "r4300/cp0.h"
#include "r4300/interupt.h"
#include "r4300/r4300.h"

/* provided by usf.c */
int usf_startup(usf_state_t * state);

/* Layout of a Project64 1.x save state:
 *   0x000 magic, RDRAM size, ROM header (0x40), VI timer, PC
 *   0x050 GPR[32] (64 bit)        0x150 FPR[32] (64 bit)
 *   0x250 CP0[32]                 0x2D0 FPCR[32]
 *   0x350 HI, LO (64 bit)         0x360 RDRAM regs[10]
 *   0x388 SP regs[10]             0x3B0 DPC regs[10]
 *   0x3D8 MI regs[4]              0x3E8 VI regs[14]
 *   0x420 AI regs[6]              0x438 PI regs[13]
 *   0x46C RI regs[8]              0x48C SI regs[4]
 *   0x49C TLB[32] (5 words each)  0x71C PIF RAM (0x40)
 *   0x75C RDRAM, then SP DMEM (0x1000) and IMEM (0x1000)
 */
#define PJ64_HEADER_SIZE 0x75C

size_t usfx_state_file_size(uint32_t rdram_size)
{
    return (size_t)PJ64_HEADER_SIZE + rdram_size + 0x2000;
}

void usfx_set_rom_shared(void * state, const uint8_t * rom_words, size_t size)
{
    usf_state_t * s = USF_STATE;

    if (s->g_rom && !s->usfx.rom_shared)
        free(s->g_rom);

    /* the core only ever reads through this pointer */
    s->g_rom = (unsigned char *)(uintptr_t)rom_words;
    s->g_rom_size = (int)size;
    s->usfx.rom_shared = 1;
}

void usfx_set_controller(void * state, int port, int present)
{
    if (port >= 0 && port < 4)
        USF_STATE->usfx.controller_present[port] = present ? 1 : 0;
}

void usfx_set_buttons(void * state, int port, uint32_t buttons)
{
    if (port >= 0 && port < 4)
        USF_STATE->usfx.buttons[port] = buttons;
}

void usfx_set_audio_callback(void * state, usfx_audio_fn fn, void * user)
{
    USF_STATE->usfx.audio_fn = fn;
    USF_STATE->usfx.audio_user = user;
}

void usfx_set_pi_dma_callback(void * state, usfx_pi_dma_fn fn, void * user)
{
    USF_STATE->usfx.pi_dma_fn = fn;
    USF_STATE->usfx.pi_dma_user = user;
}

void usfx_set_fix_code_mirrors(void * state, int enable)
{
    USF_STATE->usfx.fix_code_mirrors = enable ? 1 : 0;
}

void usfx_track_rom(void * state, int enable)
{
    usf_state_t * s = USF_STATE;

    free(s->usfx.rom_cover);
    s->usfx.rom_cover = NULL;
    s->usfx.rom_cover_bytes = 0;

    if (enable)
    {
        /* one bit per word for the largest cartridge the bus can address */
        size_t bytes = (0x4000000u / 4) / 8;
        s->usfx.rom_cover = (uint8_t *) calloc(1, bytes);
        if (s->usfx.rom_cover)
            s->usfx.rom_cover_bytes = bytes;
    }
}

const uint8_t * usfx_rom_coverage(void * state, size_t * bytes)
{
    if (bytes)
        *bytes = USF_STATE->usfx.rom_cover_bytes;
    return USF_STATE->usfx.rom_cover;
}

void usfx_clear_rom_coverage(void * state)
{
    if (USF_STATE->usfx.rom_cover)
        memset(USF_STATE->usfx.rom_cover, 0, USF_STATE->usfx.rom_cover_bytes);
}

void usfx_boot_from_rom(void * state, uint32_t rdram_size)
{
    usf_state_t * s = USF_STATE;
    static const unsigned char magic[4] = { 0xC8, 0xA6, 0xD8, 0x23 };

    if (rdram_size != 0x400000)
        rdram_size = 0x800000;

    s->usfx.boot_from_rom = 1;

    /* usf_startup() reads the RDRAM size from the (otherwise unused) save state */
    if (s->save_state && s->save_state_size >= 8)
    {
        memcpy(s->save_state, magic, 4);
        s->save_state[4] = (unsigned char)(rdram_size & 0xff);
        s->save_state[5] = (unsigned char)((rdram_size >> 8) & 0xff);
        s->save_state[6] = (unsigned char)((rdram_size >> 16) & 0xff);
        s->save_state[7] = (unsigned char)((rdram_size >> 24) & 0xff);
    }
}

int usfx_run(void * state, int stop_mask)
{
    usf_state_t * s = USF_STATE;
    int reason;

    s->last_error = 0;
    s->error_message[0] = '\0';

    if (!s->MemoryState)
    {
        if (usf_startup(s) < 0)
            return USFX_STOP_ERROR;
    }

    s->usfx.active = 1;
    s->usfx.stop_mask = stop_mask;
    s->usfx.stop_reason = 0;

    s->stop = 0;
    main_run(s);

    s->usfx.active = 0;
    reason = s->usfx.stop_reason;
    if (s->last_error)
        reason |= USFX_STOP_ERROR;
    if (!reason)
    {
        /* the core stopped on its own without telling why */
        if (!s->last_error)
        {
            strcpy(s->error_message, "emulation stopped unexpectedly");
            s->last_error = s->error_message;
        }
        reason = USFX_STOP_ERROR;
    }
    return reason;
}

const char * usfx_last_error(void * state)
{
    return USF_STATE->last_error ? USF_STATE->last_error : "";
}

int usfx_snapshot_busy(void * state)
{
    usf_state_t * s = USF_STATE;
    struct node * e;
    int busy = 0;

    if (!s->MemoryState)
        return USFX_BUSY_NOTRUN;

    if (s->delay_slot || s->skip_jump || s->interupt_unsafe_state)
        busy |= USFX_BUSY_CPU;
    if (s->reset_hard_job)
        busy |= USFX_BUSY_RESET;

    /* Bring COUNT up to date; an event that is already due but has not been
       dispatched yet (the core only dispatches at branches) can not be
       expressed in the file. */
    update_count(s);
    if (s->cycle_count >= 0)
        busy |= USFX_BUSY_CHECK;

    for (e = s->q.first; e != NULL; e = e->next)
    {
        switch (e->data.type)
        {
        case SI_INT: busy |= USFX_BUSY_SI; break;
        case PI_INT: busy |= USFX_BUSY_PI; break;
        case SP_INT: busy |= USFX_BUSY_SP; break;
        case DP_INT: busy |= USFX_BUSY_DP; break;
        case CHECK_INT: busy |= USFX_BUSY_CHECK; break;
        case HW2_INT:
        case NMI_INT: busy |= USFX_BUSY_RESET; break;
        case AI_INT:
            /* With FIFO emulation the loader re-creates the pending audio
               interrupt; without it the interrupt would simply be lost. */
            if (!s->enableFIFOfull)
                busy |= USFX_BUSY_AI;
            break;
        default:
            break;
        }
    }

    if (s->g_pi.regs[PI_STATUS_REG] & 3)
        busy |= USFX_BUSY_PI;

    /* A buffer sitting in the second FIFO slot has not been handed to the
       DAC yet and the file format has no place for it.  That only matters
       if it actually contains sound. */
    if (s->g_ai.regs[AI_STATUS_REG] & 0x80000000u)
    {
        uint32_t addr = s->g_ai.fifo[1].address & 0x00fffff8;
        uint32_t len = s->g_ai.fifo[1].length & 0x0003fff8;
        uint32_t size = (uint32_t)s->g_ri.rdram.dram_size;
        int loud = 0;
        if (addr < size)
        {
            const int16_t * p16 = (const int16_t *)((const unsigned char *)s->g_rdram + addr);
            uint32_t n, i;
            if (len > size - addr)
                len = size - addr;
            n = len / 2;
            for (i = 0; i < n; i++)
            {
                if (p16[i] > 8 || p16[i] < -8)
                {
                    loud = 1;
                    break;
                }
            }
        }
        if (loud)
            busy |= USFX_BUSY_AI_FULL;
    }

    /* the loader forgets an audio interrupt that was raised but not yet
       acknowledged; only matters if the game listens for it */
    if ((s->g_r4300.mi.AudioIntrReg & MI_INTR_AI) &&
        (s->g_r4300.mi.regs[MI_INTR_MASK_REG] & MI_INTR_AI))
        busy |= USFX_BUSY_AI;

    return busy;
}

static unsigned char * put32(unsigned char * p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
    return p + 4;
}

static unsigned char * put64(unsigned char * p, uint64_t v)
{
    p = put32(p, (uint32_t)(v & 0xffffffffu));
    return put32(p, (uint32_t)(v >> 32));
}

size_t usfx_snapshot(void * state, uint8_t * out, size_t capacity)
{
    usf_state_t * s = USF_STATE;
    uint32_t rdram_size, count, vi_event;
    int32_t vi_timer;
    size_t total;
    unsigned char * p = out;
    int i;

    if (!s->MemoryState || !out)
        return 0;

    rdram_size = (uint32_t)s->g_ri.rdram.dram_size;
    if (rdram_size != 0x400000)
        rdram_size = 0x800000;
    total = usfx_state_file_size(rdram_size);
    if (capacity < total)
        return 0;

    update_count(s);
    count = s->g_cp0_regs[CP0_COUNT_REG];

    /* time until the next vertical interrupt, relative to COUNT */
    vi_event = get_event(s, VI_INT);
    vi_timer = (int32_t)((vi_event ? vi_event : s->g_vi.next_vi) - count);
    if (vi_timer < 1)
        vi_timer = 1;

    p = put32(p, 0x23D8A6C8u);
    p = put32(p, rdram_size);

    /* ROM header exactly as the cartridge image is held in memory */
    if (s->g_rom && s->g_rom_size >= 0x40)
        memcpy(p, s->g_rom, 0x40);
    else
        memset(p, 0, 0x40);
    p += 0x40;

    p = put32(p, (uint32_t)vi_timer);
    p = put32(p, s->PC->addr);

    for (i = 0; i < 32; i++) p = put64(p, (uint64_t)s->reg[i]);
    for (i = 0; i < 32; i++) p = put64(p, (uint64_t)s->reg_cop1_fgr_64[i]);
    for (i = 0; i < 32; i++) p = put32(p, s->g_cp0_regs[i]);

    p = put32(p, (uint32_t)s->FCR0);
    for (i = 1; i < 31; i++) p = put32(p, 0);
    p = put32(p, (uint32_t)s->FCR31);

    p = put64(p, (uint64_t)s->hi);
    p = put64(p, (uint64_t)s->lo);

    for (i = 0; i < 10; i++) p = put32(p, s->g_ri.rdram.regs[i]);

    for (i = 0; i < 8; i++) p = put32(p, s->g_sp.regs[i]);
    p = put32(p, s->g_sp.regs2[SP_PC_REG]);
    p = put32(p, s->g_sp.regs2[SP_IBIST_REG]);

    for (i = 0; i < 8; i++) p = put32(p, s->g_dp.dpc_regs[i]);
    p = put32(p, 0);
    p = put32(p, 0);

    for (i = 0; i < 4; i++) p = put32(p, s->g_r4300.mi.regs[i]);
    for (i = 0; i < 14; i++) p = put32(p, s->g_vi.regs[i]);
    for (i = 0; i < 6; i++) p = put32(p, s->g_ai.regs[i]);
    for (i = 0; i < 13; i++) p = put32(p, s->g_pi.regs[i]);
    for (i = 0; i < 8; i++) p = put32(p, s->g_ri.regs[i]);
    for (i = 0; i < 4; i++) p = put32(p, s->g_si.regs[i]);

    for (i = 0; i < 32; i++)
    {
        const tlb * t = &s->tlb_e[i];
        uint32_t g = t->g ? 1u : 0u;
        uint32_t lo0 = (((uint32_t)t->pfn_even << 6) & 0x3FFFFFC0u) | (((uint32_t)t->c_even & 7) << 3) |
                       (((uint32_t)t->d_even & 1) << 2) | (((uint32_t)t->v_even & 1) << 1) | g;
        uint32_t lo1 = (((uint32_t)t->pfn_odd << 6) & 0x3FFFFFC0u) | (((uint32_t)t->c_odd & 7) << 3) |
                       (((uint32_t)t->d_odd & 1) << 2) | (((uint32_t)t->v_odd & 1) << 1) | g;
        uint32_t hi = (((uint32_t)t->vpn2 << 13) & 0xFFFFE000u) | (uint32_t)t->asid;
        uint32_t mask = (((uint32_t)(unsigned short)t->mask) << 13) & 0x01FFE000u;

        p = put32(p, (t->v_even || t->v_odd) ? 1u : 0u);
        p = put32(p, mask);
        p = put32(p, hi);
        p = put32(p, lo0);
        p = put32(p, lo1);
    }

    memcpy(p, s->g_si.pif.ram, PIF_RAM_SIZE);
    p += PIF_RAM_SIZE;

    /* RDRAM and SP memory are kept as 32 bit words in host order, which on a
       little endian host is already the byte order of the file.  This is by
       far the bulk of the state, so copy it in one go when possible. */
    {
        const uint32_t probe = 1;
        if (*(const unsigned char *)&probe == 1)
        {
            memcpy(p, s->g_rdram, rdram_size);
            p += rdram_size;
            memcpy(p, s->g_sp.mem, SP_MEM_SIZE);
            p += SP_MEM_SIZE;
        }
        else
        {
            for (i = 0; i < (int)(rdram_size / 4); i++) p = put32(p, s->g_rdram[i]);
            for (i = 0; i < (int)(SP_MEM_SIZE / 4); i++) p = put32(p, s->g_sp.mem[i]);
        }
    }

    return (size_t)(p - out);
}

const struct usfx_stats * usfx_get_stats(void * state)
{
    return &USF_STATE->usfx.stats;
}

uint32_t usfx_rdram_size(void * state)
{
    return USF_STATE->MemoryState ? (uint32_t)USF_STATE->g_ri.rdram.dram_size : 0;
}

const uint8_t * usfx_rdram(void * state)
{
    return (const uint8_t *)USF_STATE->g_rdram;
}

uint32_t usfx_sample_rate(void * state)
{
    return (uint32_t)USF_STATE->SampleRate;
}

uint32_t usfx_pc(void * state)
{
    return USF_STATE->MemoryState && USF_STATE->PC ? USF_STATE->PC->addr : 0;
}

double usfx_seconds(void * state)
{
    usf_state_t * s = USF_STATE;
    /* the core runs COUNT at "aidacrate" ticks per second */
    double per_second = (double)s->ROM_PARAMS.aidacrate;
    if (per_second <= 0)
        per_second = 48681812.0;
    return (double)s->usfx.stats.vi_ticks / per_second;
}

uint32_t usfx_vi_rate(void * state)
{
    return (uint32_t)USF_STATE->ROM_PARAMS.vilimit;
}

int usfx_audio_hle_ok(void * state)
{
    usf_state_t * s = USF_STATE;
    if (s->usfx.stats.sp_audio_tasks == 0)
        return -1;
    return s->usfx.stats.hle_audio_fallbacks == 0 ? 1 : 0;
}

int usfx_cic_seed(void * state)
{
    return USF_STATE->g_si.pif.cic.seed;
}

/* ------------------------------------------------------------------------
 * Cartridge save hardware (see usfx.h).  The command sets follow what
 * mupen64plus implements for EEPROM, SRAM and FlashRAM.
 * ------------------------------------------------------------------------ */

enum
{
    FLASH_MODE_NOPES = 0,
    FLASH_MODE_ERASE,
    FLASH_MODE_WRITE,
    FLASH_MODE_READ,
    FLASH_MODE_STATUS
};

#define CART_BLOB_MAGIC 0x31565355u   /* "USV1" */
#define CART_BLOB_HEADER 32

static uint8_t * cart_area(uint8_t ** slot, size_t size)
{
    if (!*slot)
    {
        *slot = (uint8_t *)malloc(size);
        if (*slot)
            memset(*slot, 0xff, size);
    }
    return *slot;
}

void usfx_set_cart_save(void * state, int enable, int eeprom_16k)
{
    struct usfx_cart_save * c = &USF_STATE->usfx.cart;
    c->enabled = enable ? 1 : 0;
    c->eeprom_16k = eeprom_16k ? 1 : 0;
}

void usfx_cart_free(struct usf_state * s)
{
    struct usfx_cart_save * c = &s->usfx.cart;
    free(c->eeprom);
    free(c->sram);
    free(c->flash);
    c->eeprom = c->sram = c->flash = 0;
    c->dirty = 0;
    c->use_flashram = 0;
    c->flash_mode = FLASH_MODE_NOPES;
    c->flash_status = 0;
    c->flash_erase_offset = 0;
    c->flash_write_pointer = 0;
}

int usfx_cart_pif_command(struct usf_state * s, uint8_t * cmd)
{
    struct usfx_cart_save * c = &s->usfx.cart;
    uint8_t * mem;
    unsigned tx, rx, address;

    if (!c->enabled)
        return 0;

    /* cmd[0] = bytes sent, cmd[1] = bytes expected back, cmd[2] = command,
       followed by the remaining sent bytes and then room for the answer */
    tx = cmd[0] & 0x3f;
    rx = cmd[1] & 0x3f;
    s->usfx.stats.eeprom_cmds++;

    switch (cmd[2])
    {
    case 0x00: /* status: device type (2 bytes) and status */
    case 0xff: /* reset */
        if (rx > 0) cmd[2 + tx] = 0x00;
        if (rx > 1) cmd[3 + tx] = c->eeprom_16k ? 0xc0 : 0x80;
        if (rx > 2) cmd[4 + tx] = 0x00;
        break;

    case 0x04: /* read one 8 byte block */
        address = (unsigned)cmd[3] * 8;
        mem = cart_area(&c->eeprom, USFX_EEPROM_BYTES);
        if (tx >= 2 && rx >= 8 && mem && address + 8 <= USFX_EEPROM_BYTES)
            memcpy(&cmd[4], mem + address, 8);
        else
            cmd[1] |= 0x40;
        break;

    case 0x05: /* write one 8 byte block; answers with a status byte */
        address = (unsigned)cmd[3] * 8;
        mem = cart_area(&c->eeprom, USFX_EEPROM_BYTES);
        if (tx >= 10 && mem && address + 8 <= USFX_EEPROM_BYTES)
        {
            memcpy(mem + address, &cmd[4], 8);
            c->dirty |= USFX_SAVE_EEPROM;
            if (rx > 0) cmd[12] = 0x00;
        }
        else
            cmd[1] |= 0x40;
        break;

    default:   /* real time clock and anything else: nobody answers */
        cmd[1] |= 0x80;
        break;
    }
    return 1;
}

static void flash_command(struct usf_state * s, uint32_t command)
{
    struct usfx_cart_save * c = &s->usfx.cart;
    uint8_t * mem;
    uint32_t i;

    s->usfx.stats.flash_cmds++;
    switch (command & 0xff000000u)
    {
    case 0x4b000000u:   /* set the sector to erase */
        c->flash_erase_offset = (command & 0xffff) * 128;
        break;
    case 0x78000000u:   /* erase mode */
        c->flash_mode = FLASH_MODE_ERASE;
        c->flash_status = 0x1111800800c20000ULL;
        break;
    case 0xa5000000u:   /* set the page to write */
        c->flash_erase_offset = (command & 0xffff) * 128;
        c->flash_status = 0x1111800400c20000ULL;
        break;
    case 0xb4000000u:   /* write mode */
        c->flash_mode = FLASH_MODE_WRITE;
        break;
    case 0xd2000000u:   /* execute */
        mem = cart_area(&c->flash, USFX_FLASH_BYTES);
        if (mem && c->flash_mode == FLASH_MODE_ERASE)
        {
            for (i = 0; i < 128; i++)
                mem[((c->flash_erase_offset + i) % USFX_FLASH_BYTES) ^ S8] = 0xff;
            c->dirty |= USFX_SAVE_FLASH;
        }
        else if (mem && c->flash_mode == FLASH_MODE_WRITE)
        {
            const uint8_t * dram = (const uint8_t *)s->g_ri.rdram.dram;
            const uint32_t dram_size = (uint32_t)s->g_ri.rdram.dram_size;
            for (i = 0; i < 128; i++)
            {
                const uint32_t a = (c->flash_write_pointer + i) & 0x7fffff;
                if (a < dram_size)
                    mem[((c->flash_erase_offset + i) % USFX_FLASH_BYTES) ^ S8] = dram[a ^ S8];
            }
            c->dirty |= USFX_SAVE_FLASH;
        }
        c->flash_mode = FLASH_MODE_NOPES;
        break;
    case 0xe1000000u:   /* status mode */
        c->flash_mode = FLASH_MODE_STATUS;
        c->flash_status = 0x1111800100c20000ULL;
        break;
    case 0xf0000000u:   /* read mode */
        c->flash_mode = FLASH_MODE_READ;
        c->flash_status = 0x11118004f0000000ULL;
        break;
    default:
        break;
    }
}

int usfx_cart_read_status(void * opaque, uint32_t address, uint32_t * value)
{
    struct usf_state * s = (struct usf_state *)opaque;
    struct usfx_cart_save * c = &s->usfx.cart;

    *value = 0;
    if (!c->enabled || c->use_flashram == -1 || (address & 0xffff) != 0)
        return 0;
    c->use_flashram = 1;
    *value = (uint32_t)(c->flash_status >> 32);
    return 0;
}

int usfx_cart_write_command(void * opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct usf_state * s = (struct usf_state *)opaque;
    struct usfx_cart_save * c = &s->usfx.cart;

    if (!c->enabled || c->use_flashram == -1 || (address & 0xffff) != 0)
        return 0;
    c->use_flashram = 1;
    flash_command(s, value & mask);
    return 0;
}

int usfx_cart_pi_dma(struct usf_state * s, int to_cart, uint32_t cart_addr, uint32_t dram_addr, uint32_t length)
{
    struct usfx_cart_save * c = &s->usfx.cart;
    uint8_t * dram = (uint8_t *)s->g_ri.rdram.dram;
    const uint32_t dram_size = (uint32_t)s->g_ri.rdram.dram_size;
    uint32_t i;

    if (!c->enabled)
        return 0;

    s->usfx.stats.save_dmas++;
    dram_addr &= 0x00ffffff;
    length = (length & 0x00ffffff) + 1;
    cart_addr -= 0x08000000u;

    if (c->use_flashram != 1)
    {
        /* battery backed RAM: plain copies */
        uint8_t * mem = cart_area(&c->sram, USFX_SRAM_BYTES);
        c->use_flashram = -1;
        if (!mem)
            return 1;
        for (i = 0; i < length; i++)
        {
            const uint32_t d = dram_addr + i;
            const uint32_t m = (cart_addr + i) % USFX_SRAM_BYTES;
            if (d >= dram_size)
                break;
            if (to_cart)
                mem[m ^ S8] = dram[d ^ S8];
            else
                dram[d ^ S8] = mem[m ^ S8];
        }
        if (to_cart)
            c->dirty |= USFX_SAVE_SRAM;
        return 1;
    }

    if (to_cart)
    {
        /* the page itself is fetched from RAM when the write is executed */
        if (c->flash_mode == FLASH_MODE_WRITE)
            c->flash_write_pointer = dram_addr;
        return 1;
    }

    if (c->flash_mode == FLASH_MODE_STATUS)
    {
        /* the 8 byte "silicon id" */
        if (dram_addr + 8 <= dram_size)
        {
            uint32_t * w = (uint32_t *)s->g_ri.rdram.dram;
            w[dram_addr / 4] = (uint32_t)(c->flash_status >> 32);
            w[dram_addr / 4 + 1] = (uint32_t)c->flash_status;
        }
    }
    else if (c->flash_mode == FLASH_MODE_READ)
    {
        uint8_t * mem = cart_area(&c->flash, USFX_FLASH_BYTES);
        const uint32_t base = (cart_addr & 0xffff) * 2;
        if (!mem)
            return 1;
        for (i = 0; i < length; i++)
        {
            const uint32_t d = dram_addr + i;
            if (d >= dram_size)
                break;
            dram[d ^ S8] = mem[((base + i) % USFX_FLASH_BYTES) ^ S8];
        }
    }
    return 1;
}

size_t usfx_cart_save_max(void)
{
    return CART_BLOB_HEADER + USFX_EEPROM_BYTES + USFX_SRAM_BYTES + USFX_FLASH_BYTES;
}

static void blob_put32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t blob_get32(const uint8_t * p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t usfx_cart_save_export(void * state, uint8_t * out, size_t capacity)
{
    struct usfx_cart_save * c = &USF_STATE->usfx.cart;
    size_t need = CART_BLOB_HEADER;
    uint8_t * p;

    if (!c->enabled)
        return 0;
    if (!c->dirty && !c->use_flashram && c->flash_mode == FLASH_MODE_NOPES)
        return 0;   /* still a factory fresh cartridge */
    if ((c->dirty & USFX_SAVE_EEPROM) && c->eeprom) need += USFX_EEPROM_BYTES;
    if ((c->dirty & USFX_SAVE_SRAM) && c->sram) need += USFX_SRAM_BYTES;
    if ((c->dirty & USFX_SAVE_FLASH) && c->flash) need += USFX_FLASH_BYTES;
    if (!out || capacity < need)
        return 0;

    blob_put32(out, CART_BLOB_MAGIC);
    blob_put32(out + 4, (uint32_t)c->dirty);
    blob_put32(out + 8, (uint32_t)c->use_flashram);
    blob_put32(out + 12, (uint32_t)c->flash_mode);
    blob_put32(out + 16, (uint32_t)c->flash_status);
    blob_put32(out + 20, (uint32_t)(c->flash_status >> 32));
    blob_put32(out + 24, c->flash_erase_offset);
    blob_put32(out + 28, c->flash_write_pointer);
    p = out + CART_BLOB_HEADER;
    if ((c->dirty & USFX_SAVE_EEPROM) && c->eeprom) { memcpy(p, c->eeprom, USFX_EEPROM_BYTES); p += USFX_EEPROM_BYTES; }
    if ((c->dirty & USFX_SAVE_SRAM) && c->sram) { memcpy(p, c->sram, USFX_SRAM_BYTES); p += USFX_SRAM_BYTES; }
    if ((c->dirty & USFX_SAVE_FLASH) && c->flash) { memcpy(p, c->flash, USFX_FLASH_BYTES); p += USFX_FLASH_BYTES; }
    return (size_t)(p - out);
}

int usfx_cart_save_import(void * state, const uint8_t * data, size_t size)
{
    usf_state_t * s = USF_STATE;
    struct usfx_cart_save * c = &s->usfx.cart;
    const int enabled = c->enabled, big = c->eeprom_16k;
    uint32_t dirty;
    size_t need = CART_BLOB_HEADER;
    const uint8_t * p;

    usfx_cart_free(s);
    c->enabled = enabled;
    c->eeprom_16k = big;
    if (!data || size == 0)
        return 1;
    if (size < CART_BLOB_HEADER || blob_get32(data) != CART_BLOB_MAGIC)
        return 0;
    dirty = blob_get32(data + 4) & (USFX_SAVE_EEPROM | USFX_SAVE_SRAM | USFX_SAVE_FLASH);
    if (dirty & USFX_SAVE_EEPROM) need += USFX_EEPROM_BYTES;
    if (dirty & USFX_SAVE_SRAM) need += USFX_SRAM_BYTES;
    if (dirty & USFX_SAVE_FLASH) need += USFX_FLASH_BYTES;
    if (size < need)
        return 0;

    c->use_flashram = (int)blob_get32(data + 8);
    if (c->use_flashram != 1 && c->use_flashram != -1)
        c->use_flashram = 0;
    c->flash_mode = (int)blob_get32(data + 12);
    if (c->flash_mode < FLASH_MODE_NOPES || c->flash_mode > FLASH_MODE_STATUS)
        c->flash_mode = FLASH_MODE_NOPES;
    c->flash_status = (uint64_t)blob_get32(data + 16) | ((uint64_t)blob_get32(data + 20) << 32);
    c->flash_erase_offset = blob_get32(data + 24);
    c->flash_write_pointer = blob_get32(data + 28);
    p = data + CART_BLOB_HEADER;
    if (dirty & USFX_SAVE_EEPROM)
    {
        if (cart_area(&c->eeprom, USFX_EEPROM_BYTES)) { memcpy(c->eeprom, p, USFX_EEPROM_BYTES); c->dirty |= USFX_SAVE_EEPROM; }
        p += USFX_EEPROM_BYTES;
    }
    if (dirty & USFX_SAVE_SRAM)
    {
        if (cart_area(&c->sram, USFX_SRAM_BYTES)) { memcpy(c->sram, p, USFX_SRAM_BYTES); c->dirty |= USFX_SAVE_SRAM; }
        p += USFX_SRAM_BYTES;
    }
    if (dirty & USFX_SAVE_FLASH)
    {
        if (cart_area(&c->flash, USFX_FLASH_BYTES)) { memcpy(c->flash, p, USFX_FLASH_BYTES); c->dirty |= USFX_SAVE_FLASH; }
        p += USFX_FLASH_BYTES;
    }
    return 1;
}

/* ------------------------------------------------------------------------
 * Aleck64 board (see usfx.h)
 * ------------------------------------------------------------------------ */

#define A64_BLOB_MAGIC 0x34364C41u   /* "AL64" */
#define A64_BLOB_HEADER 16

void usfx_set_aleck64(void * state, int enable)
{
    struct usfx_aleck64 * a = &USF_STATE->usfx.a64;
    a->enabled = enable ? 1 : 0;
    if (a->enabled && !a->in0 && !a->in1 && !a->inmj)
    {
        /* all DIP switches off, nothing pressed */
        a->in0 = 0xffffffffu;
        a->in1 = 0xffffffffu;
        a->inmj = 0xffffffffu;
    }
}

void usfx_set_aleck64_inputs(void * state, uint32_t in0, uint32_t in1, uint32_t inmj)
{
    struct usfx_aleck64 * a = &USF_STATE->usfx.a64;
    a->in0 = in0;
    a->in1 = in1;
    a->inmj = inmj;
}

void usfx_a64_free(struct usf_state * s)
{
    free(s->usfx.a64.sdram);
    s->usfx.a64.sdram = 0;
    s->usfx.a64.dip_read_offset = 0;
}

static uint32_t * a64_sdram(struct usfx_aleck64 * a)
{
    if (!a->sdram)
        a->sdram = (uint32_t *)calloc(USFX_A64_SDRAM_BYTES / 4, 4);
    return a->sdram;
}

int usfx_a64_read(void * opaque, uint32_t address, uint32_t * value)
{
    struct usf_state * s = (struct usf_state *)opaque;
    struct usfx_aleck64 * a = &s->usfx.a64;

    *value = 0;
    if ((address & 0xff800000u) == 0xc0000000u)
    {
        uint32_t * m = a64_sdram(a);
        if (m)
            *value = m[(address & 0x7ffffcu) >> 2];
    }
    else if ((address & 0xfffff000u) == 0xc0800000u)
    {
        switch ((address & 0xfffu) >> 2)
        {
        case 0: *value = a->in0; break;
        case 1: *value = a->in1; break;
        case 2:
            s->usfx.stats.a64_mj_reads++;
            switch ((a->dip_read_offset >> 8) & 0xff)
            {
            case 1: *value = a->inmj; break;
            case 2: *value = a->inmj << 8; break;
            case 4: *value = a->inmj << 16; break;
            case 8: *value = a->inmj >> 8; break;
            default: *value = 0; break;
            }
            break;
        default: break;
        }
    }
    return 0;
}

int usfx_a64_write(void * opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct usf_state * s = (struct usf_state *)opaque;
    struct usfx_aleck64 * a = &s->usfx.a64;

    if ((address & 0xff800000u) == 0xc0000000u)
    {
        uint32_t * m = a64_sdram(a);
        if (m)
        {
            uint32_t * w = &m[(address & 0x7ffffcu) >> 2];
            *w = (*w & ~mask) | (value & mask);
        }
    }
    else if ((address & 0xfffff000u) == 0xc0800000u && ((address & 0xfffu) >> 2) == 2)
    {
        a->dip_read_offset = (a->dip_read_offset & ~mask) | (value & mask);
    }
    return 0;
}

size_t usfx_aleck64_max(void)
{
    return A64_BLOB_HEADER + USFX_A64_SDRAM_BYTES;
}

size_t usfx_aleck64_export(void * state, uint8_t * out, size_t capacity)
{
    struct usfx_aleck64 * a = &USF_STATE->usfx.a64;
    const size_t need = A64_BLOB_HEADER + (a->sdram ? USFX_A64_SDRAM_BYTES : 0);

    if (!a->enabled || (!a->sdram && !a->dip_read_offset))
        return 0;
    if (!out || capacity < need)
        return 0;
    blob_put32(out, A64_BLOB_MAGIC);
    blob_put32(out + 4, a->dip_read_offset);
    blob_put32(out + 8, a->sdram ? 1u : 0u);
    blob_put32(out + 12, 0);
    if (a->sdram)
    {
        /* little endian on every host, so the blob is portable */
        uint8_t * p = out + A64_BLOB_HEADER;
        size_t i;
        for (i = 0; i < USFX_A64_SDRAM_BYTES / 4; i++)
            blob_put32(p + i * 4, a->sdram[i]);
    }
    return need;
}

int usfx_aleck64_import(void * state, const uint8_t * data, size_t size)
{
    usf_state_t * s = USF_STATE;
    struct usfx_aleck64 * a = &s->usfx.a64;
    size_t i;

    usfx_a64_free(s);
    if (!data || size == 0)
        return 1;
    if (size < A64_BLOB_HEADER || blob_get32(data) != A64_BLOB_MAGIC)
        return 0;
    a->dip_read_offset = blob_get32(data + 4);
    if (blob_get32(data + 8))
    {
        if (size < A64_BLOB_HEADER + USFX_A64_SDRAM_BYTES || !a64_sdram(a))
            return 0;
        for (i = 0; i < USFX_A64_SDRAM_BYTES / 4; i++)
            a->sdram[i] = blob_get32(data + A64_BLOB_HEADER + i * 4);
    }
    return 1;
}
