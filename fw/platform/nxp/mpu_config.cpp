#include "platform/nxp/mpu_config.hpp"

extern "C" {
#include "MIMXRT1011.h"
#include "mpu_armv7.h"
}

namespace platform::nxp {
void configure_mpu() noexcept {
    if ((SCB->CCR & SCB_CCR_IC_Msk) != 0U) SCB_DisableICache();
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) SCB_DisableDCache();

    ARM_MPU_Disable();

    // Match the known-working firmware. Region 0 prevents speculative access
    // to unmapped space; the later regions selectively permit real memories.
    MPU->RBAR = ARM_MPU_RBAR(0U, 0x00000000U);
    MPU->RASR = ARM_MPU_RASR(1U, ARM_MPU_AP_NONE, 0U, 0U, 0U, 0U, 0U, ARM_MPU_REGION_SIZE_4GB);

    MPU->RBAR = ARM_MPU_RBAR(1U, 0x80000000U);
    MPU->RASR = ARM_MPU_RASR(0U, ARM_MPU_AP_FULL, 2U, 0U, 0U, 0U, 0U, ARM_MPU_REGION_SIZE_512MB);

    MPU->RBAR = ARM_MPU_RBAR(2U, 0x60000000U);
    MPU->RASR = ARM_MPU_RASR(0U, ARM_MPU_AP_FULL, 2U, 0U, 0U, 0U, 0U, ARM_MPU_REGION_SIZE_512MB);

    // FlexSPI NOR XIP: read-only, executable, write-back cacheable.
    MPU->RBAR = ARM_MPU_RBAR(3U, 0x60000000U);
    MPU->RASR = ARM_MPU_RASR(0U, ARM_MPU_AP_RO, 0U, 0U, 1U, 1U, 0U, ARM_MPU_REGION_SIZE_16MB);

    MPU->RBAR = ARM_MPU_RBAR(4U, 0x00000000U);
    MPU->RASR = ARM_MPU_RASR(0U, ARM_MPU_AP_FULL, 2U, 0U, 0U, 0U, 0U, ARM_MPU_REGION_SIZE_1GB);

    MPU->RBAR = ARM_MPU_RBAR(5U, 0x00000000U);
    MPU->RASR = ARM_MPU_RASR(0U, ARM_MPU_AP_FULL, 0U, 0U, 1U, 1U, 0U, ARM_MPU_REGION_SIZE_32KB);

    MPU->RBAR = ARM_MPU_RBAR(6U, 0x20000000U);
    MPU->RASR = ARM_MPU_RASR(0U, ARM_MPU_AP_FULL, 0U, 0U, 1U, 1U, 0U, ARM_MPU_REGION_SIZE_32KB);

    MPU->RBAR = ARM_MPU_RBAR(7U, 0x20200000U);
    MPU->RASR = ARM_MPU_RASR(0U, ARM_MPU_AP_FULL, 0U, 0U, 1U, 1U, 0U, ARM_MPU_REGION_SIZE_64KB);

    MPU->RBAR = ARM_MPU_RBAR(9U, 0x40000000U);
    MPU->RASR = ARM_MPU_RASR(0U, ARM_MPU_AP_FULL, 2U, 0U, 0U, 0U, 0U, ARM_MPU_REGION_SIZE_2MB);

    MPU->RBAR = ARM_MPU_RBAR(10U, 0x42000000U);
    MPU->RASR = ARM_MPU_RASR(0U, ARM_MPU_AP_FULL, 2U, 0U, 0U, 0U, 0U, ARM_MPU_REGION_SIZE_32MB);

    ARM_MPU_Enable(MPU_CTRL_PRIVDEFENA_Msk);
    SCB_EnableDCache();
    SCB_EnableICache();
}
} // namespace platform::nxp
