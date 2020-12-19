/*
 * GENERATED FILE - DO NOT EDIT
 * Copyright (c) 2008-2013 Code Red Technologies Ltd,
 * Copyright 2015, 2018-2019 NXP
 * (c) NXP Semiconductors 2013-2020
 * Generated linker script file for MIMXRT1011xxxxx
 * Created from memory.ldt by FMCreateLinkMemory
 * Using Freemarker v2.3.23
 * MCUXpresso IDE v11.2.1 [Build 4149] [2020-10-07] on 19-Dec-2020 01:08:39
 */

MEMORY
{
  /* Define each memory region */
  BOARD_FLASH (rx) : ORIGIN = 0x60000000, LENGTH = 0xf4240 /* 1000000 bytes (alias Flash) */  
  SRAM_OC (rwx) : ORIGIN = 0x20200000, LENGTH = 0x17a00 /* 96768 bytes (alias RAM) */  
  NCACHE_REGION (rwx) : ORIGIN = 0x20220000, LENGTH = 0x0 /* 0M bytes (alias RAM2) */  
  SRAM_DTC (rwx) : ORIGIN = 0x20000000, LENGTH = 0x8000 /* 32K bytes (alias RAM3) */  
  SRAM_ITC (rwx) : ORIGIN = 0x0, LENGTH = 0x0 /* 0M bytes (alias RAM4) */  
}

  /* Define a symbol for the top of each memory region */
  __base_BOARD_FLASH = 0x60000000  ; /* BOARD_FLASH */  
  __base_Flash = 0x60000000 ; /* Flash */  
  __top_BOARD_FLASH = 0x60000000 + 0xf4240 ; /* 1000000 bytes */  
  __top_Flash = 0x60000000 + 0xf4240 ; /* 1000000 bytes */  
  __base_SRAM_OC = 0x20200000  ; /* SRAM_OC */  
  __base_RAM = 0x20200000 ; /* RAM */  
  __top_SRAM_OC = 0x20200000 + 0x17a00 ; /* 96768 bytes */  
  __top_RAM = 0x20200000 + 0x17a00 ; /* 96768 bytes */  
  __base_NCACHE_REGION = 0x20220000  ; /* NCACHE_REGION */  
  __base_RAM2 = 0x20220000 ; /* RAM2 */  
  __top_NCACHE_REGION = 0x20220000 + 0x0 ; /* 0M bytes */  
  __top_RAM2 = 0x20220000 + 0x0 ; /* 0M bytes */  
  __base_SRAM_DTC = 0x20000000  ; /* SRAM_DTC */  
  __base_RAM3 = 0x20000000 ; /* RAM3 */  
  __top_SRAM_DTC = 0x20000000 + 0x8000 ; /* 32K bytes */  
  __top_RAM3 = 0x20000000 + 0x8000 ; /* 32K bytes */  
  __base_SRAM_ITC = 0x0  ; /* SRAM_ITC */  
  __base_RAM4 = 0x0 ; /* RAM4 */  
  __top_SRAM_ITC = 0x0 + 0x0 ; /* 0M bytes */  
  __top_RAM4 = 0x0 + 0x0 ; /* 0M bytes */  
