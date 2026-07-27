#include "dev_rtc.h"
#include "main.h"   /* RTC_SCLK/IO/CE 핀 + 포트 매크로, HAL GPIO */

/* ---- DS1302 커맨드 바이트 구성 (데이터시트 "Command Byte") ----
   bit7 = 1 (고정), bit6 = RAM(1)/CLOCK(0), bit5-1 = 주소, bit0 = READ(1)/WRITE(0).
   아래 레지스터 정의는 바로 쓸 수 있는 WRITE 주소이며, READ 주소는 항상
   write|0x01 임. */
#define DS1302_SEC_W    0x80   /* 초  (bit7 = CH, Clock Halt) */
#define DS1302_MIN_W    0x82   /* 분 */
#define DS1302_HOUR_W   0x84   /* 시 (bit7 = 0 이면 24시간 모드) */
#define DS1302_DATE_W   0x86   /* 일(날짜) */
#define DS1302_MONTH_W  0x88   /* 월 */
#define DS1302_DAY_W    0x8A   /* 요일 */
#define DS1302_YEAR_W   0x8C   /* 년 */
#define DS1302_WP_W     0x8E   /* 제어 (bit7 = WP, Write Protect) */

#define DS1302_READ     0x01   /* write 주소에 OR 하면 read 가 됨 */

#define DS1302_CH_BIT   0x80   /* 초 레지스터의 Clock Halt 플래그 */
#define DS1302_WP_BIT   0x80   /* 제어 레지스터의 Write Protect 플래그 */

/* ---- BCD 변환 헬퍼 (시간 레지스터는 예를 들어 45를 0x45로 저장함) ---- */
static uint8_t bcd2bin(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

static uint8_t bin2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* ---- 아주 짧은 비트뱅 타이밍 여유. DS1302는 최대 2MHz로 클럭킹하므로 엣지마다
   nop 몇 개의 안정화 시간이면 충분함. 레벨을 안정적으로 유지하는 용도. ---- */
static void ds_delay(void)
{
    volatile uint8_t i;
    for (i = 0; i < 8; i++) { __NOP(); }
}

/* ---- I/O 핀 방향 전환 (데이터 라인은 양방향임) ---- */
static void io_as_output(void)
{
    GPIO_InitTypeDef init = {0};
    init.Pin = RTC_IO_Pin;
    init.Mode = GPIO_MODE_OUTPUT_PP;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(RTC_IO_GPIO_Port, &init);
}

static void io_as_input(void)
{
    GPIO_InitTypeDef init = {0};
    init.Pin = RTC_IO_Pin;
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_NOPULL;   /* 읽는 동안 DS1302가 라인을 능동적으로 구동함 */
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(RTC_IO_GPIO_Port, &init);
}

/* ---- 저수준 라인 제어 헬퍼 ---- */
static void sclk(GPIO_PinState s)
{
    HAL_GPIO_WritePin(RTC_SCLK_GPIO_Port, RTC_SCLK_Pin, s);
}

static void ce(GPIO_PinState s)
{
    HAL_GPIO_WritePin(RTC_CE_GPIO_Port, RTC_CE_Pin, s);
}

static void io_write(GPIO_PinState s)
{
    HAL_GPIO_WritePin(RTC_IO_GPIO_Port, RTC_IO_Pin, s);
}

static uint8_t io_read(void)
{
    return (HAL_GPIO_ReadPin(RTC_IO_GPIO_Port, RTC_IO_Pin) == GPIO_PIN_SET) ? 1 : 0;
}

/* 한 바이트를 LSB부터 내보냄. 호스트가 비트를 내놓은 뒤 SCLK를 펄스하면
   DS1302가 상승 엣지에서 래치함. I/O가 이미 출력 상태라고 가정함. */
static void write_byte(uint8_t val)
{
    uint8_t i;
    for (i = 0; i < 8; i++)
    {
        io_write((val & 0x01) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        val >>= 1;
        ds_delay();
        sclk(GPIO_PIN_SET);     /* 상승 엣지: 칩이 비트를 샘플링함 */
        ds_delay();
        sclk(GPIO_PIN_RESET);
    }
}

/* 한 바이트를 LSB부터 읽어들임. DS1302는 SCLK 하강 엣지마다 다음 비트를
   내보내므로 클럭을 내린 뒤에 읽음. I/O가 입력 상태라고 가정함. */
static uint8_t read_byte(void)
{
    uint8_t i;
    uint8_t val = 0;
    for (i = 0; i < 8; i++)
    {
        if (io_read())
        {
            val |= (uint8_t)(1 << i);
        }
        sclk(GPIO_PIN_SET);
        ds_delay();
        sclk(GPIO_PIN_RESET);   /* 하강 엣지: 칩이 다음 비트를 내놓음 */
        ds_delay();
    }
    return val;
}

/* 레지스터 하나 쓰기. addr은 WRITE 커맨드 바이트(예: DS1302_MIN_W). */
static void reg_write(uint8_t addr, uint8_t data)
{
    sclk(GPIO_PIN_RESET);
    io_as_output();
    ce(GPIO_PIN_SET);           /* 트랜잭션 전체 동안 CE를 high로 유지 */
    ds_delay();

    write_byte(addr);
    write_byte(data);

    ce(GPIO_PIN_RESET);
}

/* 레지스터 하나 읽기. addr은 WRITE 커맨드 바이트이며, READ 비트는 여기서 더함. */
static uint8_t reg_read(uint8_t addr)
{
    uint8_t data;

    sclk(GPIO_PIN_RESET);
    io_as_output();
    ce(GPIO_PIN_SET);
    ds_delay();

    write_byte((uint8_t)(addr | DS1302_READ));
    io_as_input();              /* 라인을 놓아줌; 이제 칩이 라인을 구동함 */
    data = read_byte();

    ce(GPIO_PIN_RESET);
    io_as_output();             /* 쉬는 동안 라인을 구동 출력 상태로 둠 */

    return data;
}

void DS1302_Init(void)
{
    /* 무엇보다 먼저 라인을 유휴 레벨로 설정. */
    ce(GPIO_PIN_RESET);
    sclk(GPIO_PIN_RESET);
    io_as_output();

    /* Write Protect를 먼저 해제. 안 그러면 이후 레지스터 쓰기가 모두 무시됨. */
    reg_write(DS1302_WP_W, 0x00);

    /* Clock Halt 비트를 지워서 오실레이터가 동작하게 함. 현재 초 값은 보존하고
       CH(bit7)만 0으로 강제함. */
    {
        uint8_t sec = reg_read(DS1302_SEC_W);
        reg_write(DS1302_SEC_W, (uint8_t)(sec & ~DS1302_CH_BIT));
    }
}

void DS1302_SetTime(const RTC_Time_t *t)
{
    if (t == 0)
    {
        return;
    }

    reg_write(DS1302_WP_W, 0x00);   /* 쓰기가 허용되도록 보장 */

    /* 오실레이터가 계속 동작하도록 CH를 지운 초 값. */
    reg_write(DS1302_SEC_W,   (uint8_t)(bin2bcd(t->sec) & ~DS1302_CH_BIT));
    reg_write(DS1302_MIN_W,   bin2bcd(t->min));
    reg_write(DS1302_HOUR_W,  bin2bcd(t->hour));   /* bit7 = 0 -> 24시간 모드 */
    reg_write(DS1302_DATE_W,  bin2bcd(t->date));
    reg_write(DS1302_MONTH_W, bin2bcd(t->month));
    reg_write(DS1302_DAY_W,   bin2bcd(t->day));
    reg_write(DS1302_YEAR_W,  bin2bcd(t->year));

    reg_write(DS1302_WP_W, DS1302_WP_BIT);   /* 실수로 인한 쓰기를 막도록 다시 보호 설정 */
}

void DS1302_GetTime(RTC_Time_t *t)
{
    if (t == 0)
    {
        return;
    }

    t->sec   = bcd2bin((uint8_t)(reg_read(DS1302_SEC_W) & 0x7F));  /* CH 비트 마스킹 */
    t->min   = bcd2bin(reg_read(DS1302_MIN_W));
    t->hour  = bcd2bin((uint8_t)(reg_read(DS1302_HOUR_W) & 0x3F)); /* 24시간: bit5-0 */
    t->date  = bcd2bin(reg_read(DS1302_DATE_W));
    t->month = bcd2bin(reg_read(DS1302_MONTH_W));
    t->day   = bcd2bin(reg_read(DS1302_DAY_W));
    t->year  = bcd2bin(reg_read(DS1302_YEAR_W));
}
