/*******************************************************************************
  Main Source File

  Company:
    Microchip Technology Inc.

  File Name:
    main.c

  Summary:
    This file contains the "main" function for a project.

  Description:
    This file contains the "main" function for a project.  The
    "main" function calls the "SYS_Initialize" function to initialize the state
    machines of all modules in the system
 *******************************************************************************/

// *****************************************************************************
// *****************************************************************************
// Section: Included Files
// *****************************************************************************
// *****************************************************************************

#include <stddef.h>                     // Defines NULL
#include <stdbool.h>                    // Defines true
#include <stdlib.h>                     // Defines EXIT_FAILURE
#include <stdint.h>
#include "definitions.h"                // SYS function prototypes
#include "core_cm4.h"                   // DWT cycle counter and NVIC_SystemReset

// *****************************************************************************
// Application version identification
// *****************************************************************************

/* 升级时只需要改这三个数字；这里不能带 U 后缀，否则字符串化会把 U 打印出来 */
#define APP_VERSION_MAJOR           1
#define APP_VERSION_MINOR           0
#define APP_VERSION_PATCH           0

#define APP_VERSION_STRINGIFY(x)    #x
#define APP_VERSION_TO_STRING(x)    APP_VERSION_STRINGIFY(x)
#define APP_VERSION_BANNER          "APP v" APP_VERSION_TO_STRING(APP_VERSION_MAJOR) "." APP_VERSION_TO_STRING(APP_VERSION_MINOR) "." APP_VERSION_TO_STRING(APP_VERSION_PATCH) "\r\n"

// *****************************************************************************
// Bootloader trigger definitions
// *****************************************************************************

/* BL custom_trigger.c 检测 RAM 起始 16 字节：4 个 0x5048434D */
#define BTL_TRIGGER_RAM_START       0x20000000UL
#define BTL_TRIGGER_PATTERN         0x5048434DUL
#define BTL_TRIGGER_WORD_COUNT      4U

#define SW1_HOLD_TIME_MS           3000U
#define SW1_DEBOUNCE_TIME_MS        20U
#define CYCLES_PER_MS               (CPU_CLOCK_FREQUENCY / 1000U)
#define SW1_HOLD_THRESHOLD_CYCLES   ((uint32_t)SW1_HOLD_TIME_MS * CYCLES_PER_MS)
#define SW1_DEBOUNCE_CYCLES         ((uint32_t)SW1_DEBOUNCE_TIME_MS * CYCLES_PER_MS)

typedef enum
{
    SW1_STATE_IDLE = 0,
    SW1_STATE_DEBOUNCE_PRESS,
    SW1_STATE_PRESSED
} SW1_KEY_STATE;

static volatile SW1_KEY_STATE sw1State       = SW1_STATE_IDLE;
static volatile uint32_t      sw1DebounceAt = 0U;
static volatile uint32_t      sw1PressedAt   = 0U;

static void SW1_EnterBootloader(void)
{
    volatile uint32_t *triggerRam = (volatile uint32_t *)BTL_TRIGGER_RAM_START;
    uint32_t index;

    for (index = 0U; index < BTL_TRIGGER_WORD_COUNT; index++)
    {
        triggerRam[index] = BTL_TRIGGER_PATTERN;
    }

    /* NVIC_SystemReset 内部有 DSB，会保证 RAM 写入完成后再复位 */
    NVIC_SystemReset();
}

static void SW1_KeyTasks(void)
{
    /* PD00 内部上拉：松开为 1，按下为 0 */
    bool sw1Pressed = (SW1_Get() == 0U);
    uint32_t now    = DWT->CYCCNT;

    switch (sw1State)
    {
        case SW1_STATE_IDLE:
            if (sw1Pressed)
            {
                sw1DebounceAt = now;
                sw1State      = SW1_STATE_DEBOUNCE_PRESS;
            }
            break;

        case SW1_STATE_DEBOUNCE_PRESS:
            if (!sw1Pressed)
            {
                sw1State = SW1_STATE_IDLE;
            }
            else if ((now - sw1DebounceAt) >= SW1_DEBOUNCE_CYCLES)
            {
                sw1PressedAt = now;
                sw1State     = SW1_STATE_PRESSED;
            }
            break;

        case SW1_STATE_PRESSED:
            if (!sw1Pressed)
            {
                uint32_t heldCycles = now - sw1PressedAt;
                sw1State = SW1_STATE_IDLE;

                /* 按键抬起时判断：按住满 3 秒才进入 Bootloader，短按不触发 */
                if (heldCycles >= SW1_HOLD_THRESHOLD_CYCLES)
                {
                    SW1_EnterBootloader();
                }
            }
            break;

        default:
            sw1State = SW1_STATE_IDLE;
            break;
    }
}

static void APP_VersionBannerSend(void)
{
    const char *msg = APP_VERSION_BANNER;

    while (*msg != '\0')
    {
        /* 等待发送数据寄存器为空，再写入下一个字节 */
        while ((SERCOM4_REGS->USART_INT.SERCOM_INTFLAG & SERCOM_USART_INT_INTFLAG_DRE_Msk) == 0U)
        {
        }

        SERCOM4_REGS->USART_INT.SERCOM_DATA = (uint8_t)*msg;
        msg++;
    }

    /* 等待最后一个字节真正发完，避免回到主循环后被后续流程打断 */
    while (SERCOM4_USART_TransmitComplete() == false)
    {
    }
}

static void DWT_CycleCounterInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT       = 0U;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
}

// *****************************************************************************
// *****************************************************************************
// Section: Main Entry Point
// *****************************************************************************
// *****************************************************************************

int main ( void )
{
    /* Initialize all modules */
    SYS_Initialize ( NULL );

    /* 启动时通过 SERCOM4 输出一行 APP 版本号，用于区分当前运行的是哪个 Bank 的固件 */
    APP_VersionBannerSend();

    /* 板上 LED 为低电平有效：输出低电平点亮，高电平熄灭；LED1(PC21)、LED2(PA16) 同时点亮 */
    // LED1_Clear();
    LED2_Clear();

    DWT_CycleCounterInit();

    while ( true )
    {
        SW1_KeyTasks();

        /* Maintain state machines of all polled MPLAB Harmony modules. */
        SYS_Tasks ( );
    }

    /* Execution should not come here during normal operation */

    return ( EXIT_FAILURE );
}


/*******************************************************************************
 End of File
*/
