#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "gnuboy.h"
#include "hw.h"
#include "cpu.h"
#include "sound.h"
#include "lcd.h"

// --- CrowPanel local patch (see VENDORED.md) -------------------------------
// A Game Boy ROM is 1-2 MB of 16 KB banks and the cartridge SRAM is up to
// 128 KB. The ESP32-P4 has only ~500 KB of internal SRAM but 32 MB of PSRAM,
// so the big cartridge allocations are forced into PSRAM. Small, hot
// structures (hw.rambanks / hw.vbanks in hw.c, the rombanks pointer table)
// deliberately stay in internal SRAM for speed. heap_caps memory is released
// with plain free(), so gnuboy's existing free() calls need no change.
#include "esp_heap_caps.h"
// Cube Boy patch: the boot-time preload loop below reads up to 128 ROM banks off
// SD back-to-back on the Arduino loopTask (CPU 1). With nothing yielding, CPU 1's
// IDLE task never runs and the task watchdog (idle_core_mask, ~5s -- Storage.ino)
// aborts mid-load on big carts (e.g. 1MB Pokemon Yellow). vTaskDelay() in the loop
// lets IDLE run and reset the WDT, so the load takes as long as it needs.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// Cube Boy patch on top: PSRAM when present, internal-SRAM fallback when the
// unit has none (hardware-verified: the Cardputer ADV reports psram=0 in both
// opi and qspi modes). This core is retro-go's bank-paging design: rombanks
// are loaded lazily from cart.romFile and RECLAIMED when allocation fails
// (gnuboy_load_bank below), so a big cart runs from however many banks fit -
// the fallback just must not hand gnuboy every last byte of internal RAM.
// GB_BANK_RESERVE keeps headroom for the app (Strings, SD, UI); below it the
// malloc returns NULL and the reclaim path takes over.
#define GB_BANK_RESERVE (28 * 1024)
static inline void *gb_big_malloc(size_t sz) {
	void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (p) return p;
	if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < sz + GB_BANK_RESERVE)
		return NULL;  // bank manager pages from SD instead
	return heap_caps_malloc(sz, MALLOC_CAP_8BIT);
}
// calloc feeds cart.rambanks (battery SRAM, <=32 KB for these carts) - that
// one has no reclaim path and must succeed, so no reserve check here.
static inline void *gb_big_calloc(size_t n, size_t sz) {
	void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	return p ? p : heap_caps_calloc(n, sz, MALLOC_CAP_8BIT);
}
#define GB_PSRAM_MALLOC(sz)    gb_big_malloc(sz)
#define GB_PSRAM_CALLOC(n, sz) gb_big_calloc(n, sz)
#define GBDBG(stage, fmt, ...) do { \
	printf("[GBDBG core %s] " fmt "\n", stage, ##__VA_ARGS__); \
	fflush(stdout); \
} while (0)
// Bank-paging state for the no-PSRAM path (see gnuboy_load_bank).
static int gb_live_bank = 1;   // switchable bank currently mapped by the MBC
static int gb_preloading = 0;  // inside the boot-time preload loop
static unsigned gb_debug_run_count = 0;  // bounded early-boot frame tracing
// ---------------------------------------------------------------------------

#define hw GB

// Set in the far future for VBA-M support
#define RTC_BASE 1893456000

#define BANK_SIZE 0x4000

static void gb_debug_dump_rom_bytes(const char *source, const byte *data,
									size_t base, size_t count)
{
	printf("[GBDBG ROMCHK] %s %05X-%05X:", source, (unsigned)base,
		(unsigned)(base + count - 1));
	for (size_t i = 0; i < count; ++i)
		printf(" %02X", data[i]);
	printf("\n");
	fflush(stdout);
}

// Verify the bytes the CPU will execute against a completely fresh SD-card
// read. This deliberately runs before reset: it distinguishes a bad ROM file
// from corruption while copying/storing banks in PSRAM, instead of inferring
// either one from the CPU's later symptoms.
static bool gb_debug_verify_preloaded_rom(long probed_file_size)
{
	const size_t expected_size = (size_t)cart.romsize * BANK_SIZE;
	byte file_bytes[1024];
	uint16_t file_sum = 0;
	uint16_t loaded_sum = 0;
	uint32_t file_fnv = 2166136261u;
	uint32_t loaded_fnv = 2166136261u;
	size_t compared = 0;
	size_t first_mismatch = (size_t)-1;
	size_t first_missing_bank = (size_t)-1;
	byte first_mismatch_file = 0;
	byte first_mismatch_loaded = 0;
	bool read_failed = false;

	clearerr(cart.romFile);
	errno = 0;
	int seek_result = fseek(cart.romFile, 0, SEEK_SET);
	GBDBG("V1", "verify begin expected=%u probed=%ld seek=%d errno=%d",
		(unsigned)expected_size, probed_file_size, seek_result, errno);
	if (seek_result != 0)
		return false;

	for (size_t base = 0; base < expected_size; base += sizeof(file_bytes))
	{
		size_t want = expected_size - base;
		if (want > sizeof(file_bytes)) want = sizeof(file_bytes);
		size_t got = fread(file_bytes, 1, want, cart.romFile);
		if (got != want)
		{
			GBDBG("V!", "fresh fread failed offset=%u want=%u got=%u errno=%d feof=%d ferror=%d",
				(unsigned)base, (unsigned)want, (unsigned)got, errno,
				feof(cart.romFile), ferror(cart.romFile));
			read_failed = true;
			break;
		}

		for (size_t i = 0; i < got; ++i)
		{
			const size_t absolute = base + i;
			const byte from_file = file_bytes[i];
			if (absolute != 0x014E && absolute != 0x014F)
				file_sum = (uint16_t)(file_sum + from_file);
			file_fnv = (file_fnv ^ from_file) * 16777619u;

			const size_t bank = absolute / BANK_SIZE;
			if (!cart.rombanks[bank])
			{
				if (first_missing_bank == (size_t)-1) first_missing_bank = bank;
				continue;
			}
			const byte from_loaded = cart.rombanks[bank][absolute % BANK_SIZE];
			if (absolute != 0x014E && absolute != 0x014F)
				loaded_sum = (uint16_t)(loaded_sum + from_loaded);
			loaded_fnv = (loaded_fnv ^ from_loaded) * 16777619u;
			if (from_file != from_loaded && first_mismatch == (size_t)-1)
			{
				first_mismatch = absolute;
				first_mismatch_file = from_file;
				first_mismatch_loaded = from_loaded;
			}
			++compared;
		}
	}

	const uint16_t header_sum = cart.rombanks[0]
		? (((uint16_t)cart.rombanks[0][0x014E] << 8) | cart.rombanks[0][0x014F])
		: 0;
	GBDBG("V2", "fresh file sum=%04X header=%04X match=%u fnv=%08X read_failed=%u",
		file_sum, header_sum, file_sum == header_sum, (unsigned)file_fnv,
		read_failed ? 1u : 0u);
	GBDBG("V3", "loaded sum=%04X header=%04X match=%u fnv=%08X compared=%u/%u missing_bank=%d",
		loaded_sum, header_sum,
		(first_missing_bank == (size_t)-1 && loaded_sum == header_sum) ? 1u : 0u,
		(unsigned)loaded_fnv, (unsigned)compared, (unsigned)expected_size,
		first_missing_bank == (size_t)-1 ? -1 : (int)first_missing_bank);
	if (first_mismatch == (size_t)-1)
		GBDBG("V4", "fresh file and loaded banks are byte-identical");
	else
	{
		GBDBG("V!", "FIRST MISMATCH offset=%05X bank=%u file=%02X loaded=%02X",
			(unsigned)first_mismatch, (unsigned)(first_mismatch / BANK_SIZE),
			first_mismatch_file, first_mismatch_loaded);
	}

	if (cart.rombanks[0])
		gb_debug_dump_rom_bytes("loaded", cart.rombanks[0] + 0x3E70, 0x3E70, 32);
	clearerr(cart.romFile);
	if (fseek(cart.romFile, 0x3E70, SEEK_SET) == 0
		&& fread(file_bytes, 1, 32, cart.romFile) == 32)
		gb_debug_dump_rom_bytes("fresh ", file_bytes, 0x3E70, 32);
	clearerr(cart.romFile);

	const bool size_matches = probed_file_size == (long)expected_size;
	const bool checksum_matches = !read_failed && file_sum == header_sum;
	const bool copies_match = first_mismatch == (size_t)-1;
	GBDBG("V5", "verdict size=%u checksum=%u copy=%u => %s",
		size_matches ? 1u : 0u, checksum_matches ? 1u : 0u,
		copies_match ? 1u : 0u,
		(size_matches && checksum_matches && copies_match) ? "VALID" : "REJECT");
	return size_matches && checksum_matches && copies_match;
}


// Note: Eventually we'll just pass a gb_host_t to init...
// But for now assume it's been configured before we were alled!
int gnuboy_init(int samplerate, gb_audio_fmt_t audio_fmt, gb_video_fmt_t video_fmt, gb_video_cb_t *video_callback, gb_audio_cb_t *audio_callback)
{
	GBDBG("01", "gnuboy_init enter sample_rate=%d audio_fmt=%d video_fmt=%d", samplerate, audio_fmt, video_fmt);
	GB = (gb_t){
		.video.colorize = GB_PALETTE_CGB,
		.video.format = video_fmt,
		.video.callback = video_callback,
		.audio.samplerate = samplerate,
		.audio.format = audio_fmt,
		.audio.callback = audio_callback,
	};
	GBDBG("02", "gb_hw_init begin");
	if (!gb_hw_init()) {
		GBDBG("03", "gb_hw_init FAILED internal_free=%u largest=%u",
			(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
			(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
		return -1;
	}
	GBDBG("04", "gb_hw_init complete");
	return 0;
}


/*
 * gnuboy_reset is called to initialize the state of the emulated
 * system. It should set cpu registers, hardware registers, etc. to
 * their appropriate values at power up time.
 */
void gnuboy_reset(bool hard)
{
	GBDBG("R1", "reset enter hard=%u", hard ? 1u : 0u);
	GBDBG("R2", "hardware reset begin");
	gb_hw_reset(hard);
	GBDBG("R3", "hardware reset complete; LCD reset begin");
	gb_lcd_reset(hard);
	GBDBG("R4", "LCD reset complete; CPU reset begin");
	gb_cpu_reset(hard);
	GBDBG("R5", "CPU reset complete; sound reset begin");
	gb_sound_reset(hard);
	gb_debug_run_count = 0;
	GBDBG("R6", "reset complete");
}


void gnuboy_set_framebuffer(void *buffer)
{
	GB.video.buffer = buffer;
}


void gnuboy_set_soundbuffer(void *buffer, size_t length)
{
	GB.audio.buffer = buffer;
	GB.audio.len = length;
}


/*
	Time intervals throughout the code, unless otherwise noted, are
	specified in double-speed machine cycles (2MHz), each unit
	roughly corresponds to 0.477us.

	For CPU each cycle takes 2dsc (0.954us) in single-speed mode
	and 1dsc (0.477us) in double speed mode.

	Although hardware gbc LCDC would operate at completely different
	and fixed frequency, for emulation purposes timings for it are
	also specified in double-speed cycles.

	line = 228 dsc (109us)
	frame (154 lines) = 35112 dsc (16.7ms)
	of which
		visible lines x144 = 32832 dsc (15.66ms)
		vblank lines x10 = 2280 dsc (1.08ms)
*/
void gnuboy_run(bool draw)
{
	const unsigned debug_run = ++gb_debug_run_count;
	const bool trace_run = debug_run <= 12;
	if (trace_run) {
		GBDBG("F0", "run=%u enter draw=%u pc=%04X sp=%04X ly=%u lcdc=%02X stat=%02X if=%02X ie=%02X cycles=%d bank=%d",
			debug_run, draw ? 1u : 0u,
			GB.cpu ? GB.cpu->pc.w : 0, GB.cpu ? GB.cpu->sp.w : 0,
			R_LY, R_LCDC, R_STAT, R_IF, R_IE, GB.cycles, cart.rombank);
	}
	GB.video.enabled = draw;
	GB.audio.pos = 0;

	int cycles = 0;

	// LCD is powered down, it won't touch LY or do vblank
	if (!(R_LCDC & 0x80)) {
		if (trace_run)
			GBDBG("F1", "run=%u LCD disabled; CPU advance begin pc=%04X", debug_run,
				GB.cpu ? GB.cpu->pc.w : 0);
		cycles += 154 * 228;
		cycles -= gb_cpu_emulate(cycles);
		if (trace_run)
			GBDBG("F4", "run=%u exit LCD disabled pc=%04X ly=%u lcdc=%02X stat=%02X cycles=%d",
				debug_run, GB.cpu ? GB.cpu->pc.w : 0, R_LY, R_LCDC, R_STAT, GB.cycles);
		return;
	}

	// We emulate until vblank (0..144)
	int next_ly_report = 0;
	while (R_LY <= 144) {
		if (trace_run && R_LY >= next_ly_report) {
			GBDBG("F1", "run=%u visible progress ly=%u pc=%04X stat=%02X cycles=%d",
				debug_run, R_LY, GB.cpu ? GB.cpu->pc.w : 0, R_STAT, GB.cycles);
			next_ly_report = ((int)R_LY / 24 + 1) * 24;
		}
		cycles += 228;
		cycles -= gb_cpu_emulate(cycles);
	}
	if (trace_run)
		GBDBG("F2", "run=%u visible complete ly=%u pc=%04X stat=%02X cycles=%d",
			debug_run, R_LY, GB.cpu ? GB.cpu->pc.w : 0, R_STAT, GB.cycles);

	/* When using GB_PIXEL_PALETTED, the host should draw the frame in this callback
	   because the palette can be modified below before gnuboy_run returns. */
	if (draw && GB.video.callback) {
		(GB.video.callback)(GB.video.buffer);
	}

	gb_hw_vblank();
	if (trace_run)
		GBDBG("F3", "run=%u vblank callback complete ly=%u pc=%04X audio_samples=%u",
			debug_run, R_LY, GB.cpu ? GB.cpu->pc.w : 0, (unsigned)GB.audio.pos);

	// Emulate vblank (145...0)
	while (R_LY > 0) {
		cycles += 228;
		cycles -= gb_cpu_emulate(cycles);
	}
	if (trace_run)
		GBDBG("F4", "run=%u exit pc=%04X ly=%u lcdc=%02X stat=%02X if=%02X ie=%02X cycles=%d bank=%d",
			debug_run, GB.cpu ? GB.cpu->pc.w : 0, R_LY, R_LCDC, R_STAT,
			R_IF, R_IE, GB.cycles, cart.rombank);

	if (GB.audio.callback && GB.audio.pos > 0) {
		(GB.audio.callback)(GB.audio.buffer, GB.audio.pos);
	}
}


void gnuboy_set_pad(int pad)
{
	if (hw.pad != pad)
	{
		gb_hw_setpad(pad);
	}
}


int gnuboy_load_bios(const byte *data, size_t size)
{
	if (size > 0x900)
	{
		MESSAGE_ERROR("Invalid BIOS size.\n");
		return -1;
	}

	if (hw.bios == NULL)
		hw.bios = malloc(0x900);

	if (!hw.bios)
	{
		MESSAGE_ERROR("Mem alloc failed.\n");
		return -2;
	}

	memcpy(hw.bios, data, size);

	return 0;
}


void gnuboy_free_bios(void)
{
	free(hw.bios);
	hw.bios = NULL;
}


int gnuboy_load_bios_file(const char *file)
{
	MESSAGE_INFO("Loading BIOS file: '%s'\n", file);
	byte buffer[0x900];

	FILE *fp = fopen(file, "rb");
	if (!fp || fread(buffer, 1, 0x900, fp) < 0x100)
	{
		MESSAGE_ERROR("File read failed.\n");
		fclose(fp);
		return -1;
	}
	fclose(fp);

	return gnuboy_load_bios(buffer, sizeof(buffer));
}


void gnuboy_load_bank(int bank)
{
	const size_t OFFSET = bank * BANK_SIZE;
	if (bank < 0 || bank >= cart.romsize) {
		GBDBG("B!", "invalid bank=%d rom_banks=%d", bank, cart.romsize);
		return;
	}
	GBDBG("B1", "bank=%d/%d begin offset=%u ptr=%p psram_free=%u largest=%u",
		bank, cart.romsize - 1, (unsigned)OFFSET, (void *)cart.rombanks[bank],
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
		(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

	if (!cart.rombanks[bank])
		cart.rombanks[bank] = GB_PSRAM_MALLOC(BANK_SIZE);  // CrowPanel patch: PSRAM
	GBDBG("B2", "bank=%d allocation ptr=%p", bank, (void *)cart.rombanks[bank]);

	if (!cart.romFile) {
		GBDBG("B!", "bank=%d stopped: ROM FILE pointer is null", bank);
		return;
	}

	// Cube Boy: during the boot-time preload, stop at the first failed alloc
	// instead of thrashing every ROM bank through the tiny reclaim cache -
	// missing banks load on demand at their first MBC select.
	if (!cart.rombanks[bank] && gb_preloading) {
		GBDBG("B3", "bank=%d preload allocation unavailable", bank);
		return;
	}

	// NOTE: no MESSAGE_INFO here - on a thrashing cart this path runs many
	// times per frame and the serial printf becomes its own bottleneck.
	int tries = 0;
	while (!cart.rombanks[bank])
	{
		// Cube Boy fix: bound the index by the real bank count. The table
		// only has cart.romsize entries - the original rand()&0xFF read past
		// it and stole a garbage pointer (hardware-verified StoreProhibited
		// loading a 1 MB cart on the no-PSRAM ADV; the P4 never ran this path
		// because everything fit in PSRAM). Prefer not to steal bank 0 or the
		// currently mapped bank, which the CPU may be executing from; after
		// many tries widen rather than spin forever.
		int i = rand() % cart.romsize;
		if (i == bank) continue;
		if (tries++ < 512 && (i == 0 || i == gb_live_bank)) continue;
		if (cart.rombanks[i])
		{
			cart.rombanks[bank] = cart.rombanks[i];
			cart.rombanks[i] = NULL;
			break;
		}
	}

	// Load the 16K page
	GBDBG("B4", "bank=%d fseek begin", bank);
	errno = 0;
	int seek_result = fseek(cart.romFile, OFFSET, SEEK_SET);
	GBDBG("B5", "bank=%d fseek returned=%d errno=%d", bank, seek_result, errno);
	size_t read_result = 0;
	if (seek_result == 0) {
		GBDBG("B6", "bank=%d fread begin bytes=%u", bank, (unsigned)BANK_SIZE);
		errno = 0;
		read_result = fread(cart.rombanks[bank], BANK_SIZE, 1, cart.romFile);
		GBDBG("B7", "bank=%d fread returned=%u errno=%d feof=%d ferror=%d", bank,
			(unsigned)read_result, errno, feof(cart.romFile), ferror(cart.romFile));
	}
	if (seek_result != 0 || read_result != 1)
	{
		MESSAGE_WARN("ROM bank loading failed\n");
		if (!feof(cart.romFile))
			abort(); // This indicates an SD Card failure
	}
	if (!gb_preloading)
		gb_live_bank = bank;  // the MBC just mapped this bank - never steal it
	GBDBG("B8", "bank=%d complete ptr=%p", bank, (void *)cart.rombanks[bank]);
}


int gnuboy_load_rom(const byte *data, size_t size)
{
	GBDBG("10", "header setup enter data=%p bytes=%u", (const void *)data, (unsigned)size);
	// Memory Bank Controller names
	const char *mbc_names[16] = {
		"MBC_NONE", "MBC_MBC1", "MBC_MBC2", "MBC_MBC3",
		"MBC_MBC5", "MBC_MBC6", "MBC_MBC7", "MBC_HUC1",
		"MBC_HUC3", "MBC_MMM01", "INVALID", "INVALID",
		"INVALID", "INVALID", "INVALID", "INVALID",
	};
	const char *hw_types[] = {
		"DMG", "CGB", "SGB", "AGB", "???"
	};

	// We need at least the header
	if (size < 0x200) {
		GBDBG("11", "header rejected: need 512 bytes, got %u", (unsigned)size);
		return -1;
	}

	const byte *header = data;
	int type = header[0x0147];
	int romsize = header[0x0148];
	int ramsize = header[0x0149];
	GBDBG("12", "header bytes type=0x%02X rom_code=0x%02X ram_code=0x%02X cgb=0x%02X sgb=0x%02X",
		type, romsize, ramsize, header[0x0143], header[0x0146]);

	if (header[0x0143] == 0x80 || header[0x0143] == 0xC0)
		hw.hwtype = GB_HW_CGB; // Game supports CGB mode so we go for that
	else if (header[0x0146] == 0x03)
		hw.hwtype = GB_HW_SGB; // Game supports SGB features
	else
		hw.hwtype = GB_HW_DMG; // Games supports DMG only

	memcpy(&cart.checksum, header + 0x014E, 2);
	memcpy(&cart.name, header + 0x0134, 16);
	cart.name[16] = 0;
	GBDBG("13", "title parsed name='%s' checksum=%02X%02X", cart.name,
		header[0x014E], header[0x014F]);

	cart.has_battery = (type == 3 || type == 6 || type == 9 || type == 13 || type == 15 ||
						type == 16 || type == 19 || type == 27 || type == 30 || type == 255);
	cart.has_rtc  = (type == 15 || type == 16);
	cart.has_rumble = (type == 28 || type == 29 || type == 30);
	cart.has_sensor = (type == 34);
	cart.colorize = 0;

	if (type >= 1 && type <= 3)
		cart.mbc = MBC_MBC1;
	else if (type >= 5 && type <= 6)
		cart.mbc = MBC_MBC2;
	else if (type >= 11 && type <= 13)
		cart.mbc = MBC_MMM01;
	else if (type >= 15 && type <= 19)
		cart.mbc = MBC_MBC3;
	else if (type >= 25 && type <= 30)
		cart.mbc = MBC_MBC5;
	else if (type == 32)
		cart.mbc = MBC_MBC6;
	else if (type == 34)
		cart.mbc = MBC_MBC7;
	else if (type == 254)
		cart.mbc = MBC_HUC3;
	else if (type == 255)
		cart.mbc = MBC_HUC1;
	else
		cart.mbc = MBC_NONE;
	GBDBG("14", "mode parsed hw=%d mbc=%d battery=%u rtc=%u rumble=%u sensor=%u",
		hw.hwtype, cart.mbc, cart.has_battery, cart.has_rtc,
		cart.has_rumble, cart.has_sensor);

	if (romsize < 9)
	{
		cart.romsize = (2 << romsize);
	}
	else if (romsize > 0x51 && romsize < 0x55)
	{
		cart.romsize = 128; // (2 << romsize) + 64;
	}
	else
	{
		MESSAGE_ERROR("Invalid ROM size: %d\n", romsize);
		return -2;
	}

	if (ramsize < 6)
	{
		const byte ramsize_table[] = {1, 1, 1, 4, 16, 8};
		cart.ramsize = ramsize_table[ramsize];
	}
	else
	{
		MESSAGE_ERROR("Invalid RAM size: %d\n", ramsize);
		cart.ramsize = 1;
	}
	GBDBG("15", "size parsed rom_banks=%d (%d KiB) ram_banks=%d (%d KiB)",
		cart.romsize, cart.romsize * 16, cart.ramsize, cart.ramsize * 8);

	GBDBG("16", "cart RAM allocation begin bytes=%u psram_free=%u largest=%u",
		(unsigned)(cart.ramsize * 0x2000),
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
		(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
	cart.rambanks = GB_PSRAM_CALLOC(cart.ramsize, 0x2000);  // CrowPanel patch: PSRAM
	GBDBG("17", "cart RAM allocation ptr=%p", (void *)cart.rambanks);
	GBDBG("18", "ROM pointer table allocation begin entries=%d bytes=%u internal_free=%u largest=%u",
		cart.romsize, (unsigned)(cart.romsize * sizeof(byte *)),
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
		(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
	cart.rombanks = calloc(cart.romsize, sizeof(byte *));   // small hot table: internal
	GBDBG("19", "ROM pointer table allocation ptr=%p", (void *)cart.rombanks);

	if (!cart.rambanks || !cart.rombanks)
	{
		MESSAGE_ERROR("Memory allocation failed.");
		return -3;
	}
	GBDBG("20", "cart allocations complete");

	for (size_t pos = 0; size - pos >= BANK_SIZE; pos += BANK_SIZE)
	{
		// FIXME: We need a way to tag this as non-freeable...
		cart.rombanks[pos / BANK_SIZE] = (byte *)(data + pos);
	}

	// Detect colorization palette that the real GBC would be using
	if (!IS_CGB)
	{
		//
		// The following algorithm was adapted from visualboyadvance-m at
		// https://github.com/visualboyadvance-m/visualboyadvance-m/blob/master/src/gb/GB.cpp
		//

		// Title checksums that are treated specially by the CGB boot ROM
		const uint8_t col_checksum[79] = {
			0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C, 0x92, 0x3D, 0x5C,
			0x58, 0xC9, 0x3E, 0x70, 0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA,
			0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97, 0x4B, 0x90, 0x17, 0x10,
			0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E, 0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
			0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD,
			0x5D, 0x6D, 0x67, 0x3F, 0x6B, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27,
			0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4
		};

		// The fourth character of the game title for disambiguation on collision.
		const uint8_t col_disambig_chars[29] = {
			'B', 'E', 'F', 'A', 'A', 'R', 'B', 'E',
			'K', 'E', 'K', ' ', 'R', '-', 'U', 'R',
			'A', 'R', ' ', 'I', 'N', 'A', 'I', 'L',
			'I', 'C', 'E', ' ', 'R'
		};

		// Palette ID | (Flags << 5)
		const uint8_t col_palette_info[94] = {
			0x7C, 0x08, 0x12, 0xA3, 0xA2, 0x07, 0x87, 0x4B, 0x20, 0x12, 0x65, 0xA8,
			0x16, 0xA9, 0x86, 0xB1, 0x68, 0xA0, 0x87, 0x66, 0x12, 0xA1, 0x30, 0x3C,
			0x12, 0x85, 0x12, 0x64, 0x1B, 0x07, 0x06, 0x6F, 0x6E, 0x6E, 0xAE, 0xAF,
			0x6F, 0xB2, 0xAF, 0xB2, 0xA8, 0xAB, 0x6F, 0xAF, 0x86, 0xAE, 0xA2, 0xA2,
			0x12, 0xAF, 0x13, 0x12, 0xA1, 0x6E, 0xAF, 0xAF, 0xAD, 0x06, 0x4C, 0x6E,
			0xAF, 0xAF, 0x12, 0x7C, 0xAC, 0xA8, 0x6A, 0x6E, 0x13, 0xA0, 0x2D, 0xA8,
			0x2B, 0xAC, 0x64, 0xAC, 0x6D, 0x87, 0xBC, 0x60, 0xB4, 0x13, 0x72, 0x7C,
			0xB5, 0xAE, 0xAE, 0x7C, 0x7C, 0x65, 0xA2, 0x6C, 0x64, 0x85
		};

		uint8_t infoIdx = 0;
		uint8_t checksum = 0;

		// Calculate the checksum over 16 title bytes.
		for (int i = 0; i < 16; i++)
		{
			checksum += header[0x0134 + i];
		}

		// Check if the checksum is in the list.
		for (size_t idx = 0; idx < 79; idx++)
		{
			if (col_checksum[idx] == checksum)
			{
				infoIdx = idx;

				// Indexes above 0x40 have to be disambiguated.
				if (idx <= 0x40)
					break;

				// No idea how that works. But it works.
				for (size_t i = idx - 0x41, j = 0; i < 29; i += 14, j += 14) {
					if (header[0x0137] == col_disambig_chars[i]) {
						infoIdx += j;
						break;
					}
				}
				break;
			}
		}

		cart.colorize = col_palette_info[infoIdx];
	}
	GBDBG("21", "palette detection complete colorize=%d", cart.colorize);

	MESSAGE_INFO("Cart loaded: name='%s', hw=%s, mbc=%s, romsize=%dK, ramsize=%dK, colorize=%d\n",
		cart.name, hw_types[hw.hwtype], mbc_names[cart.mbc], cart.romsize * 16, cart.ramsize * 8, cart.colorize);

	// Apply game-specific hacks
	if (memcmp(cart.name, "SIREN GB2 ", 10) == 0 || memcmp(cart.name, "DONKEY KONG", 12) == 0)
	{
		MESSAGE_INFO("HACK: Window offset hack enabled (12)\n");
		hw.compat.window_offset = 12;
	}
	else if (memcmp(cart.name, "RES EVIL GD", 11) == 0 || memcmp(cart.name, "BIOHAZARDGDB", 12) == 0)
	{
		MESSAGE_INFO("HACK: Window offset hack enabled (10)\n");
		hw.compat.window_offset = 10;
	}
	else
	{
		hw.compat.window_offset = 0;
	}

	GBDBG("22", "header setup complete");
	return 0;
}


int gnuboy_load_rom_file(const char *file)
{
	GBDBG("30", "ROM file load enter path='%s'", file ? file : "(null)");

	byte header[0x200];

	errno = 0;
	GBDBG("31", "fopen begin");
	cart.romFile = fopen(file, "rb");
	GBDBG("32", "fopen returned fp=%p errno=%d", (void *)cart.romFile, errno);
	if (cart.romFile == NULL)
	{
		MESSAGE_ERROR("ROM fopen failed\n");
		return -1;
	}

	errno = 0;
	GBDBG("33", "file size probe begin");
	int size_seek = fseek(cart.romFile, 0, SEEK_END);
	long file_size = size_seek == 0 ? ftell(cart.romFile) : -1;
	int rewind_result = fseek(cart.romFile, 0, SEEK_SET);
	GBDBG("34", "file size probe size=%ld seek=%d rewind=%d errno=%d", file_size,
		size_seek, rewind_result, errno);
	GBDBG("35", "header fread begin bytes=%u", (unsigned)sizeof(header));
	errno = 0;
	size_t header_read = fread(&header, sizeof(header), 1, cart.romFile);
	GBDBG("36", "header fread returned=%u errno=%d feof=%d ferror=%d",
		(unsigned)header_read, errno, feof(cart.romFile), ferror(cart.romFile));
	if (header_read != 1)
	{
		MESSAGE_ERROR("ROM fread failed\n");
		fclose(cart.romFile);
		cart.romFile = NULL;
		return -1;
	}

	GBDBG("37", "header setup call begin");
	int ret = gnuboy_load_rom(header, 0x200);
	GBDBG("38", "header setup returned=%d", ret);
	if (ret != 0)
	{
		MESSAGE_ERROR("ROM setup failed\n");
		return ret;
	}

	// Gameboy color games can be very large so we preload a maximum of 128 banks for faster boot
	// Also 4/8MB games do not fully fit anyway, we need to leave room for our bank manager's swapping.

	int preload = cart.romsize < 128 ? cart.romsize : 128;

	if (cart.romsize > 64 && (strncmp(cart.name, "RAYMAN", 6) == 0 || strncmp(cart.name, "NONAME", 6) == 0))
	{
		MESSAGE_INFO("Special preloading for Rayman 1/2\n");
		preload = cart.romsize - 40;
	}

	GBDBG("39", "preload plan banks=%d of %d", preload, cart.romsize);
	gb_preloading = 1;
	gb_live_bank = 1;
	for (int i = 0; i < preload; i++)
	{
		GBDBG("40", "preload dispatch bank=%d", i);
		gnuboy_load_bank(i);
		if (!cart.rombanks[i])
		{
			// No PSRAM and internal RAM is full: the rest of the cart pages
			// in on demand at its first MBC select.
			MESSAGE_INFO("Preload stopped at bank %d (RAM full); paging from SD\n", i);
			break;
		}
		// Cube Boy patch: yield EVERY bank so CPU 1's IDLE task runs and feeds the
		// task watchdog. SD reads here are slow (~0.6s/bank on big carts), so a
		// coarser interval lets an 8-bank chunk (~5s) reach the ~5s WDT limit and
		// abort mid-load; per-bank keeps each gap well under it. Cost is ~1ms/bank.
		vTaskDelay(1);
	}
	gb_preloading = 0;
	GBDBG("41", "preload complete psram_free=%u largest=%u internal_free=%u largest=%u",
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
		(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
		(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
	if (!gb_debug_verify_preloaded_rom(file_size))
	{
		MESSAGE_ERROR("ROM integrity check failed; refusing to execute corrupt cartridge data\n");
		return -5;
	}

	GBDBG("42", "ROM file load complete");
	return 0;
}


void gnuboy_free_rom(void)
{
	// If cart.romFile isn't NULL it indicates that we haven't allocated those buffers, don't free them.
	if (cart.romFile && cart.rombanks)
	{
		for (int i = 0; i < cart.romsize; i++)
			free(cart.rombanks[i]);
	}

	free(cart.rombanks);
	cart.rombanks = NULL;

	free(cart.rambanks);
	cart.rambanks = NULL;

	if (cart.romFile)
	{
		fclose(cart.romFile);
		cart.romFile = NULL;
	}

	if (cart.sramFile)
	{
		fclose(cart.sramFile);
		cart.sramFile = NULL;
	}

	memset(&cart, 0, sizeof(cart));
}


void gnuboy_get_time(int *day, int *hour, int *minute, int *second)
{
	if (day) *day = cart.rtc.d;
	if (hour) *hour = cart.rtc.h;
	if (minute) *minute = cart.rtc.m;
	if (second) *second = cart.rtc.s;
}


void gnuboy_set_time(int day, int hour, int minute, int second)
{
	cart.rtc.d = day % 365;
	cart.rtc.h = hour % 24;
	cart.rtc.m = minute % 60;
	cart.rtc.s = second % 60;
	cart.rtc.ticks = 0;
	cart.rtc.dirty = 0;
}


int gnuboy_get_hwtype(void)
{
	return hw.hwtype;
}


void gnuboy_set_hwtype(gb_hwtype_t type)
{
	// nothing for now
}


int gnuboy_get_palette(void)
{
	return GB.video.colorize;
}


void gnuboy_set_palette(gb_palette_t pal)
{
	GB.video.colorize = pal;
	gb_lcd_pal_dirty();
}


bool gnuboy_sram_dirty(void)
{
	return cart.sram_dirty != 0;
}


int gnuboy_load_sram(const char *file)
{
	GBDBG("S1", "SRAM load enter path='%s' battery=%u ram_banks=%d",
		file ? file : "(null)", cart.has_battery, cart.ramsize);
	if (!cart.has_battery || !cart.ramsize || !file || !*file) {
		GBDBG("S2", "SRAM load skipped: cartridge/path not applicable");
		return -1;
	}

	errno = 0;
	FILE *f = fopen(file, "rb");
	GBDBG("S3", "SRAM fopen returned fp=%p errno=%d", (void *)f, errno);
	if (!f)
		return -2;

	MESSAGE_INFO("Loading SRAM from '%s'\n", file);

	cart.sram_dirty = 0;
	cart.sram_saved = 0;

	for (int i = 0; i < cart.ramsize; i++)
	{
		GBDBG("S4", "SRAM bank=%d read begin", i);
		int sram_seek = fseek(f, i * 8192, SEEK_SET);
		size_t sram_read = sram_seek == 0 ? fread(cart.rambanks[i], 8192, 1, f) : 0;
		GBDBG("S5", "SRAM bank=%d seek=%d read=%u feof=%d ferror=%d", i,
			sram_seek, (unsigned)sram_read, feof(f), ferror(f));
		if (sram_seek == 0 && sram_read == 1)
		{
			MESSAGE_INFO("Loaded SRAM bank %d.\n", i);
			cart.sram_saved = (1 << i);
		}
	}

	if (cart.has_rtc)
	{
		uint32_t rtc_buf[12];

		if (fseek(f, cart.ramsize * 8192, SEEK_SET) == 0 && fread(&rtc_buf, 48, 1, f) == 1)
		{
			cart.rtc = (gb_rtc_t){
				.s = rtc_buf[0],
				.m = rtc_buf[1],
				.h = rtc_buf[2],
				.d = rtc_buf[3],
				.flags = rtc_buf[4],
				.regs = {rtc_buf[5], rtc_buf[6], rtc_buf[7], rtc_buf[8], rtc_buf[9]},
			};
			MESSAGE_INFO("Loaded RTC section %03d %02d:%02d:%02d.\n", cart.rtc.d, cart.rtc.h, cart.rtc.m, cart.rtc.s);
		}
	}

	fclose(f);

	GBDBG("S6", "SRAM load complete saved_mask=0x%X", cart.sram_saved);
	return cart.sram_saved ? 0 : -1;
}


/**
 * If quick_save is set to true, sram_save will only save the sectors that
 * changed + the rtc. If set to false then a full sram file is created.
 */
int gnuboy_save_sram(const char *file, bool quick_save)
{
	if (!cart.has_battery || !cart.ramsize || !file || !*file)
		return -1;

	FILE *f = fopen(file, "wb");
	if (!f)
		return -2;

	MESSAGE_INFO("Saving SRAM to '%s'...\n", file);

	// Mark everything as dirty and unsaved (do a full save)
	if (!quick_save)
	{
		cart.sram_dirty = (1 << cart.ramsize) - 1;
		cart.sram_saved = 0;
	}

	for (int i = 0; i < cart.ramsize; i++)
	{
		if (!(cart.sram_saved & (1 << i)) || (cart.sram_dirty & (1 << i)))
		{
			if (fseek(f, i * 8192, SEEK_SET) == 0 && fwrite(cart.rambanks[i], 8192, 1, f) == 1)
			{
				MESSAGE_INFO("Saved SRAM bank %d.\n", i);
				cart.sram_dirty &= ~(1 << i);
				cart.sram_saved |= (1 << i);
			}
		}
	}

	if (cart.has_rtc)
	{
		uint64_t rt = RTC_BASE + cart.rtc.s + (cart.rtc.m * 60) + (cart.rtc.h * 3600) + (cart.rtc.d * 86400);
		uint32_t *rtp = (uint32_t*)&rt;
		uint32_t rtc_buf[12] = {
			cart.rtc.s,
			cart.rtc.m,
			cart.rtc.h,
			cart.rtc.d,
			cart.rtc.flags,
			cart.rtc.regs[0],
			cart.rtc.regs[1],
			cart.rtc.regs[2],
			cart.rtc.regs[3],
			cart.rtc.regs[4],
			rtp[0],
			rtp[1],
		};
		if (fseek(f, cart.ramsize * 8192, SEEK_SET) == 0 && fwrite(&rtc_buf, 48, 1, f) == 1)
		{
			MESSAGE_INFO("Saved RTC section.\n");
		}
	}

	fclose(f);

	return cart.sram_dirty ? -1 : 0;
}



/**
 * Save state file format is:
 * GB:
 * 0x0000 - 0x0BFF: svars
 * 0x0CF0 - 0x0CFF: hw.snd->wave
 * 0x0D00 - 0x0DFF: hw.ioregs
 * 0x0E00 - 0x0E80: lcd.pal
 * 0x0F00 - 0x0FFF: lcd.oam
 * 0x1000 - 0x2FFF: RAM
 * 0x3000 - 0x4FFF: VRAM
 * 0x5000 - 0x...:  SRAM
 *
 * GBC:
 * 0x0000 - 0x0BFF: svars
 * 0x0CF0 - 0x0CFF: hw.snd->wave
 * 0x0D00 - 0x0DFF: hw.ioregs
 * 0x0E00 - 0x0EFF: lcd.pal
 * 0x0F00 - 0x0FFF: lcd.oam
 * 0x1000 - 0x8FFF: RAM
 * 0x9000 - 0xCFFF: VRAM
 * 0xD000 - 0x...:  SRAM
 *
 */

#ifndef IS_BIG_ENDIAN
#define LIL(x) (x)
#else
#define LIL(x) ((x<<24)|((x&0xff00)<<8)|((x>>8)&0xff00)|(x>>24))
#endif

#define SAVE_VERSION 0x107

#define I1(s, p) { 1, s, p }
#define I2(s, p) { 2, s, p }
#define I4(s, p) { 4, s, p }
#define END { 0, "\0\0\0\0", 0 }

typedef struct
{
	size_t len;
	char key[4];
	void *ptr;
} svar_t;

typedef struct
{
	void *ptr;
	size_t len;
} sblock_t;


static int do_save_load(const char *file, bool save)
{
	uint32_t sav_ver = SAVE_VERSION;
	const svar_t svars[] =
	{
		I4("GbSs", &sav_ver),

		I2("PC  ", &W(hw.cpu->pc)),
		I2("SP  ", &W(hw.cpu->sp)),
		I2("BC  ", &W(hw.cpu->bc)),
		I2("DE  ", &W(hw.cpu->de)),
		I2("HL  ", &W(hw.cpu->hl)),
		I2("AF  ", &W(hw.cpu->af)),

		I4("IME ", &hw.cpu->ime),
		I4("ima ", &hw.cpu->ima),
		I4("spd ", &hw.cpu->double_speed),
		I4("halt", &hw.cpu->halted),
		I4("div ", &hw.cpu->div),
		I4("tim ", &hw.cpu->timer),
		I4("lcdc", &hw.cycles),
		I4("snd ", &hw.snd->cycles),

		I4("ints", &hw.ilines),
		I4("pad ", &hw.pad),
		I4("hdma", &hw.hdma),
		I4("seri", &hw.serial),

		I4("mbcm", &hw.cart->bankmode),
		I4("romb", &hw.cart->rombank),
		I4("ramb", &hw.cart->rambank),
		I4("enab", &hw.cart->enableram),

		// We should pack that below. Size of components could vary per platform
		I4("rtcR", &hw.cart->rtc.sel),
		I4("rtcL", &hw.cart->rtc.latch),
		I4("rtcF", &hw.cart->rtc.flags),
		I4("rtcd", &hw.cart->rtc.d),
		I4("rtch", &hw.cart->rtc.h),
		I4("rtcm", &hw.cart->rtc.m),
		I4("rtcs", &hw.cart->rtc.s),
		I4("rtct", &hw.cart->rtc.ticks),
		I1("rtR8", &hw.cart->rtc.regs[0]),
		I1("rtR9", &hw.cart->rtc.regs[1]),
		I1("rtRA", &hw.cart->rtc.regs[2]),
		I1("rtRB", &hw.cart->rtc.regs[3]),
		I1("rtRC", &hw.cart->rtc.regs[4]),

		I4("S1on", &hw.snd->ch[0].on),
		I4("S1p ", &hw.snd->ch[0].pos),
		I4("S1c ", &hw.snd->ch[0].cnt),
		I4("S1ec", &hw.snd->ch[0].encnt),
		I4("S1sc", &hw.snd->ch[0].swcnt),
		I4("S1sf", &hw.snd->ch[0].swfreq),

		I4("S2on", &hw.snd->ch[1].on),
		I4("S2p ", &hw.snd->ch[1].pos),
		I4("S2c ", &hw.snd->ch[1].cnt),
		I4("S2ec", &hw.snd->ch[1].encnt),

		I4("S3on", &hw.snd->ch[2].on),
		I4("S3p ", &hw.snd->ch[2].pos),
		I4("S3c ", &hw.snd->ch[2].cnt),

		I4("S4on", &hw.snd->ch[3].on),
		I4("S4p ", &hw.snd->ch[3].pos),
		I4("S4c ", &hw.snd->ch[3].cnt),
		I4("S4ec", &hw.snd->ch[3].encnt),

		END
	};

	byte *buf = calloc(1, 4096);
	if (!buf) return -2;

	uint32_t (*header)[2] = (uint32_t (*)[2])buf;

	sblock_t blocks[] = {
		{buf, 1},
		{hw.rambanks, IS_CGB ? 8 : 2},
		{hw.vbanks, IS_CGB ? 4 : 2},
		{cart.rambanks, cart.ramsize * 2},
		{NULL, 0},
	};

	FILE *fp = NULL;

	if (save)
	{
		if (!(fp = fopen(file, "wb")))
			goto _error;

		for (int i = 0; svars[i].ptr; i++)
		{
			uint32_t d = 0;

			switch (svars[i].len)
			{
			case 1:
				d = *(uint8_t *)svars[i].ptr;
				break;
			case 2:
				d = *(uint16_t *)svars[i].ptr;
				break;
			case 4:
				d = *(uint32_t *)svars[i].ptr;
				break;
			}

			header[i][0] = *(uint32_t *)svars[i].key;
			header[i][1] = LIL(d);
		}

		memcpy(buf + 0xD00, hw.ioregs, 256);
		memcpy(buf + 0xE00, hw.pal, 128);
		memcpy(buf + 0xF00, hw.oam, 256);
		memcpy(buf + 0xCF0, hw.snd->wave, 16);

		for (int i = 0; blocks[i].ptr != NULL; i++)
		{
			if (fwrite(blocks[i].ptr, 4096, blocks[i].len, fp) < 1)
			{
				MESSAGE_ERROR("Write error in block %d\n", i);
				goto _error;
			}
		}
	}
	else
	{
		if (!(fp = fopen(file, "rb")))
			goto _error;

		for (int i = 0; blocks[i].ptr != NULL; i++)
		{
			if (fread(blocks[i].ptr, 4096, blocks[i].len, fp) < 1)
			{
				MESSAGE_ERROR("Read error in block %d\n", i);
				goto _error;
			}
		}

		for (int i = 0; svars[i].ptr; i++)
		{
			uint32_t d = 0;

			for (int j = 0; header[j][0]; j++)
			{
				if (header[j][0] == *(uint32_t *)svars[i].key)
				{
					d = LIL(header[j][1]);
					break;
				}
			}

			switch (svars[i].len)
			{
			case 1:
				*(uint8_t *)svars[i].ptr = d;
				break;
			case 2:
				*(uint16_t *)svars[i].ptr = d;
				break;
			case 4:
				*(uint32_t *)svars[i].ptr = d;
				break;
			}
		}

		if (sav_ver != SAVE_VERSION)
			MESSAGE_ERROR("Save file version mismatch!\n");

		memcpy(hw.ioregs, buf + 0xD00, 256);
		memcpy(hw.pal, buf + 0xE00, 128);
		memcpy(hw.oam, buf + 0xF00, 256);
		memcpy(hw.snd->wave, buf + 0xCF0, 16);

		// Disable BIOS. This is a hack to support old saves
		R_BIOS = 0x1;

		// Older saves might overflow this
		cart.rambank &= (cart.ramsize - 1);

		gb_lcd_pal_dirty();
		gb_sound_dirty();
		gb_hw_updatemap();
	}

	fclose(fp);
	free(buf);

	return 0;

_error:
	if (fp) fclose(fp);
	if (buf) free(buf);

	return -1;
}


int gnuboy_save_state(const char *file)
{
	return do_save_load(file, true);
}


int gnuboy_load_state(const char *file)
{
	return do_save_load(file, false);
}
