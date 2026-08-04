#include "dev_rtc.h"
#include "main.h"   /* RTC_SCLK/IO/CE 핀 + 포트 매크로, HAL GPIO */

/* ---- DS1302 커맨드 바이트 구성 (데이터시트의 "Command Byte" 항목) ----
   bit7   = 1 (항상 고정)
   bit6   = RAM(1) / CLOCK(0) 영역 선택
   bit5-1 = 레지스터 주소
   bit0   = READ(1) / WRITE(0)
   아래 정의값은 그대로 쓸 수 있는 WRITE 커맨드 바이트이고,
   READ 커맨드는 항상 (WRITE 값 | 0x01) 이다. */
#define DS1302_SEC_W    0x80   /* 초  (bit7 = CH, Clock Halt) */
#define DS1302_MIN_W    0x82   /* 분 */
#define DS1302_HOUR_W   0x84   /* 시 (bit7 = 0 이면 24시간 모드) */
#define DS1302_DATE_W   0x86   /* 일(날짜) */
#define DS1302_MONTH_W  0x88   /* 월 */
#define DS1302_DAY_W    0x8A   /* 요일 */
#define DS1302_YEAR_W   0x8C   /* 년 */
#define DS1302_WP_W     0x8E   /* 제어 (bit7 = WP, Write Protect) */

#define DS1302_READ     0x01   /* WRITE 커맨드에 OR 하면 READ 커맨드가 된다 */

#define DS1302_CH_BIT   0x80   /* 초 레지스터의 Clock Halt 플래그 */
#define DS1302_WP_BIT   0x80   /* 제어 레지스터의 Write Protect 플래그 */

/* ---- BCD 변환 헬퍼 ----
   DS1302의 시각 레지스터는 BCD 형식이라 10진수 45를 0x45로 저장한다.
   따라서 읽을 때는 BCD -> 2진수, 쓸 때는 2진수 -> BCD 변환이 필요하다. */
static uint8_t bcd2bin(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

static uint8_t bin2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* ---- 비트뱅잉용 최소 지연 ----
   DS1302는 최대 2MHz까지 클럭을 받을 수 있으므로(= 반주기 250ns 이상이면 충분),
   각 엣지 사이에 nop 몇 개 정도의 여유만 줘도 타이밍 규격을 만족한다. ---- */
static void ds_delay(void)
{
    volatile uint8_t i;
    for (i = 0; i < 8; i++) { __NOP(); }
}

/* ---- I/O 핀 방향 전환 ----
   DS1302의 데이터 라인(IO)은 송신과 수신을 한 가닥으로 겸하는 양방향 선이라,
   보낼 때는 출력, 받을 때는 입력으로 그때그때 방향을 바꿔 줘야 한다. */
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
    init.Pull = GPIO_NOPULL;   /* 읽는 동안 DS1302가 직접 라인을 구동하므로 풀업/풀다운 불필요 */
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

/* 한 바이트를 LSB(최하위 비트)부터 순서대로 내보낸다.
   MCU가 데이터 비트를 실어 놓은 뒤 SCLK를 한 번 펄스하면 DS1302가 상승 엣지에서
   그 비트를 읽어 간다. 호출 전에 IO 핀이 출력 상태여야 한다. */
static void write_byte(uint8_t val)
{
    uint8_t i;
    for (i = 0; i < 8; i++)
    {
        io_write((val & 0x01) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        val >>= 1;
        ds_delay();
        sclk(GPIO_PIN_SET);     /* 상승 엣지에서 DS1302가 비트를 읽어 간다 */
        ds_delay();
        sclk(GPIO_PIN_RESET);
    }
}

/* 한 바이트를 LSB(최하위 비트)부터 순서대로 읽어들인다.
   DS1302는 SCLK 하강 엣지마다 다음 비트를 라인에 실어 주므로,
   클럭을 내린 뒤에 값을 읽는다. 호출 전에 IO 핀이 입력 상태여야 한다. */
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
        sclk(GPIO_PIN_RESET);   /* 하강 엣지에서 DS1302가 다음 비트를 내보낸다 */
        ds_delay();
    }
    return val;
}

/* 레지스터 한 개에 값 쓰기. addr에는 WRITE 커맨드 바이트를 넣는다(예: DS1302_MIN_W).
   [커맨드 바이트 -> 데이터 바이트] 순서로 연달아 보내면 한 번의 쓰기가 끝난다. */
static void reg_write(uint8_t addr, uint8_t data)
{
    sclk(GPIO_PIN_RESET);
    io_as_output();
    ce(GPIO_PIN_SET);           /* 통신이 끝날 때까지 CE를 High로 유지해야 한다 */
    ds_delay();

    write_byte(addr);
    write_byte(data);

    ce(GPIO_PIN_RESET);
}

/* 레지스터 한 개 읽기. addr에는 WRITE 커맨드 바이트를 그대로 넣으면 되고,
   READ 비트(bit0)는 이 함수 안에서 붙여 준다. */
static uint8_t reg_read(uint8_t addr)
{
    uint8_t data;

    sclk(GPIO_PIN_RESET);
    io_as_output();
    ce(GPIO_PIN_SET);
    ds_delay();

    write_byte((uint8_t)(addr | DS1302_READ));
    io_as_input();              /* MCU가 라인에서 손을 떼고, 이제부터는 DS1302가 구동한다 */
    data = read_byte();

    ce(GPIO_PIN_RESET);
    io_as_output();             /* 통신을 쉬는 동안에는 라인을 출력 상태로 되돌려 둔다 */

    return data;
}

void DS1302_Init(void)
{
    /* 가장 먼저 세 가닥 신호선을 대기(유휴) 상태로 맞춘다. */
    ce(GPIO_PIN_RESET);
    sclk(GPIO_PIN_RESET);
    io_as_output();

    /* Write Protect(쓰기 보호)를 먼저 푼다. 이걸 안 풀면 이후 쓰기가 전부 무시된다. */
    reg_write(DS1302_WP_W, 0x00);

    /* CH(Clock Halt) 비트를 0으로 만들어 발진기를 돌린다.
       현재 초 값을 먼저 읽어 그대로 보존한 채 bit7만 0으로 바꿔 다시 쓴다.
       (그냥 0x00을 쓰면 흘러가던 시각이 초기화되므로 반드시 읽고-고쳐-쓴다.) */
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

    reg_write(DS1302_WP_W, 0x00);   /* 쓰기 보호 해제 (이후 쓰기가 반영되도록) */

    /* 초 레지스터는 발진기가 계속 돌도록 CH 비트를 0으로 지워서 쓴다. */
    reg_write(DS1302_SEC_W,   (uint8_t)(bin2bcd(t->sec) & ~DS1302_CH_BIT));
    reg_write(DS1302_MIN_W,   bin2bcd(t->min));
    reg_write(DS1302_HOUR_W,  bin2bcd(t->hour));   /* bit7 = 0 -> 24시간 모드 */
    reg_write(DS1302_DATE_W,  bin2bcd(t->date));
    reg_write(DS1302_MONTH_W, bin2bcd(t->month));
    reg_write(DS1302_DAY_W,   bin2bcd(t->day));
    reg_write(DS1302_YEAR_W,  bin2bcd(t->year));

    reg_write(DS1302_WP_W, DS1302_WP_BIT);   /* 실수로 시각이 바뀌지 않도록 쓰기 보호를 다시 건다 */
}

void DS1302_GetTime(RTC_Time_t *t)
{
    if (t == 0)
    {
        return;
    }

    t->sec   = bcd2bin((uint8_t)(reg_read(DS1302_SEC_W) & 0x7F));  /* bit7(CH)은 시각이 아니므로 잘라낸다 */
    t->min   = bcd2bin(reg_read(DS1302_MIN_W));
    t->hour  = bcd2bin((uint8_t)(reg_read(DS1302_HOUR_W) & 0x3F)); /* 24시간 모드에서 시각은 bit5-0 */
    t->date  = bcd2bin(reg_read(DS1302_DATE_W));
    t->month = bcd2bin(reg_read(DS1302_MONTH_W));
    t->day   = bcd2bin(reg_read(DS1302_DAY_W));
    t->year  = bcd2bin(reg_read(DS1302_YEAR_W));
}
