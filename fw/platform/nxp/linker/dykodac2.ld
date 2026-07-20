ENTRY(ResetISR)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x60000000, LENGTH = 1000000
    SRAM_OC (rwx) : ORIGIN = 0x20200000, LENGTH = 64K
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
        . = 0x1030; KEEP(*(.boot_hdr.dcd_data))
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
        __data_section_table_end = .;
        __bss_section_table = .;
        LONG(ADDR(.bss)); LONG(SIZEOF(.bss));
        __bss_section_table_end = .;
        __section_table_end = .;
        *(.after_vectors*)
        *(.text*)
        *(.rodata*)
        KEEP(*(.init)) KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : { *(.ARM.extab*) } > FLASH
    .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH

    .data : ALIGN(4)
    {
        _data = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM_OC AT > FLASH

    .dma (NOLOAD) : ALIGN(32)
    {
        __dma_start__ = .;
        *(.dma*)
        . = ALIGN(32);
        __dma_end__ = .;
    } > SRAM_DTC

    .bss (NOLOAD) : ALIGN(4)
    {
        _bss = .;
        *(.bss*) *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        end = .;
    } > SRAM_OC

    .noinit (NOLOAD) : ALIGN(4) { *(.noinit*) } > SRAM_OC

    _HeapSize = 0;
    _pvHeapStart = .;
    _pvHeapLimit = .;
    _StackSize = 0x2000;
    _vStackBase = ORIGIN(SRAM_DTC) + LENGTH(SRAM_DTC) - _StackSize;
    _vStackTop = ORIGIN(SRAM_DTC) + LENGTH(SRAM_DTC);

    _image_start = LOADADDR(.text);
    _image_end = LOADADDR(.data) + SIZEOF(.data);
    _image_size = _image_end - _image_start;

}
