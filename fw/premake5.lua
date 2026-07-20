workspace "dykodac2"
    location "build/gmake2"
    configurations { "Debug", "Release", "HostTest" }
    language "C++"
    cppdialect "C++17"
    warnings "Extra"

local legacy = "../original_evkmimxrt1010_dev_audio_speaker_bm"

project "dykodac2_fw"
    kind "ConsoleApp"
    targetname "dykodac2"
    targetextension ".elf"
    targetdir "build/gmake2/bin/%{cfg.buildcfg}"
    objdir "build/gmake2/obj/%{cfg.buildcfg}/%{prj.name}"
    files {
        "app/app_controller.cpp",
        "audio/audio_engine.cpp",
        "audio/stream_buffer.cpp",
        "audio/feedback_controller.cpp",
        "audio/sample_rate.cpp",
        "board/clock_manager.cpp",
        "board/monotonic_timer.cpp",
        "board/output_relay.cpp",
        "board/target_board.cpp",
        "devices/dac.cpp",
        "platform/nxp/main.cpp",
        "platform/nxp/mpu_config.cpp",
        "platform/nxp/rom_bootloader.cpp",
        "platform/nxp/flexio_dac_transport.cpp",
        "usb/usb_device.cpp",
        "usb/usb_descriptors.cpp",
        "third_party/tinyusb/src/tusb.c",
        "third_party/tinyusb/src/common/tusb_fifo.c",
        "third_party/tinyusb/src/device/usbd.c",
        "third_party/tinyusb/src/device/usbd_control.c",
        "third_party/tinyusb/src/class/audio/audio_device.c",
        "third_party/tinyusb/src/class/hid/hid_device.c",
        "third_party/tinyusb/src/portable/chipidea/ci_hs/dcd_ci_hs.c",
        legacy .. "/board/clock_config.c",
        legacy .. "/device/system_MIMXRT1011.c",
        legacy .. "/drivers/fsl_clock.c",
        legacy .. "/drivers/fsl_common.c",
        legacy .. "/drivers/fsl_common_arm.c",
        legacy .. "/drivers/fsl_gpio.c",
        legacy .. "/drivers/fsl_flexio.c",
        legacy .. "/drivers/fsl_flexio_spi.c",
        legacy .. "/drivers/fsl_dmamux.c",
        legacy .. "/drivers/fsl_edma.c",
        legacy .. "/drivers/fsl_sai.c",
        legacy .. "/drivers/fsl_sai_edma.c",
        legacy .. "/startup/startup_mimxrt1011.c",
        legacy .. "/xip/evkmimxrt1010_flexspi_nor_config.c",
        legacy .. "/xip/fsl_flexspi_nor_boot.c",
    }
    includedirs {
        ".",
        "platform/nxp",
        "usb",
        "third_party/tinyusb/src",
        legacy .. "/board",
        legacy .. "/CMSIS",
        legacy .. "/device",
        legacy .. "/drivers",
        legacy .. "/xip",
    }
    defines {
        "CPU_MIMXRT1011DAE5A",
        "CPU_MIMXRT1011DAE5A_cm7",
        "DATA_SECTION_IS_CACHEABLE=1",
        "XIP_EXTERNAL_FLASH=1",
        "XIP_BOOT_HEADER_ENABLE=1",
        "MCUXPRESSO_SDK",
        "__MCUXPRESSO",
        "__USE_CMSIS",
        "SDK_OS_BAREMETAL",
        "CFG_TUSB_CONFIG_FILE=\"usb/tusb_config.h\"",
    }
    buildoptions {
        "-mcpu=cortex-m7", "-mfpu=fpv5-sp-d16", "-mfloat-abi=hard", "-mthumb",
        "-ffunction-sections", "-fdata-sections", "-ffreestanding",
        "-fno-exceptions", "-fstack-usage",
        "-Wall", "-Wextra", "-Wconversion", "-Wshadow", "-Wundef",
    }
    linkoptions {
        "-mcpu=cortex-m7", "-mfpu=fpv5-sp-d16", "-mfloat-abi=hard", "-mthumb",
        "-nostartfiles", "--specs=nano.specs", "--specs=nosys.specs",
        "-Wl,--gc-sections", "-Wl,--cref", "-Wl,--print-memory-usage",
        "-Wl,-Map=bin/%{cfg.buildcfg}/dykodac2.map",
        "-T../../platform/nxp/linker/dykodac2.ld",
    }
    filter "configurations:HostTest"
        flags { "ExcludeFromBuild" }
    filter "files:**.cpp"
        buildoptions { "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit" }
    filter "configurations:Debug"
        defines { "DEBUG=1" }
        optimize "Debug"
        symbols "On"
    filter "configurations:Release"
        optimize "Speed"
        symbols "Off"
        buildoptions { "-O2" }
    filter {}
    postbuildcommands {
        "arm-none-eabi-objcopy -O binary \"%{cfg.buildtarget.abspath}\" \"%{cfg.buildtarget.directory}/dykodac2.bin\"",
        "arm-none-eabi-objcopy -O ihex \"%{cfg.buildtarget.abspath}\" \"%{cfg.buildtarget.directory}/dykodac2.hex\"",
        "arm-none-eabi-objdump -d -S \"%{cfg.buildtarget.abspath}\" > \"%{cfg.buildtarget.directory}/dykodac2.lst\"",
        "arm-none-eabi-size \"%{cfg.buildtarget.abspath}\"",
    }

project "dykodac2_host_tests"
    kind "ConsoleApp"
    targetdir "build/gmake2/bin/%{cfg.buildcfg}"
    objdir "build/gmake2/obj/%{cfg.buildcfg}/%{prj.name}"
    files {
        "app/app_controller.cpp",
        "audio/sample_rate.cpp",
        "audio/stream_buffer.cpp",
        "audio/feedback_controller.cpp",
        "devices/dac.cpp",
        "usb/usb_event_queue_host.cpp",
        "tests/host/**.cpp",
    }
    includedirs { "." }
    filter "configurations:not HostTest"
        flags { "ExcludeFromBuild" }
    filter "configurations:HostTest"
        defines { "DYKODAC_HOST_TEST=1" }
        buildoptions { "-Wconversion", "-Wshadow", "-Wundef" }
    filter {}
