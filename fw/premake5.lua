workspace "dykodac2"
    location "build/gmake2"
    configurations { "Debug", "Release", "HostTest" }
    language "C++"
    cppdialect "C++17"
    warnings "Extra"

local nxp_sdk = "third_party/nxp-mcuxpresso"
local common_includes = { ".", "platform/nxp", "usb", "third_party/tinyusb/src",
    nxp_sdk .. "/board", nxp_sdk .. "/CMSIS", nxp_sdk .. "/device", nxp_sdk .. "/periph",
    nxp_sdk .. "/drivers", nxp_sdk .. "/xip" }
local cpu_options = { "-mcpu=cortex-m7", "-mfpu=fpv5-sp-d16", "-mfloat-abi=hard", "-mthumb" }
local common_defines = { "CPU_MIMXRT1011DAE5A", "CPU_MIMXRT1011DAE5A_cm7", "DATA_SECTION_IS_CACHEABLE=1",
    "MCUXPRESSO_SDK", "__USE_CMSIS", "SDK_OS_BAREMETAL", "FSL_FEATURE_SAI_FIFO_COUNT=32" }
local sdk_core = { nxp_sdk .. "/board/clock_config.c", nxp_sdk .. "/device/system_MIMXRT1011.c",
    nxp_sdk .. "/drivers/fsl_clock.c", nxp_sdk .. "/drivers/fsl_common.c", nxp_sdk .. "/drivers/fsl_common_arm.c" }
local tinyusb_core = { "third_party/tinyusb/src/tusb.c", "third_party/tinyusb/src/common/tusb_fifo.c",
    "third_party/tinyusb/src/device/usbd.c", "third_party/tinyusb/src/class/hid/hid_device.c",
    "third_party/tinyusb/src/portable/chipidea/ci_hs/dcd_ci_hs.c" }

local function target_common()
    includedirs(common_includes)
    defines(common_defines)
    buildoptions { table.unpack(cpu_options), "-ffunction-sections", "-fdata-sections", "-ffreestanding",
        "-fno-exceptions", "-fstack-usage", "-Wall", "-Wextra", "-Wconversion", "-Wshadow", "-Wundef" }
    linkoptions { table.unpack(cpu_options), "-nostartfiles", "--specs=nano.specs", "--specs=nosys.specs",
        "-Wl,--gc-sections", "-Wl,--cref", "-Wl,--print-memory-usage" }
    filter "configurations:HostTest" flags { "ExcludeFromBuild" }
    filter "files:**.cpp" buildoptions { "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit" }
    filter "configurations:Debug" defines { "DEBUG=1" } optimize "Debug" symbols "On"
    filter "configurations:Release" optimize "Speed" symbols "Off" buildoptions { "-O2" }
    filter {}
end

local app_sources = {
    "app/app_controller.cpp", "audio/audio_engine.cpp", "audio/stream_buffer.cpp", "audio/feedback_controller.cpp",
    "audio/sample_rate.cpp", "board/clock_manager.cpp", "board/monotonic_timer.cpp", "board/output_relay.cpp",
    "board/target_board.cpp", "devices/dac.cpp", "platform/nxp/main.cpp", "platform/nxp/mpu_config.cpp",
    "platform/nxp/rom_bootloader.cpp", "platform/nxp/flexio_dac_transport.cpp", "platform/nxp/boot_control.cpp",
    "platform/nxp/update_layout.cpp", "usb/usb_device.cpp", "usb/usb_descriptors.cpp",
    "third_party/tinyusb/src/class/audio/audio_device.c", nxp_sdk .. "/drivers/fsl_gpio.c",
    nxp_sdk .. "/drivers/fsl_flexio.c", nxp_sdk .. "/drivers/fsl_flexio_spi.c", nxp_sdk .. "/drivers/fsl_dmamux.c",
    nxp_sdk .. "/drivers/fsl_edma.c", nxp_sdk .. "/drivers/fsl_sai.c", nxp_sdk .. "/drivers/fsl_sai_edma.c",
    nxp_sdk .. "/startup/startup_mimxrt1011.c"
}
for _,f in ipairs(tinyusb_core) do table.insert(app_sources,f) end
for _,f in ipairs(sdk_core) do table.insert(app_sources,f) end

local function app_project(name,suffix,address)
    project(name)
        kind "ConsoleApp" targetname("dykodac2_" .. suffix) targetextension ".elf"
        targetdir "build/gmake2/bin/%{cfg.buildcfg}" objdir("build/gmake2/obj/%{cfg.buildcfg}/" .. name)
        files(app_sources)
        target_common()
        defines { "XIP_EXTERNAL_FLASH=1", "__MCUXPRESSO", "CFG_TUSB_CONFIG_FILE=\"usb/tusb_config.h\"" }
        linkoptions { "-T../../platform/nxp/linker/dykodac2.ld", "-Wl,--defsym=APP_SLOT_BASE=" .. address,
            "-Wl,-Map=bin/%{cfg.buildcfg}/dykodac2_" .. suffix .. ".map" }
        postbuildcommands {
            "arm-none-eabi-objcopy -O binary \"%{cfg.buildtarget.abspath}\" \"%{cfg.buildtarget.directory}/dykodac2_" .. suffix .. ".payload.bin\"",
            "arm-none-eabi-objdump -d -S \"%{cfg.buildtarget.abspath}\" > \"%{cfg.buildtarget.directory}/dykodac2_" .. suffix .. ".lst\"",
            "arm-none-eabi-size \"%{cfg.buildtarget.abspath}\"" }
end
app_project("dykodac2_fw_a","a","0x60010000")
app_project("dykodac2_fw_b","b","0x60070000")

project "dykodac2_boot"
    kind "ConsoleApp" targetname "dykodac2_boot" targetextension ".elf"
    targetdir "build/gmake2/bin/%{cfg.buildcfg}" objdir "build/gmake2/obj/%{cfg.buildcfg}/dykodac2_boot"
    files { "boot/startup.S", "boot/main.cpp", "boot/descriptors.cpp", "boot/flexspi_flash.cpp",
        "platform/nxp/update_layout.cpp", "platform/nxp/sha256.cpp", "platform/nxp/mpu_config.cpp",
        nxp_sdk .. "/xip/evkmimxrt1010_flexspi_nor_config.c", nxp_sdk .. "/xip/fsl_flexspi_nor_boot.c" }
    for _,f in ipairs(tinyusb_core) do files { f } end
    for _,f in ipairs(sdk_core) do files { f } end
    target_common()
    defines { "XIP_EXTERNAL_FLASH=1", "XIP_BOOT_HEADER_ENABLE=1", "CFG_TUSB_CONFIG_FILE=\"boot/tusb_config.h\"" }
    linkoptions { "-T../../platform/nxp/linker/boot_manager.ld", "-Wl,-Map=bin/%{cfg.buildcfg}/dykodac2_boot.map" }
    postbuildcommands { "arm-none-eabi-objcopy -O binary \"%{cfg.buildtarget.abspath}\" \"%{cfg.buildtarget.directory}/dykodac2_boot.bin\"",
        "arm-none-eabi-size \"%{cfg.buildtarget.abspath}\"" }

project "dykodac2_host_tests"
    kind "ConsoleApp"
    targetdir "build/gmake2/bin/%{cfg.buildcfg}" objdir "build/gmake2/obj/%{cfg.buildcfg}/%{prj.name}"
    files { "app/app_controller.cpp", "audio/sample_rate.cpp", "audio/stream_buffer.cpp", "audio/feedback_controller.cpp",
        "devices/dac.cpp", "platform/nxp/update_layout.cpp", "platform/nxp/sha256.cpp",
        "usb/usb_event_queue_host.cpp", "tests/host/**.cpp" }
    includedirs { "." }
    filter "configurations:not HostTest" flags { "ExcludeFromBuild" }
    filter "configurations:HostTest" defines { "DYKODAC_HOST_TEST=1" } buildoptions { "-Wconversion", "-Wshadow", "-Wundef" }
    filter {}
