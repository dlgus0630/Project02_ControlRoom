#include "dev_rfid.h"
#include "main.h"   /* RC522_CS/RST 핀 + 포트 매크로 */
#include "spi.h"    /* extern SPI_HandleTypeDef hspi4; */

/* ---- MFRC522 레지스터 주소 (데이터시트 9장) ---- */
#define CommandReg      0x01
#define ComIEnReg       0x02
#define DivIEnReg       0x03
#define ComIrqReg       0x04
#define DivIrqReg       0x05
#define ErrorReg        0x06
#define Status2Reg      0x08
#define FIFODataReg     0x09
#define FIFOLevelReg    0x0A
#define ControlReg      0x0C
#define BitFramingReg   0x0D
#define ModeReg         0x11
#define TxControlReg    0x14
#define TxASKReg        0x15
#define TModeReg        0x2A
#define TPrescalerReg   0x2B
#define TReloadRegH     0x2C
#define TReloadRegL     0x2D

/* ---- PCD(리더) 명령 ---- */
#define PCD_Idle        0x00
#define PCD_Transceive  0x0C
#define PCD_SoftReset   0x0F

/* ---- PICC(카드) 명령 ---- */
#define PICC_REQA       0x26
#define PICC_ANTICOLL   0x93

/* ---- 아래에서 쓰는 비트 마스크 ---- */
#define BIT_StartSend   0x80   /* BitFramingReg: 송신 시작                       */
#define BIT_RxIRq       0x20   /* ComIrqReg: 수신 완료                           */
#define BIT_IdleIRq     0x10   /* ComIrqReg: 명령 종료                           */
#define BIT_TxRFEn      0x03   /* TxControlReg: Tx1RFEn | Tx2RFEn (안테나 켜짐)  */
#define BIT_FIFOFlush   0x80   /* FIFOLevelReg: 버퍼 비우기 (FlushBuffer)        */
#define ERR_PROTECT     0x1B   /* ErrorReg: 버퍼/충돌/패리티/프로토콜 오류        */

#define RC522_TIMEOUT   100    /* SPI HAL 타임아웃 (ms) */

/* ---- 저수준 SPI 레지스터 접근 ---- */

static void RC522_CS(GPIO_PinState s)
{
    HAL_GPIO_WritePin(RC522_CS_GPIO_Port, RC522_CS_Pin, s);
}

/* 주소 바이트: 레지스터를 왼쪽으로 1비트 시프트; 쓰기는 MSB를 0, 읽기는 MSB를 1로 함. */
static void RC522_WriteReg(uint8_t reg, uint8_t val)
{
    uint8_t addr = (uint8_t)((reg << 1) & 0x7E);
    uint8_t tx[2];

    tx[0] = addr;
    tx[1] = val;

    RC522_CS(GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi4, tx, 2, RC522_TIMEOUT);
    RC522_CS(GPIO_PIN_SET);
}

static uint8_t RC522_ReadReg(uint8_t reg)
{
    uint8_t addr = (uint8_t)(((reg << 1) & 0x7E) | 0x80);
    uint8_t val = 0;

    RC522_CS(GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi4, &addr, 1, RC522_TIMEOUT);
    /* 값을 클록아웃함: 1바이트를 수신하는 동안 0x00을 보냄. */
    HAL_SPI_Receive(&hspi4, &val, 1, RC522_TIMEOUT);
    RC522_CS(GPIO_PIN_SET);

    return val;
}

/* read-modify-write 헬퍼 (읽어서 일부 비트만 바꾸고 다시 씀) */
static void RC522_SetBitMask(uint8_t reg, uint8_t mask)
{
    uint8_t tmp = RC522_ReadReg(reg);
    RC522_WriteReg(reg, (uint8_t)(tmp | mask));
}

static void RC522_ClearBitMask(uint8_t reg, uint8_t mask)
{
    uint8_t tmp = RC522_ReadReg(reg);
    RC522_WriteReg(reg, (uint8_t)(tmp & (~mask)));
}

static void RC522_AntennaOn(void)
{
    uint8_t val = RC522_ReadReg(TxControlReg);
    if ((val & BIT_TxRFEn) != BIT_TxRFEn)
    {
        RC522_SetBitMask(TxControlReg, BIT_TxRFEn);
    }
}

/* Transceive 명령으로 카드에 sendData[sendLen]을 보내고 응답을 backData에 모음.
   성공하면 응답 길이(바이트)를 *backLen에 쓰고 1을 반환; 오류/타임아웃 시 0을 반환. */
static uint8_t RC522_Transceive(const uint8_t *sendData, uint8_t sendLen,
                                uint8_t *backData, uint8_t *backLen)
{
    uint8_t i;
    uint8_t n;
    uint8_t irq;
    uint16_t wait;

    RC522_WriteReg(CommandReg, PCD_Idle);   /* 진행 중인 명령을 모두 멈춤 */
    RC522_WriteReg(ComIrqReg, 0x7F);        /* 모든 인터럽트 플래그를 지움 */
    RC522_SetBitMask(FIFOLevelReg, BIT_FIFOFlush);

    for (i = 0; i < sendLen; i++)
    {
        RC522_WriteReg(FIFODataReg, sendData[i]);
    }

    RC522_WriteReg(CommandReg, PCD_Transceive);
    RC522_SetBitMask(BitFramingReg, BIT_StartSend); /* 전송 시작 */

    /* 수신 완료 또는 명령 종료를 정해진 횟수 안에서 폴링. */
    wait = 2000;
    do
    {
        irq = RC522_ReadReg(ComIrqReg);
        wait--;
    } while (wait != 0 && (irq & (BIT_RxIRq | BIT_IdleIRq)) == 0);

    RC522_ClearBitMask(BitFramingReg, BIT_StartSend);

    if (wait == 0)
    {
        return 0;   /* 시간 내 응답 없음 */
    }

    if ((RC522_ReadReg(ErrorReg) & ERR_PROTECT) != 0)
    {
        return 0;
    }

    n = RC522_ReadReg(FIFOLevelReg);
    if (n == 0 || n > *backLen)
    {
        return 0;
    }

    for (i = 0; i < n; i++)
    {
        backData[i] = RC522_ReadReg(FIFODataReg);
    }
    *backLen = n;

    return 1;
}

void RC522_Init(void)
{
    RC522_CS(GPIO_PIN_SET);

    /* 하드웨어 리셋을 해제하고 오실레이터가 안정될 때까지 기다림. */
    HAL_GPIO_WritePin(RC522_RST_GPIO_Port, RC522_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(5);

    /* 소프트 리셋 후, CommandReg의 리셋 비트가 지워질 때까지 정해진 횟수만큼 기다림. */
    RC522_WriteReg(CommandReg, PCD_SoftReset);
    {
        uint16_t wait = 1000;
        while (wait != 0 && (RC522_ReadReg(CommandReg) & (1 << 4)) != 0)
        {
            wait--;
        }
    }

    /* 타이머: TPrescaler=0x0D3E, TReload=30 -> 약 25ms 자동 타임아웃 구간. */
    RC522_WriteReg(TModeReg, 0x8D);
    RC522_WriteReg(TPrescalerReg, 0x3E);
    RC522_WriteReg(TReloadRegL, 30);
    RC522_WriteReg(TReloadRegH, 0);

    RC522_WriteReg(TxASKReg, 0x40);   /* 100% ASK 변조 */
    RC522_WriteReg(ModeReg, 0x3D);    /* CRC 초기값 0x6363, 표준 모드 */

    RC522_AntennaOn();
}

uint8_t RC522_CheckCard(uint8_t *uidBuf)
{
    uint8_t buf[8];
    uint8_t len;
    uint8_t cmd[2];

    if (uidBuf == 0)
    {
        return 0;
    }

    /* --- REQA: 7비트 짧은 프레임, 2바이트 ATQA 응답을 기대 --- */
    RC522_WriteReg(BitFramingReg, 0x07);
    cmd[0] = PICC_REQA;
    len = sizeof(buf);
    if (!RC522_Transceive(cmd, 1, buf, &len))
    {
        return 0;   /* 필드 안에 카드 없음 */
    }

    /* --- Anticollision(충돌 방지): 온전한 바이트 단위, UID 4바이트 + BCC 1바이트를 기대 --- */
    RC522_WriteReg(BitFramingReg, 0x00);
    cmd[0] = PICC_ANTICOLL;   /* 0x93 */
    cmd[1] = 0x20;            /* NVB: UID 전체 */
    len = sizeof(buf);
    if (!RC522_Transceive(cmd, 2, buf, &len))
    {
        return 0;
    }

    if (len < 5)
    {
        return 0;
    }

    /* BCC = UID 4바이트의 XOR; 값이 맞지 않으면 거부함. */
    if ((buf[0] ^ buf[1] ^ buf[2] ^ buf[3]) != buf[4])
    {
        return 0;
    }

    uidBuf[0] = buf[0];
    uidBuf[1] = buf[1];
    uidBuf[2] = buf[2];
    uidBuf[3] = buf[3];

    return 4;
}
