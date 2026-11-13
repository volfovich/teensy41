/* imxrt1062_t41_cold.ld - the Teensy 4.1 core's imxrt1062_t41.ld (Teensyduino core as
 * installed by PlatformIO when this file was made), plus rules that keep cold code and its
 * constant data in flash instead of RAM1. Same idea as teensy-ir-loader's
 * tools/imxrt1062_cold.ld.
 *
 * The core's script copies every function without a section attribute into ITCM, the fast
 * RAM1 that the stack and the audio state share, and every constant into DTCM. Most of that
 * is library code that never runs in the audio interrupt: newlib's printf/strtod/malloc,
 * SdFat, EEPROM. The COLD_* rules below place it, and this firmware's background-only
 * files, in flash, where it runs through the 32 KB instruction cache.
 *
 * Rules: an input section goes to the first output section whose pattern matches, so these
 * come before .text.itcm's catch-all. For the files in src/ only plain functions are taken
 * (sections .text._Z<digit>... and .text._ZL...: free and static functions). Class and
 * namespace members (.text._ZN...) are left for ITCM, because those are mostly the dsp
 * library's inline and template functions, which the audio interrupt runs: the compiler
 * emits a copy in every file that uses one and the linker keeps a single copy, which must
 * not be one that a cold rule sends to flash. Never list a file here that has code the audio
 * interrupt runs (main.cpp's callback, AudioChain, the effects, DrumMachine). After
 * changing this list, run tools/check_hot_code.sh: it fails if an audio-path function is
 * not in ITCM.
 * libc's mem* stay in ITCM (memset and memcpy run in the interrupt), and so do its str*:
 * their unwind entries in .ARM.exidx (ITCM) can't reach flash. */

MEMORY
{
	ITCM (rx):  ORIGIN = 0x00000000, LENGTH = 512K
	DTCM (rw):  ORIGIN = 0x20000000, LENGTH = 512K
	RAM (rw):   ORIGIN = 0x20200000, LENGTH = 512K
	FLASH (rx): ORIGIN = 0x60000000, LENGTH = 7936K
	ERAM (rw):  ORIGIN = 0x70000000, LENGTH = 32768K
}

ENTRY(ImageVectorTable)

SECTIONS
{
	.text.headers : {
		KEEP(*(.flashconfig))
		FILL(0xFF)
		. = ORIGIN(FLASH) + 0x1000;
		KEEP(*(.ivt))
		KEEP(*(.bootdata))
		. = ALIGN(1024);
	} > FLASH

	.text.code : {
		KEEP(*(.startup))
		*(.flashmem*)
		/* COLD_TEXT */
		*libc.a:*(EXCLUDE_FILE(*libc_a-mem*.o *libc_a-str*.o) .text*)
		*libSdFat.a:*(.text*)
		*libSD.a:*(.text*)
		*libEEPROM.a:*(.text*)
		*libparams.a:*(.text*)
		*libdsp.a:IRLoader.cpp.o(.text*)
		*libdsp.a:MidiDrums.cpp.o(.text*)
		*libdsp.a:NamFile.cpp.o(.text*)
		*src/SerialUI.cpp.o(.text._Z[0-9]* .text._ZL*)
		*src/Settings.cpp.o(.text._Z[0-9]* .text._ZL*)
		*src/IRLibrary.cpp.o(.text._Z[0-9]* .text._ZL*)
		*src/DrumSongs.cpp.o(.text._Z[0-9]* .text._ZL*)
		*src/Params.cpp.o(.text._Z[0-9]* .text._ZL*)
		*src/AmpLibrary.cpp.o(.text._Z[0-9]* .text._ZL*)
		*src/PresetLibrary.cpp.o(.text._Z[0-9]* .text._ZL*)
		*src/Tuner.cpp.o(.text._Z[0-9]* .text._ZL*)
		*src/MidiControl.cpp.o(.text*)
		. = ALIGN(4);
		KEEP(*(.init))
		__preinit_array_start = .;
		KEEP (*(.preinit_array))
		__preinit_array_end = .;
		__init_array_start = .;
		KEEP (*(.init_array))
		__init_array_end = .;
		. = ALIGN(4);
	} > FLASH

	.text.progmem : {
		*(.progmem*)
		/* COLD_RODATA: the same files' constants and string literals */
		*libc.a:*(.rodata*)
		*libSdFat.a:*(.rodata*)
		*libSD.a:*(.rodata*)
		*libparams.a:*(.rodata*)
		*libdsp.a:IRLoader.cpp.o(.rodata*)
		*libdsp.a:MidiDrums.cpp.o(.rodata*)
		*libdsp.a:NamFile.cpp.o(.rodata*)
		*src/SerialUI.cpp.o(.rodata*)
		*src/Settings.cpp.o(.rodata*)
		*src/IRLibrary.cpp.o(.rodata*)
		*src/DrumSongs.cpp.o(.rodata*)
		*src/Params.cpp.o(.rodata*)
		*src/AmpLibrary.cpp.o(.rodata*)
		*src/PresetLibrary.cpp.o(.rodata*)
		*src/Tuner.cpp.o(.rodata*)
		*src/MidiControl.cpp.o(.rodata*)
		. = ALIGN(4);
	} > FLASH

	.text.itcm : {
		. = . + 32; /* MPU to trap NULL pointer deref */
		*(.fastrun)
		*(.text*)
		. = ALIGN(16);
	} > ITCM  AT> FLASH

	.ARM.exidx : {
		__exidx_start = .;
		*(.ARM.exidx* .ARM.extab.text* .gnu.linkonce.armexidx.*)
		__exidx_end = .;
	} > ITCM  AT> FLASH

	.data : {
		*(.endpoint_queue)   
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.rodata*)))
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.data*)))
		KEEP(*(.vectorsram))
	} > DTCM  AT> FLASH

	.bss ALIGN(4) : {
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.bss*)))
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(COMMON)))
		. = ALIGN(32);
		. = . + 32; /* MPU to trap stack overflow */
	} > DTCM

	.bss.dma (NOLOAD) : {
		*(.hab_log)
		*(.dmabuffers)
		. = ALIGN(32);
	} > RAM

	.bss.extram (NOLOAD) : {
		*(SORT_BY_ALIGNMENT(SORT_BY_NAME(.externalram)))
		. = ALIGN(32);
	} > ERAM

	.text.csf : {
		FILL(0xFF)
		. = ALIGN(1024);
		KEEP(*(.csf))
		__text_csf_end = .;
	} > FLASH

	_stext = ADDR(.text.itcm);
	_etext = ADDR(.text.itcm) + SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx);
	_stextload = LOADADDR(.text.itcm);

	_sdata = ADDR(.data);
	_edata = ADDR(.data) + SIZEOF(.data);
	_sdataload = LOADADDR(.data);

	_sbss = ADDR(.bss);
	_ebss = ADDR(.bss) + SIZEOF(.bss);

	_heap_start = ADDR(.bss.dma) + SIZEOF(.bss.dma);
	_heap_end = ORIGIN(RAM) + LENGTH(RAM);

	_extram_start = ADDR(.bss.extram);
	_extram_end = ADDR(.bss.extram) + SIZEOF(.bss.extram);

	_itcm_block_count = (SIZEOF(.text.itcm) + SIZEOF(.ARM.exidx) + 0x7FFF) >> 15;
	_flexram_bank_config = 0xAAAAAAAA | ((1 << (_itcm_block_count * 2)) - 1);
	_estack = ORIGIN(DTCM) + ((16 - _itcm_block_count) << 15);

	_flashimagelen = __text_csf_end - ORIGIN(FLASH);
	_teensy_model_identifier = 0x25;

	.debug_info     0 : { *(.debug_info) }
	.debug_abbrev   0 : { *(.debug_abbrev) }
	.debug_line     0 : { *(.debug_line) }
	.debug_frame    0 : { *(.debug_frame) }
	.debug_str      0 : { *(.debug_str) }
	.debug_loc      0 : { *(.debug_loc) }

}
