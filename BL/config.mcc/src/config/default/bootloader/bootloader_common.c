/*******************************************************************************
  Bootloader Common Source File

  File Name:
    bootloader_common.c

  Summary:
    This file contains common definitions and functions.

  Description:
    This file contains common definitions and functions.
 *******************************************************************************/

// DOM-IGNORE-BEGIN
/*******************************************************************************
* Copyright (C) 2019 Microchip Technology Inc. and its subsidiaries.
*
* Subject to your compliance with these terms, you may use Microchip software
* and any derivatives exclusively with Microchip products. It is your
* responsibility to comply with third party license terms applicable to your
* use of third party software (including open source software) that may
* accompany Microchip software.
*
* THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
* EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
* WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
* PARTICULAR PURPOSE.
*
* IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
* INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
* WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
* BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
* FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
* ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
* THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 *******************************************************************************/
// DOM-IGNORE-END

// *****************************************************************************
// *****************************************************************************
// Section: Include Files
// *****************************************************************************
// *****************************************************************************

#include "bootloader_common.h"

// *****************************************************************************
// *****************************************************************************
// Section: Type Definitions
// *****************************************************************************
// *****************************************************************************

/* Bootloader Major and Minor version sent for a Read Version command (MAJOR.MINOR)*/
#define BTL_MAJOR_VERSION       3U
#define BTL_MINOR_VERSION       7U
#define ASM_VECTOR              asm("bx %0"::"r" (reset_vector))

// *****************************************************************************
// *****************************************************************************
// Section: Global objects
// *****************************************************************************
// *****************************************************************************

// *****************************************************************************
// *****************************************************************************
// Section: Bootloader Local Functions
// *****************************************************************************
// *****************************************************************************

// *****************************************************************************
// *****************************************************************************
// Section: Bootloader Global Functions
// *****************************************************************************
// *****************************************************************************

/* MISRA C-2023 Rule 8.6 and 5.8 deviated below. Deviation record ID -
   H3_MISRAC_2023_R_8_6_DR_1, H3_MISRAC_2023_R_5_8_DR_1 */
bool __WEAK bootloader_Trigger(void)
{
    /* Function can be overriden with custom implementation */
    return false;
}

void __WEAK SYS_DeInitialize( void *data )
{
    /* Function can be overriden with custom implementation */
}

uint16_t __WEAK bootloader_GetVersion( void )
{
    /* Function can be overriden with custom implementation */
    uint16_t btlVersion = (((BTL_MAJOR_VERSION & (uint16_t)0xFFU) << 8) | (BTL_MINOR_VERSION & (uint16_t)0xFFU));

    return btlVersion;
}


/* MISRA C-2023 Rule 10.1, 10.4, 11.1, 11.6 deviated below. Deviation record ID -
   H3_MISRAC_2023_R_10_1_DR_1, H3_MISRAC_2023_R_10_4_DR_1, H3_MISRAC_2023_R_11_1_DR_1 & H3_MISRAC_2023_R_11_6_DR_1 */


/* Function to Generate CRC using the device service unit peripheral on programmed data */
uint32_t bootloader_CRCGenerate(uint32_t start_addr, uint32_t size)
{
    uint32_t crc  = 0xffffffffU;

    PAC_PeripheralProtectSetup (PAC_PERIPHERAL_DSU, PAC_PROTECTION_CLEAR);

    (void) DSU_CRCCalculate (start_addr, size, crc, &crc);

    PAC_PeripheralProtectSetup (PAC_PERIPHERAL_DSU, PAC_PROTECTION_SET);

    return crc;
}

/* Trigger a reset */
void bootloader_TriggerReset(void)
{
    NVIC_SystemReset();
}

void run_Application(uint32_t address)
{
    uint32_t msp            = *(uint32_t *)(address);
    uint32_t reset_vector   = *(uint32_t *)(address + 4U);

    if (msp == 0xffffffffU)
    {
        return;
    }

    /* Call Deinitialize routine to free any resources acquired by Bootloader */
    SYS_DeInitialize(NULL);

    __set_MSP(msp);

    ASM_VECTOR;
}



/* MISRAC 2023 deviation block end */

/* Erase the 8KB state block and program one 512B page holding
 * expected CRC + failure counter, rest 0xFF.
 * addr: physical block address (0x7E000 active, 0xFE000 inactive).
 */
static void FailSafeWrite(uint32_t addr, uint32_t expected_crc, uint32_t fail_count)
{
    static uint32_t page_buffer[PAGE_SIZE / 4U];
    uint32_t i;

    for (i = 0U; i < (PAGE_SIZE / 4U); i++)
    {
        page_buffer[i] = 0xFFFFFFFFU;
    }
    page_buffer[FAILSAFE_EXPECTED_CRC_OFFSET / 4U] = expected_crc;
    page_buffer[FAILSAFE_FAIL_COUNT_OFFSET / 4U]   = fail_count;

    (void) NVMCTRL_BlockErase(addr);
    while (NVMCTRL_IsBusy())
    {
        /* wait for block erase complete */
    }
    (void) NVMCTRL_PageWrite(page_buffer, addr);
    while (NVMCTRL_IsBusy())
    {
        /* wait for page write complete */
    }
}

/* Called on every cold boot with no trigger magic present.
 * Returns true when the APP may be jumped to; false to stay in serial
 * upgrade mode. Side effect: on a permitted jump the failure counter is
 * incremented and written back.
 */
bool bootloader_BootCheck(void)
{
    uint32_t expected_crc;
    uint32_t actual_crc;
    uint32_t fail_count;

    if (*(uint32_t *)APP_START_ADDRESS == 0xFFFFFFFFU)
    {
        return false;   /* empty APP area */
    }

    expected_crc = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_EXPECTED_CRC_OFFSET);
    actual_crc   = bootloader_CRCGenerate(APP_START_ADDRESS, APP_REGION_SIZE);

    /* A recorded expected CRC must match; a blank sector (first debugger flash)
     * skips the CRC check.
     */
    if ((expected_crc != 0xFFFFFFFFU) && (expected_crc != actual_crc))
    {
        return false;   /* APP region corrupted */
    }

    fail_count = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_FAIL_COUNT_OFFSET);
    if (fail_count == 0xFFFFFFFFU)
    {
        fail_count = 0U;
    }

    if (fail_count >= BOOT_FAIL_THRESHOLD)
    {
        return false;   /* repeated failed boot attempts, stay in BL */
    }

    fail_count++;
    FailSafeWrite(FAILSAFE_SECTOR_ADDR, expected_crc, fail_count);

    return true;
}

/* APP actively jumped back to the bootloader: it ran successfully.
 * Clear the failure counter, keep the recorded expected CRC.
 */
void bootloader_FailureCountClear(void)
{
    uint32_t expected_crc;

    expected_crc = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_EXPECTED_CRC_OFFSET);
    FailSafeWrite(FAILSAFE_SECTOR_ADDR, expected_crc, 0U);
}

/* End of a successful OTA, before BankSwap: store the expected CRC of the
 * freshly programmed inactive-bank APP and reset its failure counter.
 */
void bootloader_RecordAppCrc(uint32_t expected_crc)
{
    FailSafeWrite(INACTIVE_BANK_START + FAILSAFE_SECTOR_ADDR, expected_crc, 0U);
}

/* Read the current boot failure counter (0xFFFFFFFF = treated as 0). */
uint32_t bootloader_FailureCountGet(void)
{
    uint32_t count = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_FAIL_COUNT_OFFSET);
    if (count == 0xFFFFFFFFU)
    {
        count = 0U;
    }
    return count;
}