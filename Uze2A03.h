#include <avr/io.h>
#include <stdlib.h>
#include <string.h>
#include <avr/pgmspace.h>
#include <uzebox.h>
#include <spiram.h>
#include <petitfatfs/pffconf.h>
#include <petitfatfs/diskio.h>
#include <petitfatfs/pff.h>
#include "data/tiles.inc"

extern u8 ram_tiles[];
extern u8 vram[];
extern u8 mix_buf[];
extern BYTE rcv_spi();
#define	SD_SELECT()	PORTD &= ~(1<<6)
#define	SD_DESELECT()	PORTD |= (1<<6)

#define UZENET_EEPROM_ID0	32
#define UZENET_EEPROM_ID1	UZENET_EEPROM_ID0+1

extern volatile u8 mix_bank;
extern u16 joypad1_status_lo, joypad2_status_lo;
extern u16 joypad1_status_hi, joypad2_status_hi;
#define Wait200ns() asm volatile("lpm\n\tlpm\n\t")

#define BUFFER_SIZE	262

#define CONT_BTN_W	16
#define CONT_BAR_X	40
#define CONT_BAR_W	13*CONT_BTN_W
#define CONT_BTN_H	CONT_BTN_W
#define CONT_BAR_Y	0
#define CONT_BAR_H	CONT_BTN_H

#define TILE_CURSOR	129
#define TILE_WIN_TLC	TILE_CURSOR+1
#define TILE_WIN_TRC	TILE_WIN_TLC+1
#define TILE_WIN_BLC	TILE_WIN_TRC+3
#define TILE_WIN_BRC	TILE_WIN_BLC+1
#define TILE_WIN_TBAR	TILE_WIN_TRC+1
#define TILE_WIN_BBAR	TILE_WIN_TBAR+1
#define TILE_WIN_LBAR	TILE_WIN_BRC+1
#define TILE_WIN_RBAR	TILE_WIN_LBAR+1
#define TILE_WIN_SCRU	TILE_WIN_RBAR+1
#define TILE_WIN_SCRD	TILE_WIN_SCRU+1

FATFS fs;
static s16 accum_span[BUFFER_SIZE];
static u8 cony = 2;
static u32 detected_ram;
//static u16 spi_seeks;
static u16 oldpad, pad;
static u8 play_state = 0;
static u8 masterVolume = 64;
static u16 total_files;
static u8 dirty_sectors = 0;

#define PS_LOADED	1
#define PS_PLAYING	2
#define PS_STOP		4
#define PS_PAUSE	8
#define PS_SHUFFLE	16
#define PS_DRAWN	32

static u8 ptime_min,ptime_sec,ptime_frame;

#define PTIME_X	SCREEN_TILES_H-9
#define PTIME_Y	2

#define DEFAULT_COLOR_MASK	0b00000111

static const u8 bad_masks[] PROGMEM = {0,1,2,3,8,9,10,11,16,17,18,19,24,25,26,27,64,65,66,67,72,73,74,75,80,81,82,83,88,89,90,91,};//skip those not legible

static u8 SpiRamPrintString(u8 x, u8 y, u32 pos, u8 invert, u8 fill);
static void FileSelectWindow();
static void PreviousDir();
static void NextDir(char *s);
static void UMPrintChar(u8 x, u8 y, char c);
static void UMPrint(u8 x, u8 y, const char *s);
static void UMPrintRam(u8 x, u8 y, char *s);
static void SpiRamWriteStringEntryFlash(u32 pos, const char *s);
static void SpiRamWriteStringEntry(u32 pos, char prefix, char *s);
static u16 SpiRamStringLen(u32 pos);

static void PlayerInterface();
static void LoadPreferences();
static void SavePreferences();
static void DrawWindow(u8 x, u8 y, u8 w, u8 h, const char *title, const char *lb, const char *rb);

static void read_vgm_header();
static u8 start_vgm(u8 looping);
static void stop_vgm();
static FRESULT load_vgm(u8 reload);
static void update_2a03();

#define VGM_CACHE_BYTES		128
#define DMC_CACHE_BYTES		256

static struct{
	u32 base, size, pos;
	u32 buf_start;
	u16 buf_len;
	u8 buf[VGM_CACHE_BYTES];
}vgmC;


static u32 fc_acc = 0;
static u8 fc_phase= 0;

static void apu_step_by(u16 cpu_cycles);
static void parse_vgm();
static void reset_2a03();
static void sample_audio();
static void clock_lsfr();
static void write_reg(u8 reg, u8 val);
static void clock_envelopes();
static void clock_linear_counter();
static void clock_length_counters();
static inline u8 get_duty(u8 reg);
static u16 get_11_bit_timer(u8 reg_low, u8 reg_high);
static void clock_sweep_units();
	
static u8 NES_REG[24];//$4000-$4017


#define NES_CPU_HZ	1789773UL
#define AUDIO_RATE_HZ	(262UL * 60UL)	//15720 Hz

#define CPU_PER_SAMPLE_INT	(NES_CPU_HZ / AUDIO_RATE_HZ)	//113
#define CPU_PER_SAMPLE_REM	(NES_CPU_HZ % AUDIO_RATE_HZ)	//13413

#define VGM_BASE_ADDR	0x000400UL
static u32 vgm_size = 0;

static u16 vgm_samples_to_event = 0;	//how many audio samples until next VGM event
static u32 vgm_frac_44100 = 0;		//carry for 44.1k -> AUDIO_RATE_HZ conversion

static u32 apu_frac = 0;
static u8 ff_mul = 1;


const u8 length_table[2][16] PROGMEM = {
{0x0A, 0x14, 0x28, 0x50, 0xA0, 0x3C, 0x0E, 0x1A, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x48, 0x10, 0x20},
{0xFE, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1A, 0x1C, 0x1E}
};

const u8 duty_table[4][8] PROGMEM = {
{0, 0, 0, 0, 0, 0, 0, 1},//12.5%
{0, 0, 0, 0, 0, 0, 1, 1},//25%
{0, 0, 0, 0, 1, 1, 1, 1},//50%
{1, 1, 1, 1, 1, 1, 0, 0},//25% (inv.)
};

const u16 noise_table[16] PROGMEM = {
4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

const u8 tri_table[32] PROGMEM = {
15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

//Pulse 1 Variables
static u8 p1_output = 0;
static s16 p1_11_bit_timer = 0;
static u8 p1_wave_index = 0;
static u16 p1_length_counter = 0;
static u8 p1_envelope_divider = 0;
static u8 p1_decay_counter = 0;
static u8 p1_volume = 0;
static u8 p1_swp_div=0;
static u8 p1_swp_reload=0;
static u8 p1_swp_mute=0;

//Pulse 2 Variables
static u8 p2_output = 0;
static s16 p2_11_bit_timer = 0;
static u8 p2_wave_index = 0;
static u16 p2_length_counter = 0;
static u8 p2_envelope_divider = 0;
static u8 p2_decay_counter = 0;
static u8 p2_volume = 0;
static u8 p2_swp_div=0;
static u8 p2_swp_reload=0;
static u8 p2_swp_mute=0;

//Noise Variables
static u8 n_output = 0;
static s16 n_timer = 0;
static u16 n_length_counter = 0;
static u8 n_envelope_divider = 0;
static u8 n_decay_counter = 0;
static u8 n_volume = 0;
static u16 n_lsfr = 1;

static u8 n_idx_cached = 0xFF;
static u16 n_period_cached = 0;

//Triangle Variables
static u8 t_output = 0;
static s16 t_11_bit_timer = 0;
static u8 t_wave_index = 0;
static u16 t_length_counter = 0;
static u16 t_linear_counter = 0;
static u8 t_lin_reload = 0;

static u8 dmc_irq = 0, dmc_loop = 0, dmc_enable = 0;
static u8 dmc_rate_idx = 0;
static u8 dmc_output = 0x40;		//7-bit DAC (0..127), start mid
static u16 dmc_period = 428;		//CPU cycles/bit (rate idx 0)
static s16 dmc_timer = 0;		//down-counter in CPU cycles

static u16 dmc_start_addr = 0xC000;	//$4012: start = $C000 + val*64
static u16 dmc_cur_addr = 0xC000;
static u16 dmc_start_len = 1;		//$4013: length = val*16 + 1
static u16 dmc_bytes_remaining = 0;

static u8 dmc_shift = 0;		//output shift register
static u8 dmc_bits_remaining = 0;
static u8 dmc_sample_buffer = 0;	//fetched byte
static u8 dmc_sample_buffer_empty = 1;

typedef struct{
	u32 base;	//SPI RAM base where the file starts
	u32 size;	//file size in bytes
	u32 pos;	//offset within the file
	u32 data_off;	//VGM data offset(absolute file offset)
	u32 loop_off;	//VGM loop offset(absolute file offset) or 0
	u32 eof_off;	//VGM EOF offset(absolute file offset)
}VgmStream;//VGM streaming cache

static struct{
	u32 base;
	u32 size;
	u32 mask;	//power-of-two wrap mask or 0
	u32 seek;	//E0 base inside payload
	u32 buf_start;	//index into payload
	u16 buf_len;
	u8 buf[DMC_CACHE_BYTES];
}dmcC;//DMC sample cache(totally separate from VGM cache)

static VgmStream vstr;
static inline void vgm_stream_open(u32 base_addr, u32 file_size);
static inline void vgm_seek(u32 abs_off);
static inline u8 vgm_getc();
static inline u16 vgm_get16le();
static inline u32 vgm_get32le();
static inline void vgm_skip(u32 n);

//NTSC DMC bit-rate table (CPU cycles per bit)
static const u16 dmc_rate_table[16] PROGMEM =
{ 428,380,340,320,286,254,226,214,190,160,142,128,106,85,72,54 };

static u16 vgm_wait = 0;
static u8 NES_PLAYING = 0;
static u8 NES_LOOPING = 0;
static u32 VGM_EOF_OFFSET = 0;
static u32 VGM_TOTAL_NUM_SAMPLES = 0;
//static u32 VGM_RATE = 0;
static u32 VGM_DATA_OFFSET = 0;
static u32 VGM_NES_APU_CLOCK = 0;
static u32 VGM_LOOP_OFFSET = 0;

static inline u8 ram_r8(u32 a);
static inline u32 ram_r32le(u32 a);