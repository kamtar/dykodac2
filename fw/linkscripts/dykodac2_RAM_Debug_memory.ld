/*
 * GENERATED FILE - DO NOT EDIT
 * Copyright (c) 2008-2013 Code Red Technologies Ltd,
 * Copyright 2015, 2018-2019 NXP
 * (c) NXP Semiconductors 2013-2020
 * Generated linker script file for MIMXRT1011xxxxx
 * Created from memory.ldt by FMCreateLinkMemory
 * Using Freemarker v2.3.23
 * MCUXpresso IDE v11.2.1 [Build 4149] [2020-10-07] on 15-Dec-2020 00:18:44
 */

MEMORY
{
  /* Define each memory region */
  SRAM_OC (rwx) : ORIGIN = 0x20205800, LENGTH = 0x8000 /* 32K bytes (alias RAM) */  
}

  /* Define a symbol for the top of each memory region */
  __base_SRAM_OC = 0x20205800  ; /* SRAM_OC */  
  __base_RAM = 0x20205800 ; /* RAM */  
  __top_SRAM_OC = 0x20205800 + 0x8000 ; /* 32K bytes */  
  __top_RAM = 0x20205800 + 0x8000 ; /* 32K bytes */  
