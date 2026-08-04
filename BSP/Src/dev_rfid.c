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

/* ---- 이 파일에서 쓰는 레지스터별 비트 마스크 ---- */
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

/* MFRC522 SPI 주소 바이트 규칙: 레지스터 번호를 왼쪽으로 1비트 밀고(0x7E 로 마스크),
   MSB(최상위 비트)로 방향을 지정함 - 쓰기는 0, 읽기는 1. 최하위 비트는 항상 0. */
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
    /* 주소를 보낸 뒤 1바이트를 되받음. HAL_SPI_Receive 가 클럭을 만들기 위해
       더미 데이터를 내보내는 동안 RC522 가 해당 레지스터 값을 실어 보냄. */
    HAL_SPI_Receive(&hspi4, &val, 1, RC522_TIMEOUT);
    RC522_CS(GPIO_PIN_SET);

    return val;
}

/* 읽기-수정-쓰기 헬퍼: 레지스터를 읽어 지정한 비트만 바꾸고 되씀.
   나머지 비트는 그대로 보존됨. */
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

/* Transceive(송수신) 명령으로 카드에 sendData[sendLen] 을 보내고 응답을 backData 에 모음.
   호출 시 *backLen 에는 backData 버퍼의 크기를 넣어 주어야 함.
   성공하면 실제 받은 길이(바이트)를 *backLen 에 쓰고 1을 반환하고,
   오류/타임아웃/버퍼 초과 시 0을 반환함. */
static uint8_t RC522_Transceive(const uint8_t *sendData, uint8_t sendLen,
                                uint8_t *backData, uint8_t *backLen)
{
    uint8_t i;
    uint8_t n;
    uint8_t irq;
    uint16_t wait;

    RC522_WriteReg(CommandReg, PCD_Idle);   /* 진행 중인 명령을 모두 멈춤 */
    RC522_WriteReg(ComIrqReg, 0x7F);        /* 모든 인터럽트 플래그를 지움 */
    RC522_SetBitMask(FIFOLevelReg, BIT_FIFOFlush);  /* 지난 데이터가 남지 않게 FIFO 비움 */

    for (i = 0; i < sendLen; i++)
    {
        RC522_WriteReg(FIFODataReg, sendData[i]);
    }

    RC522_WriteReg(CommandReg, PCD_Transceive);
    RC522_SetBitMask(BitFramingReg, BIT_StartSend); /* 전송 시작 */

    /* 인터럽트를 쓰지 않으므로 ComIrqReg 를 직접 읽어 수신 완료(RxIRq) 또는
       명령 종료(IdleIRq)를 기다림. 무한 대기를 막으려고 횟수로 제한함
       (시간이 아니라 읽기 횟수 기준이라 SPI 속도에 따라 실제 시간은 달라짐). */
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

    /* 버퍼 넘침/충돌/패리티/프로토콜 오류가 하나라도 있으면 받은 값을 믿을 수 없음 */
    if ((RC522_ReadReg(ErrorReg) & ERR_PROTECT) != 0)
    {
        return 0;
    }

    /* FIFO 에 쌓인 실제 수신 바이트 수. 0이거나 호출자 버퍼보다 크면 버림 */
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

    /* 소프트 리셋. 리셋이 끝나면 CommandReg 의 PowerDown 비트(bit4)가 하드웨어에서
       자동으로 0이 되므로, 그 비트가 내려갈 때까지 횟수 제한을 두고 기다림. */
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

    /* TxASKReg 0x40 = Force100ASK 비트. ISO14443A 가 요구하는 100% ASK
       (반송파를 완전히 끊는 진폭 변조)를 강제로 켬. */
    RC522_WriteReg(TxASKReg, 0x40);
    /* ModeReg 0x3D = TxWaitRF(RF 필드가 안정된 뒤 송신) + 극성 설정 +
       CRCPreset 0b01 -> CRC 초기값 0x6363. ISO14443A 표준 설정값. */
    RC522_WriteReg(ModeReg, 0x3D);

    RC522_AntennaOn();  /* 안테나 드라이버(Tx1/Tx2) 출력을 켜야 카드에 전력이 전달됨 */
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

    /* --- 1단계 REQA: 필드 안에 카드가 있는지 물어봄. 2바이트 ATQA 응답을 기대함 --- */
    /* BitFramingReg 0x07 = 마지막 바이트에서 7비트만 송신(TxLastBits=7).
       REQA(0x26)는 규격상 7비트 짧은 프레임으로 보내야 하기 때문임. */
    RC522_WriteReg(BitFramingReg, 0x07);
    cmd[0] = PICC_REQA;
    len = sizeof(buf);
    if (!RC522_Transceive(cmd, 1, buf, &len))
    {
        return 0;   /* 필드 안에 카드 없음 */
    }

    /* --- 2단계 Anticollision(충돌 방지): UID 4바이트 + BCC 1바이트를 받아옴 --- */
    /* BitFramingReg 0x00 = 온전한 8비트 바이트 단위 송신으로 되돌림 */
    RC522_WriteReg(BitFramingReg, 0x00);
    cmd[0] = PICC_ANTICOLL;   /* 0x93 */
    cmd[1] = 0x20;            /* NVB=0x20: 아직 보낼 UID 비트가 없으니 전체를 달라는 뜻 */
    len = sizeof(buf);
    if (!RC522_Transceive(cmd, 2, buf, &len))
    {
        return 0;
    }

    if (len < 5)    /* UID 4 + BCC 1 보다 짧으면 불완전한 응답 */
    {
        return 0;
    }

    /* BCC(검사 바이트) = UID 4바이트를 모두 XOR 한 값.
       일치하지 않으면 통신이 깨졌거나 카드 여러 장이 겹친 경우이므로 거부함. */
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
