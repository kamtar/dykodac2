require "eclipse-mk" -- To support eclipse makefile project generator
require "armgcc".exactver(9, 2, 1) -- Automaticka detekce toolchainu pro arm-none-eabi-

workspace "dykodac"
   configurations { "Debug_RAM", "Release_XIP"}
   architecture "ARM"
   --libdirs { "src/vendor_libs/**" }
   targetdir "build/bin"
   targetname "%{prj.name}_%{cfg.longname}"
   objdir "!build/%{cfg.longname}/obj/"

    newaction { -- V Budoucnu muze zmizet (zatim neni do verze 5 naportovano)
      trigger     = "clean",
      description = "clean the software",
      execute     = function ()
         print("clean the build...")
         os.rmdir("build")
         print("done.")
      end
   }

   project "DykoDAC2"
      toolset "gcc"
      kind "ConsoleApp"
      language "C++"   
      buildoptions {" -fno-common -g3 -Wall -c -ffunction-sections -fdata-sections -ffreestanding -fno-builtin -fmacro-prefix-map=\"../$(@D)/\"=. -mcpu=cortex-m7 -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -D__NEWLIB__ -fstack-usage -specs=nano.specs"}

      linkoptions { "-nostdlib -Xlinker -Map=\"build/dykodac2.map\" -Xlinker --gc-sections -Xlinker -print-memory-usage -Xlinker --sort-section=alignment -Xlinker --cref -mcpu=cortex-m7 -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb"}

       optimize "Debug"
       symbols "On"

      filter "files:**.cpp"
      buildoptions { "-std=gnu++1y", "-fabi-version=0", "-fno-exceptions", "-fno-rtti", "-fno-use-cxa-atexit", "-fno-threadsafe-statics", "-fcheck-new" }  
   filter "files:**.c"
      buildoptions { "-std=gnu11" }
   filter "files:**.S"
      buildoptions { "-x assembler-with-cpp" }
   filter {}

      defines { "CPU_MIMXRT1011DAE5A","CPU_MIMXRT1011DAE5A_cm7" ,"FSL_RTOS_BM","SDK_OS_BAREMETAL","SERIAL_PORT_TYPE_UART=1","SERIAL_PORT_TYPE_USBCDC=1",
      			"DEBUG_CONSOLE_TRANSFER_NON_BLOCKING", "USB_DEVICE_CONFIG_CDC_ACM=1", "SDK_DEBUGCONSOLE=1", "__MCUXPRESSO", "__USE_CMSIS", "__NEWLIB__", "BOARD_FLASH_SIZE=0xF4200"}
      targetextension ".elf"

      postbuildcommands {      
       '"%{cfg.gccprefix}size" --format=berkeley --totals "%{cfg.buildtarget.relpath}"'
      }
   
      includedirs {  "./","./**"}
        

      files { -- ** jsou rekurzivni, * neni
         "./**.h",
         "./**.hpp",
         "./**.cpp",
         "./**.c",
         "./**.S"
      }

      excludes {"./**example**"}

filter "configurations:Debug_RAM"
      defines { "DEBUG", "XIP_EXTERNAL_FLASH=0", "XIP_BOOT_HEADER_ENABLE=0"}
      optimize "Debug"
      symbols "On"
      linkoptions { "-T ./linkscripts/debug_ram/dykodac2_RAM_Debug.ld"}

filter "configurations:Release_XIP"
      defines { "DEBUG", "XIP_EXTERNAL_FLASH=1", "XIP_BOOT_HEADER_ENABLE=0"}
      optimize "Debug"
      symbols "On"
      linkoptions { "-T ./linkscripts/release_xip/dykodac2_Release.ld"}
