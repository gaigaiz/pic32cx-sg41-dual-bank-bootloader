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
#include <stdio.h>                      // snprintf for temperature report
#include "definitions.h"                // SYS function prototypes
#include "core_cm4.h"                   // DWT cycle counter and NVIC_SystemReset

/* -------------------------------------------------------------------------- */
/* WDT + fail-safe state sector (mirrors bootloader_common.h layout)            */
/* -------------------------------------------------------------------------- */
#define FAILSAFE_SECTOR_ADDR                    (0x7E000UL)
#define FAILSAFE_EXPECTED_CRC_OFFSET            (0x00U)
#define FAILSAFE_APP_VERSION_OFFSET             (0x04U)
#define FAILSAFE_RETRY_CNT_OFFSET               (0x08U)
#define FAILSAFE_BANKSWAPPED_OFFSET              (0x0CU)
#define PAGE_SIZE                               (512U)
#define WDT_TIMEOUT_2S                          (0x8U)   /* CYC2048 @ 1.024kHz ≈ 2s */

/* Erase the 8KB state block and program one 512B page, clearing retry_cnt / bankswapped.
 * expected_crc and app_version are preserved (read back and rewritten).
 */
static void App_FailSafeMarkStarted(void)
{
    static uint32_t page_buffer[PAGE_SIZE / 4U];
    uint32_t expected_crc;
    uint32_t version;
    uint32_t i;

    expected_crc = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_EXPECTED_CRC_OFFSET);
    version      = *(uint32_t *)(FAILSAFE_SECTOR_ADDR + FAILSAFE_APP_VERSION_OFFSET);
    if (version == 0xFFFFFFFFU)
    {
        version = 0U;
    }

    for (i = 0U; i < (PAGE_SIZE / 4U); i++)
    {
        page_buffer[i] = 0xFFFFFFFFU;
    }
    page_buffer[FAILSAFE_EXPECTED_CRC_OFFSET / 4U] = expected_crc;
    page_buffer[FAILSAFE_APP_VERSION_OFFSET / 4U]  = version;
    page_buffer[FAILSAFE_RETRY_CNT_OFFSET / 4U]    = 0U;
    page_buffer[FAILSAFE_BANKSWAPPED_OFFSET / 4U]  = 0U;

    (void) NVMCTRL_BlockErase(FAILSAFE_SECTOR_ADDR);
    while (NVMCTRL_IsBusy())
    {
    }
    (void) NVMCTRL_PageWrite(page_buffer, FAILSAFE_SECTOR_ADDR);
    while (NVMCTRL_IsBusy())
    {
    }
}

/* Rough delay: ~2.5s at 12MHz using DWT cycle counter. */
static void App_DelayMs(uint32_t ms)
{
    uint32_t ticks = ms * 12000U;
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < ticks)
    {
    }
}

static void Uart_WriteStr(const char *msg)
{
    while (*msg != '\0')
    {
        while ((SERCOM4_REGS->USART_INT.SERCOM_INTFLAG & SERCOM_USART_INT_INTFLAG_DRE_Msk) == 0U)
        {
        }

        SERCOM4_REGS->USART_INT.SERCOM_DATA = (uint8_t)*msg;
        msg++;
    }

    while (SERCOM4_USART_TransmitComplete() == false)
    {
    }
}

/* -------------------------------------------------------------------------- */
/* Bit-bang I2C on PD08=SCL, PD09=SDA (on-board MCP9804, U900)                */
/* -------------------------------------------------------------------------- */
#define MCP9804_ADDR            0x18U   /* 7-bit address, A2/A1/A0 tied low    */
#define MCP9804_REG_TEMP        0x05U   /* TA ambient temperature register     */
#define MCP9804_WR_ADDR         (MCP9804_ADDR << 1U)         /* 0x30          */
#define MCP9804_RD_ADDR         ((MCP9804_ADDR << 1U) | 1U)  /* 0x31          */

#define I2C_HALF_BIT_US         5U      /* ~100kHz standard mode, half period  */

static void I2C_DelayUs(uint32_t us)
{
    volatile uint32_t cycles = us * (CPU_CLOCK_FREQUENCY / 1000000U);
    while (cycles-- != 0U)
    {
        __NOP();
    }
}

/* Open-drain style: release the line (input + pull-up) to go high, drive low. */
#define I2C_SCL_High()   do { Tem_SCL_Set();      Tem_SCL_InputEnable();   } while (0)
#define I2C_SCL_Low()    do { Tem_SCL_Clear();    Tem_SCL_OutputEnable();  } while (0)
#define I2C_SDA_High()   do { Tem_SDA_Set();      Tem_SDA_InputEnable();   } while (0)
#define I2C_SDA_Low()    do { Tem_SDA_Clear();    Tem_SDA_OutputEnable();  } while (0)
#define I2C_SDA_READ()   (Tem_SDA_Get() != 0U)

static void I2C_BusInit(void)
{
    /* Internal pull-up as a backup if the board has no external I2C pull-ups. */
    PORT_REGS->GROUP[3].PORT_PINCFG[8] |= PORT_PINCFG_PULLEN_Msk;
    PORT_REGS->GROUP[3].PORT_PINCFG[9] |= PORT_PINCFG_PULLEN_Msk;

    I2C_SCL_High();
    I2C_SDA_High();
    I2C_DelayUs(I2C_HALF_BIT_US);
}

static void I2C_Start(void)
{
    I2C_SDA_High();
    I2C_SCL_High();
    I2C_DelayUs(I2C_HALF_BIT_US);
    I2C_SDA_Low();
    I2C_DelayUs(I2C_HALF_BIT_US);
    I2C_SCL_Low();
    I2C_DelayUs(I2C_HALF_BIT_US);
}

static void I2C_Stop(void)
{
    I2C_SDA_Low();
    I2C_DelayUs(I2C_HALF_BIT_US);
    I2C_SCL_High();
    I2C_DelayUs(I2C_HALF_BIT_US);
    I2C_SDA_High();
    I2C_DelayUs(I2C_HALF_BIT_US);
}

/* Returns true when the slave ACKs the byte. */
static bool I2C_WriteByte(uint8_t byte)
{
    uint8_t bit;
    bool ack;

    for (bit = 0U; bit < 8U; bit++)
    {
        if ((byte & 0x80U) != 0U)
        {
            I2C_SDA_High();
        }
        else
        {
            I2C_SDA_Low();
        }
        I2C_DelayUs(I2C_HALF_BIT_US);
        I2C_SCL_High();
        I2C_DelayUs(I2C_HALF_BIT_US);
        I2C_SCL_Low();
        I2C_DelayUs(I2C_HALF_BIT_US);
        byte <<= 1U;
    }

    /* Ninth clock: slave drives SDA low to ACK. */
    I2C_SDA_High();
    I2C_DelayUs(I2C_HALF_BIT_US);
    I2C_SCL_High();
    I2C_DelayUs(I2C_HALF_BIT_US);
    ack = !I2C_SDA_READ();
    I2C_SCL_Low();
    I2C_DelayUs(I2C_HALF_BIT_US);

    return ack;
}

/* Reads one byte. masterAck=true sends ACK (more follow), false sends NACK. */
static uint8_t I2C_ReadByte(bool masterAck)
{
    uint8_t bit;
    uint8_t byte = 0U;

    I2C_SDA_High();
    for (bit = 0U; bit < 8U; bit++)
    {
        byte <<= 1U;
        I2C_SCL_High();
        I2C_DelayUs(I2C_HALF_BIT_US);
        if (I2C_SDA_READ())
        {
            byte |= 1U;
        }
        I2C_SCL_Low();
        I2C_DelayUs(I2C_HALF_BIT_US);
    }

    if (masterAck)
    {
        I2C_SDA_Low();
    }
    else
    {
        I2C_SDA_High();
    }
    I2C_DelayUs(I2C_HALF_BIT_US);
    I2C_SCL_High();
    I2C_DelayUs(I2C_HALF_BIT_US);
    I2C_SCL_Low();
    I2C_DelayUs(I2C_HALF_BIT_US);
    I2C_SDA_High();

    return byte;
}

/* Reads the 16-bit TA register. Returns true on success. */
static bool MCP9804_ReadRaw(int16_t *rawOut)
{
    uint8_t msb;
    uint8_t lsb;

    I2C_Start();
    if (!I2C_WriteByte(MCP9804_WR_ADDR))   { I2C_Stop(); return false; }
    if (!I2C_WriteByte(MCP9804_REG_TEMP))  { I2C_Stop(); return false; }

    I2C_Start();   /* repeated start */
    if (!I2C_WriteByte(MCP9804_RD_ADDR))  { I2C_Stop(); return false; }

    msb = I2C_ReadByte(true);
    lsb = I2C_ReadByte(false);
    I2C_Stop();

    *rawOut = (int16_t)(((uint16_t)msb << 8) | lsb);
    return true;
}

/* Converts to temperature in hundredths of a degree Celsius (x100, integer). */
static bool MCP9804_ReadTempC100(int32_t *tempC100)
{
    int16_t raw;
    uint16_t data;

    if (!MCP9804_ReadRaw(&raw))
    {
        return false;
    }

    data = (uint16_t)raw & 0x0FFFU;
    if ((raw & 0x1000U) != 0U)
    {
        /* Negative: data/16 - 256, scaled by 100. */
        *tempC100 = (int32_t)data * 100 / 16 - 25600;
    }
    else
    {
        *tempC100 = (int32_t)data * 100 / 16;
    }
    return true;
}

static void MCP9804_ReportTemperature(void)
{
    int32_t t100;
    char sign;
    int32_t absT;
    int32_t whole;
    int32_t frac;
    char line[24];

    if (!MCP9804_ReadTempC100(&t100))
    {
        Uart_WriteStr("Temp: I2C ERR\r\n");
        return;
    }

    if (t100 < 0)
    {
        sign = '-';
        absT = -t100;
    }
    else
    {
        sign = '+';
        absT = t100;
    }
    whole = absT / 100;
    frac  = absT % 100;

    (void)snprintf(line, sizeof(line), "Temp: %c%ld.%02ld C\r\n",
                   sign, (long)whole, (long)frac);
    Uart_WriteStr(line);
}

/* -------------------------------------------------------------------------- */
/* QSPI serial flash: on-board SST26VF064BA (U700), 64 Mbit / 8 MiB, 24-bit  */
/* address, 256-byte page. First pass runs single-bit SPI (1-1-1), no Quad.   */
/* -------------------------------------------------------------------------- */
#define QSPI_FLASH_PAGE_SIZE            256U
#define QSPI_FLASH_SECTOR_SIZE          4096U
#define QSPI_FLASH_TEST_ADDR            0x000000U

#define QSPI_FLASH_CMD_JEDEC_ID         0x9FU
#define QSPI_FLASH_CMD_RDSR             0x05U
#define QSPI_FLASH_CMD_WREN             0x06U
#define QSPI_FLASH_CMD_PP               0x02U
#define QSPI_FLASH_CMD_SECTOR_ERASE     0x20U
#define QSPI_FLASH_CMD_READ             0x03U

#define QSPI_FLASH_SR_BUSY              0x01U
#define QSPI_FLASH_SR_WEL               0x02U

#define QSPI_BUSY_TIMEOUT               10000000UL

static uint8_t QSPI_FlashReadStatus(void)
{
    qspi_register_xfer_t xfer;
    uint32_t sr = 0U;

    xfer.instruction   = QSPI_FLASH_CMD_RDSR;
    xfer.width         = SINGLE_BIT_SPI;
    xfer.dummy_cycles  = 0U;

    (void)QSPI_RegisterRead(&xfer, &sr, 1U);
    return (uint8_t)(sr & 0xFFU);
}

/* Poll BUSY until the flash finishes its internal erase/program. Feeds the
 * WDT while waiting; returns false on timeout instead of hanging in BL. */
static bool QSPI_FlashWaitReady(void)
{
    uint32_t timeout = QSPI_BUSY_TIMEOUT;

    while ((QSPI_FlashReadStatus() & QSPI_FLASH_SR_BUSY) != 0U)
    {
        WDT_Clear();
        if (timeout-- == 0U)
        {
            return false;
        }
    }
    return true;
}

static bool QSPI_FlashReadJedecId(uint8_t id[3])
{
    qspi_register_xfer_t xfer;
    uint32_t idBuf = 0U;

    xfer.instruction   = QSPI_FLASH_CMD_JEDEC_ID;
    xfer.width         = SINGLE_BIT_SPI;
    xfer.dummy_cycles  = 0U;

    if (!QSPI_RegisterRead(&xfer, &idBuf, 3U))
    {
        return false;
    }
    id[0] = (uint8_t)(idBuf & 0xFFU);
    id[1] = (uint8_t)((idBuf >> 8) & 0xFFU);
    id[2] = (uint8_t)((idBuf >> 16) & 0xFFU);
    return true;
}

static bool QSPI_FlashWriteEnable(void)
{
    qspi_command_xfer_t xfer;

    xfer.instruction = QSPI_FLASH_CMD_WREN;
    xfer.width       = SINGLE_BIT_SPI;
    xfer.addr_en     = false;
    xfer.addr_len    = ADDRL_24_BIT;

    return QSPI_CommandWrite(&xfer, 0U);
}

static bool QSPI_FlashSectorErase(uint32_t addr)
{
    qspi_command_xfer_t xfer;

    if (!QSPI_FlashWriteEnable())
    {
        return false;
    }

    xfer.instruction = QSPI_FLASH_CMD_SECTOR_ERASE;
    xfer.width       = SINGLE_BIT_SPI;
    xfer.addr_en     = true;
    xfer.addr_len    = ADDRL_24_BIT;

    if (!QSPI_CommandWrite(&xfer, addr))
    {
        return false;
    }

    return QSPI_FlashWaitReady();
}

/* Program one page region. Caller keeps len <= 256 and does not cross a page
 * boundary. Buffer must be 32-bit aligned for the PLIB word copy. */
static bool QSPI_FlashPageProgram(uint32_t addr, const uint8_t *data, uint32_t len)
{
    qspi_memory_xfer_t xfer;

    if ((len == 0U) || (len > QSPI_FLASH_PAGE_SIZE))
    {
        return false;
    }

    if (!QSPI_FlashWriteEnable())
    {
        return false;
    }

    xfer.instruction        = QSPI_FLASH_CMD_PP;
    xfer.option             = 0U;
    xfer.width              = SINGLE_BIT_SPI;
    xfer.addr_len           = ADDRL_24_BIT;
    xfer.option_en          = false;
    xfer.option_len         = OPTL_1_BIT;
    xfer.continuous_read_en = false;
    xfer.dummy_cycles       = 0U;

    if (!QSPI_MemoryWrite(&xfer, (uint32_t *)data, len, addr))
    {
        return false;
    }

    return QSPI_FlashWaitReady();
}

static bool QSPI_FlashRead(uint32_t addr, uint8_t *buf, uint32_t len)
{
    qspi_memory_xfer_t xfer;

    if (len == 0U)
    {
        return false;
    }

    xfer.instruction        = QSPI_FLASH_CMD_READ;
    xfer.option            = 0U;
    xfer.width              = SINGLE_BIT_SPI;
    xfer.addr_len          = ADDRL_24_BIT;
    xfer.option_en          = false;
    xfer.option_len         = OPTL_1_BIT;
    xfer.continuous_read_en = false;
    xfer.dummy_cycles       = 0U;

    return QSPI_MemoryRead(&xfer, (uint32_t *)buf, len, addr);
}

/* Bring-up self test: read JEDEC ID, erase the first sector, write 16 test
 * bytes, read back and compare. Note this erases/programs that sector on every
 * boot; once the link is proven, replace it with ID/head-word read only. */
static void QSPI_FlashSelfTest(void)
{
    uint8_t id[3] = { 0xFFU, 0xFFU, 0xFFU };
    static uint32_t txBuf[QSPI_FLASH_PAGE_SIZE / 4U];
    static uint32_t rxBuf[QSPI_FLASH_PAGE_SIZE / 4U];
    uint32_t i;
    char line[32];

    if (!QSPI_FlashReadJedecId(id))
    {
        Uart_WriteStr("QSPI: no response\r\n");
        return;
    }

    (void)snprintf(line, sizeof(line), "QSPI ID: %02X %02X %02X\r\n",
                   id[0], id[1], id[2]);
    Uart_WriteStr(line);

    for (i = 0U; i < 16U; i++)
    {
        ((uint8_t *)txBuf)[i] = (uint8_t)(0xA0U + i);
    }

    if (!QSPI_FlashSectorErase(QSPI_FLASH_TEST_ADDR))
    {
        Uart_WriteStr("QSPI erase busy timeout\r\n");
        return;
    }

    if (!QSPI_FlashPageProgram(QSPI_FLASH_TEST_ADDR, (const uint8_t *)txBuf, 16U))
    {
        Uart_WriteStr("QSPI program busy timeout\r\n");
        return;
    }

    if (!QSPI_FlashRead(QSPI_FLASH_TEST_ADDR, (uint8_t *)rxBuf, 16U))
    {
        Uart_WriteStr("QSPI read fail\r\n");
        return;
    }

    for (i = 0U; i < 16U; i++)
    {
        if (((uint8_t *)rxBuf)[i] != ((uint8_t *)txBuf)[i])
        {
            Uart_WriteStr("QSPI RW MISMATCH\r\n");
            return;
        }
    }

    Uart_WriteStr("QSPI RW OK\r\n");
}

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

#define TEMP_REPORT_PERIOD_MS       1000U
#define TEMP_REPORT_PERIOD_CYCLES   ((uint32_t)TEMP_REPORT_PERIOD_MS * CYCLES_PER_MS)

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

    /* Bit-bang I2C bus (PD08=SCL, PD09=SDA) for the on-board MCP9804. */
    I2C_BusInit();

    /* QSPI serial flash (SST26VF064BA, U700): probe ID and 16-byte loopback. */
    QSPI_FlashSelfTest();

    /* ---- WDT: hand it over from BL to APP ---- */
    WDT_TimeoutPeriodSet(WDT_TIMEOUT_2S);
    WDT_Enable();
    WDT_Clear();   /* feed once immediately after enable */

    /* Let the APP settle for ~2.5s before marking start success.
     * If the app crashes before this point, WDT will reset and BL counts it.
     */
    // App_DelayMs(1000U);

    /* Write success marker: clear retry_cnt and bankswapped in the state sector. */
    App_FailSafeMarkStarted();

    static uint32_t lastTempAt = 0U;

    while ( true )
    {
        WDT_Clear();   /* feed the dog every loop */

        SW1_KeyTasks();

        /* Report on-board MCP9804 temperature once per second. */
        uint32_t now = DWT->CYCCNT;
        if ((now - lastTempAt) >= TEMP_REPORT_PERIOD_CYCLES)
        {
            lastTempAt = now;
            MCP9804_ReportTemperature();
        }

        /* Maintain state machines of all polled MPLAB Harmony modules. */
        SYS_Tasks ( );
    }

    /* Execution should not come here during normal operation */

    return ( EXIT_FAILURE );
}


/*******************************************************************************
 End of File
*/
