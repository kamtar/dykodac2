ENTRY(__Vectors)

MEMORY
{
    BOOT (rx) : ORIGIN = 0x60000000, LENGTH = 64K
    OCRAM (rwx) : ORIGIN = 0x20200000, LENGTH = 0xFFE0
    DTCM (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
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
        KEEP(*(.boot_vector))
        KEEP(*(.boot_trampoline))
        . = ALIGN(256);
    } > BOOT

    .ram_image ORIGIN(OCRAM) : ALIGN(32)
    {
        __ram_start__ = .;
        KEEP(*(.ram_vectors))
        *(.text*) *(.rodata*)
        *(.ARM.extab*)
        __exidx_start = .; *(.ARM.exidx*) __exidx_end = .;
        *(.data*)
        . = ALIGN(32);
        __ram_end__ = .;
    } > OCRAM AT > BOOT
    __ram_load__ = LOADADDR(.ram_image);

    .bss (NOLOAD) : ALIGN(32)
    {
        __bss_start__ = .;
        *(.bss*) *(COMMON)
        . = ALIGN(32);
        __bss_end__ = .;
    } > OCRAM

    .dma (NOLOAD) : ALIGN(32)
    {
        *(.dma*)
        . = ALIGN(32);
    } > DTCM

    _StackSize = 0x2000;
    _vStackTop = ORIGIN(DTCM) + LENGTH(DTCM);
    _vStackBase = _vStackTop - _StackSize;
    end = __bss_end__;
    _end = __bss_end__;
    _pvHeapStart = __bss_end__;
    _pvHeapLimit = __bss_end__;
    ASSERT(__ram_end__ <= ORIGIN(OCRAM) + LENGTH(OCRAM), "boot manager RAM image exceeds OCRAM")
    ASSERT(SIZEOF(.bss) + (__ram_end__ - __ram_start__) <= LENGTH(OCRAM), "boot manager OCRAM overflow")
}
