ENTRY(ResetISR)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x60000000, LENGTH = 0x100000
    SRAM_OC (rwx) : ORIGIN = 0x20200000, LENGTH = 0xFFE0
    SRAM_DTC (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
    SRAM_ITC (rwx) : ORIGIN = 0x00000000, LENGTH = 32K
}

SECTIONS
{
    .boot_hdr : ALIGN(4)
    {
        FILL(0xff)
        . = 0x400; KEEP(*(.boot_hdr.conf))
        . = 0x1000; KEEP(*(.boot_hdr.ivt))
        . = 0x1020; KEEP(*(.boot_hdr.boot_data))
        . = 0x2000;
    } > FLASH

    .text : ALIGN(4)
    {
        FILL(0xff)
        __vectors_start__ = ABSOLUTE(.);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
        __section_table_start = .;
        __data_section_table = .;
        LONG(LOADADDR(.data)); LONG(ADDR(.data)); LONG(SIZEOF(.data));
        LONG(LOADADDR(.itcm_text)); LONG(ADDR(.itcm_text)); LONG(SIZEOF(.itcm_text));
        LONG(LOADADDR(.dtcm_coefficients)); LONG(ADDR(.dtcm_coefficients)); LONG(SIZEOF(.dtcm_coefficients));
        __data_section_table_end = .;
        __bss_section_table = .;
        LONG(ADDR(.bss)); LONG(SIZEOF(.bss));
        __bss_section_table_end = .;
        __section_table_end = .;
        *(.after_vectors*) *(.text*) *(.rodata*)
        . = ALIGN(32);
        *(.fft_coefficients*)
        KEEP(*(.init)) KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH
    .ARM.extab : { *(.ARM.extab*) } > FLASH
    .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH
    .itcm_text : ALIGN(32)
    {
        __itcm_text_start__ = .;
        *(.itcm_text*)
        . = ALIGN(32);
        __itcm_text_end__ = .;
    } > SRAM_ITC AT > FLASH
    .data : ALIGN(4) { _data = .; *(.data*) . = ALIGN(4); _edata = .; } > SRAM_OC AT > FLASH
    .dma (NOLOAD) : ALIGN(32) { __dma_start__ = .; *(.dma*) . = ALIGN(32); __dma_end__ = .; } > SRAM_DTC
    .fft_working (NOLOAD) : ALIGN(32)
    {
        __fft_working_start__ = .;
        *(.fft_working*)
        . = ALIGN(32);
        __fft_working_end__ = .;
    } > SRAM_DTC
    .dtcm_history (NOLOAD) : ALIGN(32)
    {
        __dtcm_history_start__ = .;
        *(.dtcm_history*)
        . = ALIGN(32);
        __dtcm_history_end__ = .;
    } > SRAM_DTC
    .dtcm_coefficients : ALIGN(32)
    {
        __dtcm_coefficients_start__ = .;
        *(.dtcm_coefficients*)
        . = ALIGN(32);
        __dtcm_coefficients_end__ = .;
    } > SRAM_DTC AT > FLASH
    .fft_state_ocram (NOLOAD) : ALIGN(32)
    {
        __fft_state_ocram_start__ = .;
        *(.fft_state_ocram*)
        . = ALIGN(32);
        __fft_state_ocram_end__ = .;
    } > SRAM_OC
    .bss (NOLOAD) : ALIGN(4) { _bss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; end = .; } > SRAM_OC
    .noinit (NOLOAD) : ALIGN(4) { *(.noinit*) } > SRAM_OC
    _HeapSize = 0; _pvHeapStart = .; _pvHeapLimit = .;
    _StackSize = 0x2000;
    _vStackBase = ORIGIN(SRAM_DTC) + LENGTH(SRAM_DTC) - _StackSize;
    _vStackTop = ORIGIN(SRAM_DTC) + LENGTH(SRAM_DTC);
    ASSERT(__dtcm_coefficients_end__ <= _vStackBase, "DTCM FFT state overlaps the stack")
    ASSERT(SIZEOF(.fft_state_ocram) + SIZEOF(.bss) + SIZEOF(.data) + SIZEOF(.noinit) <= LENGTH(SRAM_OC), "OCRAM overflow")
    ASSERT(SIZEOF(.itcm_text) <= LENGTH(SRAM_ITC), "ITCM hot code exceeds ITCM")
}
