/* USF Maker extensions to lazyusf2.
 *
 * lazyusf2 is a *player* core: it can only start from a Project64 save state
 * and it has no notion of controllers.  USF Maker needs to do the opposite:
 * boot a cartridge image from power-on, press buttons, and write the running
 * machine back out as a Project64 save state.  Everything needed for that is
 * collected here so the changes to the original sources stay small and are
 * easy to audit (search for "USFX" in the tree).
 *
 * All of these are no-ops unless explicitly enabled, so a state that never
 * touches this API behaves exactly like stock lazyusf2.  That property is
 * what lets USF Maker verify a generated file "the way a player would".
 *
 * This file is part of USF Maker and is distributed under the terms of the
 * GNU General Public License version 2 or (at your option) any later version.
 */
#ifndef USFX_H
#define USFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- controller buttons (bit layout of the 4 byte PIF response) -------- */
#define USFX_BTN_A        0x00000080u
#define USFX_BTN_B        0x00000040u
#define USFX_BTN_Z        0x00000020u
#define USFX_BTN_START    0x00000010u
#define USFX_BTN_DUP      0x00000008u
#define USFX_BTN_DDOWN    0x00000004u
#define USFX_BTN_DLEFT    0x00000002u
#define USFX_BTN_DRIGHT   0x00000001u
#define USFX_BTN_L        0x00002000u
#define USFX_BTN_R        0x00001000u
#define USFX_BTN_CUP      0x00000800u
#define USFX_BTN_CDOWN    0x00000400u
#define USFX_BTN_CLEFT    0x00000200u
#define USFX_BTN_CRIGHT   0x00000100u
/* analog stick: signed 8 bit X in bits 16-23, signed 8 bit Y in bits 24-31 */
#define USFX_STICK(x, y)  ((((uint32_t)(uint8_t)(int8_t)(x)) << 16) | (((uint32_t)(uint8_t)(int8_t)(y)) << 24))

/* ---- reasons usfx_run() returned --------------------------------------- */
#define USFX_STOP_AUDIO   0x01   /* an audio DMA was pushed                 */
#define USFX_STOP_VI      0x02   /* a vertical interrupt was raised         */
#define USFX_STOP_ERROR   0x04   /* the core reported an error              */

/* ---- reasons a save state can not be taken right now -------------------- */
#define USFX_BUSY_SI        0x001  /* serial (controller) transfer pending   */
#define USFX_BUSY_PI        0x002  /* cartridge transfer pending             */
#define USFX_BUSY_SP        0x004  /* signal processor task pending          */
#define USFX_BUSY_DP        0x008  /* display processor interrupt pending    */
#define USFX_BUSY_AI        0x010  /* audio interrupt would be lost          */
#define USFX_BUSY_AI_FULL   0x020  /* a queued audio buffer would be lost    */
#define USFX_BUSY_CHECK     0x040  /* an interrupt is about to be delivered  */
#define USFX_BUSY_CPU       0x080  /* CPU is between instructions            */
#define USFX_BUSY_RESET     0x100  /* reset in progress                      */
#define USFX_BUSY_NOTRUN    0x200  /* emulation has not been started         */

typedef void (*usfx_audio_fn)(void *user, const int16_t *stereo, size_t frames, uint32_t sample_rate);
typedef void (*usfx_pi_dma_fn)(void *user, uint32_t cart_addr, uint32_t dram_addr, uint32_t length);

struct usfx_stats
{
    uint64_t vi_count;          /* vertical interrupts                       */
    uint64_t vi_ticks;          /* emulated time: sum of the VI intervals, in COUNT units */
    uint64_t ai_dma_count;      /* audio buffers handed to the DAC           */
    uint64_t ai_frames;         /* stereo sample frames handed to the DAC    */
    uint64_t pi_dma_count;      /* cartridge -> RAM transfers                */
    uint64_t pi_dma_bytes;
    uint64_t rom_io_reads;      /* direct (non DMA) cartridge reads          */
    uint64_t input_polls;       /* controller button reads on port 1         */
    uint64_t status_polls;      /* controller status queries                 */
    uint64_t sp_audio_tasks;
    uint64_t sp_gfx_tasks;
    uint64_t sp_other_tasks;
    uint64_t hle_audio_fallbacks; /* audio tasks the HLE handed to the interpreter */
    uint64_t eeprom_cmds;       /* EEPROM commands answered                  */
    uint64_t save_dmas;         /* SRAM / FlashRAM transfers                 */
    uint64_t flash_cmds;        /* FlashRAM commands                         */
    uint64_t foreign_accesses;  /* loads/stores that reached the Aleck64 board through the TLB */
    uint64_t tlb_misses;        /* TLB refill exceptions raised for data accesses */
    uint64_t a64_mj_reads;      /* reads of the Aleck64 mahjong panel        */
};

/* ---- cartridge save hardware ---------------------------------------------
 * A player core has none, and stock lazyusf2 leaves the commands unanswered.
 * That is harmless when a tune is played from a save state, but a game that
 * is booted from power-on asks its EEPROM / SRAM / FlashRAM for the save
 * file, and several versions of the system library wait forever for a
 * "ready" that never comes.  While exploring, USF Maker therefore gives the
 * game what a brand new cartridge has: blank save memory that works.
 */
#define USFX_EEPROM_BYTES  0x800      /* 16 kbit; a 4 kbit part uses the first quarter */
#define USFX_SRAM_BYTES    0x8000
#define USFX_FLASH_BYTES   0x20000

#define USFX_SAVE_EEPROM   0x1        /* "dirty" bits: written since power-on */
#define USFX_SAVE_SRAM     0x2
#define USFX_SAVE_FLASH    0x4

struct usfx_cart_save
{
    int enabled;
    int eeprom_16k;             /* report a 16 kbit EEPROM instead of a 4 kbit one */
    int use_flashram;           /* 0 = not known yet, 1 = FlashRAM, -1 = SRAM */
    int flash_mode;
    uint64_t flash_status;
    uint32_t flash_erase_offset;
    uint32_t flash_write_pointer;
    int dirty;                  /* USFX_SAVE_* */
    uint8_t *eeprom;            /* allocated on first use, erased to 0xFF */
    uint8_t *sram;
    uint8_t *flash;
};

/* ---- Seta / Nintendo Aleck64 arcade board ---------------------------------
 * An N64 with extra hardware that the games reach through the TLB at
 * physical addresses an N64 does not have (the layout follows MAME):
 *   0xC0000000-0xC07FFFFF  8 MB of SDRAM
 *   0xC0800000-0xC0800FFF  DIP switches, coin / service / start inputs
 *                          (word 0: IN0, word 1: IN1, word 2: mahjong panel)
 *   anything else          reads 0, writes are ignored
 * The cartridges carry a CIC-NUS-5101.  None of this exists in a USF player;
 * the save state format has no room for the SDRAM either (it is carried
 * separately by usfx_aleck64_export()/import() while exploring).
 */
#define USFX_A64_SDRAM_BYTES 0x800000

struct usfx_aleck64
{
    int enabled;
    uint32_t in0, in1, inmj;    /* input port values as the game reads them (active low) */
    uint32_t dip_read_offset;   /* selects the part of the mahjong panel that word 2 returns */
    uint32_t *sdram;            /* allocated on first use, host order 32 bit words */
};

/* Per-state extension block; lives at the end of struct usf_state. */
struct usfx_state
{
    /* configuration */
    int boot_from_rom;          /* start at the IPL3 entry point instead of loading a save state */
    int fix_code_mirrors;       /* notice code rewritten through the other KSEG0/KSEG1 mirror */
    int rom_shared;             /* g_rom belongs to the caller and must not be freed */
    int controller_present[4];
    uint32_t buttons[4];

    /* run control */
    int active;                 /* usfx_run() is driving the core */
    int stop_mask;
    int stop_reason;

    /* callbacks */
    usfx_audio_fn audio_fn;
    void *audio_user;
    usfx_pi_dma_fn pi_dma_fn;
    void *pi_dma_user;

    /* cartridge coverage: one bit per 32 bit word of ROM */
    uint8_t *rom_cover;
    size_t rom_cover_bytes;

    struct usfx_stats stats;

    struct usfx_cart_save cart;
    struct usfx_aleck64 a64;
};

/* Size in bytes of a Project64 save state for the given RDRAM size. */
size_t usfx_state_file_size(uint32_t rdram_size);

/* Use a cartridge image owned by the caller instead of a private copy
   (several machines can share one image; the core never writes to it).
   The image must be in the core's layout: 32 bit words in host byte order,
   size a multiple of 64K.  It has to outlive the state. */
void usfx_set_rom_shared(void *state, const uint8_t *rom_words, size_t size);

/* Controllers.  A stock player always reports "nothing plugged in". */
void usfx_set_controller(void *state, int port, int present);
void usfx_set_buttons(void *state, int port, uint32_t buttons);

/* Callbacks.  The audio callback receives native-endian L,R interleaved
   frames exactly as a player would hand them to the sound card. */
void usfx_set_audio_callback(void *state, usfx_audio_fn fn, void *user);
void usfx_set_pi_dma_callback(void *state, usfx_pi_dma_fn fn, void *user);

/* Emulation fix that stock lazyusf2 does not have: notice when code that was
   executed through one of the KSEG0/KSEG1 mirrors is rewritten through the
   other.  Needed to boot some homebrew; leave off to behave like a player. */
void usfx_set_fix_code_mirrors(void *state, int enable);

/* Cartridge coverage tracking.  Unlike usf_set_trimming_mode() this does not
   change the emulation timing in any way.  Returns a bitmap with one bit per
   32 bit ROM word (bit n of byte n/8), or NULL when not enabled. */
void usfx_track_rom(void *state, int enable);
const uint8_t *usfx_rom_coverage(void *state, size_t *bytes);
void usfx_clear_rom_coverage(void *state);

/* Blank cartridge save memory (see struct usfx_cart_save).  Off by default,
   which is what a player has. */
void usfx_set_cart_save(void *state, int enable, int eeprom_16k);
/* The save memory is not part of a Project64 save state.  These two carry it
   from one machine to the next: export returns the number of bytes written
   (0 when there is nothing worth carrying, or the buffer is too small - at
   most usfx_cart_save_max() bytes are needed); import returns non-zero on
   success.  Importing an empty blob resets the memory to "blank". */
size_t usfx_cart_save_max(void);
size_t usfx_cart_save_export(void *state, uint8_t *out, size_t capacity);
int usfx_cart_save_import(void *state, const uint8_t *data, size_t size);

/* Aleck64 hardware (see struct usfx_aleck64).  Off by default.  Enabling
   it also applies, at the next cold boot, the IPL3 patch MAME uses for
   every Aleck64 cartridge. */
void usfx_set_aleck64(void *state, int enable);
void usfx_set_aleck64_inputs(void *state, uint32_t in0, uint32_t in1, uint32_t inmj);
/* SDRAM contents and board registers, to carry from a snapshot to the
   machine it is loaded into (like the cartridge save memory).  Export
   returns the number of bytes written (0: nothing worth carrying, or the
   buffer is too small - usfx_aleck64_max() bytes always suffice). */
size_t usfx_aleck64_max(void);
size_t usfx_aleck64_export(void *state, uint8_t *out, size_t capacity);
int usfx_aleck64_import(void *state, const uint8_t *data, size_t size);

/* Runs the machine until one of the events in stop_mask happens.
   Returns the USFX_STOP_* bits that fired.  On USFX_STOP_ERROR the message
   is available from usfx_last_error(). */
int usfx_run(void *state, int stop_mask);
const char *usfx_last_error(void *state);

/* Prepare a cold boot of the uploaded ROM (instead of loading a save state).
   rdram_size must be 0x400000 or 0x800000.  Call before the first run. */
void usfx_boot_from_rom(void *state, uint32_t rdram_size);

/* Non-zero (USFX_BUSY_* bits) when a save state taken right now would lose
   something the game is waiting for. */
int usfx_snapshot_busy(void *state);

/* Writes the machine as a Project64 1.x save state.  Returns the number of
   bytes written, or 0 if the buffer is too small / the core is not running. */
size_t usfx_snapshot(void *state, uint8_t *out, size_t capacity);

const struct usfx_stats *usfx_get_stats(void *state);
uint32_t usfx_rdram_size(void *state);
const uint8_t *usfx_rdram(void *state);
uint32_t usfx_sample_rate(void *state);
uint32_t usfx_pc(void *state);
/* Emulated time since the machine was started, in seconds. */
double usfx_seconds(void *state);
/* Vertical interrupts per second the game was authored for (50 or 60). */
uint32_t usfx_vi_rate(void *state);
/* 1 if the audio microcode currently in use is handled by the HLE, 0 if it
   had to be interpreted, -1 if no audio task has run yet. */
int usfx_audio_hle_ok(void *state);
int usfx_cic_seed(void *state);

/* ---- hooks called from the patched core files (not for applications) ---- */
struct usf_state;
/* PIF channel 4 (cartridge) command; returns 0 if save hardware is off. */
int usfx_cart_pif_command(struct usf_state *state, uint8_t *cmd);
/* PI DMA touching 0x08000000-0x0800FFFF; to_cart: RDRAM -> cartridge.
   Returns 0 if save hardware is off. */
int usfx_cart_pi_dma(struct usf_state *state, int to_cart, uint32_t cart_addr, uint32_t dram_addr, uint32_t length);
int usfx_cart_read_status(void *opaque, uint32_t address, uint32_t *value);
int usfx_cart_write_command(void *opaque, uint32_t address, uint32_t value, uint32_t mask);
void usfx_cart_free(struct usf_state *state);
int usfx_a64_read(void *opaque, uint32_t address, uint32_t *value);
int usfx_a64_write(void *opaque, uint32_t address, uint32_t value, uint32_t mask);
void usfx_a64_free(struct usf_state *state);

#ifdef __cplusplus
}
#endif

#endif
