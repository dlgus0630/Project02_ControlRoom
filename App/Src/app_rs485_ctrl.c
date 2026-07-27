#include "app_rs485_ctrl.h"
#include "usart.h"     /* extern UART_HandleTypeDef huart5; (그리고 huart1) */
#include <string.h>
#include <stdio.h>     /* 임시 진단용 — 송신 시각/내용 로그 */

/* ======================================================
 * [동작 원리] — Modbus RTU 마스터(관리실) 폴링 방식
 *
 * 마스터는 "요청을 먼저 보내고 -> 응답을 기다린다"를 반복한다.
 *
 * 1) IDLE 상태에서 폴링 주기(200ms)가 지나면 요청 1개를 전송한다.
 *    - 보낼 쓰기 명령(목표층/비상해제)이 예약돼 있으면 그 0x06 쓰기를 먼저 보내고,
 *      없으면 0x03 읽기(레지스터 0~8 전부)를 보낸다.
 *    - 전송 뒤에는 WAITING 상태로 넘어가고, 무엇을 보냈는지 기억해 둔다.
 *
 * 2) WAITING 상태에서는 UART5 수신 인터럽트로 응답 바이트가 버퍼에 쌓인다.
 *    - "마지막 바이트로부터 5ms 이상 아무것도 안 오면" 프레임이 끝났다고 본다.
 *      (Modbus RTU는 원래 프레임 사이 침묵 구간으로 경계를 나누므로 같은 효과)
 *    - 프레임이 도착하면 CRC를 검사하고,
 *        · 읽기 응답이면 값을 파싱해서 shadow 변수에 저장,
 *        · 쓰기 응답(에코)이면 그 쓰기 예약을 완료 처리한다.
 *      성공하면 link_ok=1로 올리고 IDLE로 복귀.
 *    - 응답 타임아웃(100ms) 동안 아무것도 안 오면 실패로 보고 IDLE로 복귀.
 *      연속 실패가 일정 횟수(3회)를 넘으면 link_ok=0 으로 내린다.
 *      (재시도는 다음 주기에 자동으로 다시 요청하므로 별도 로직 없음)
 * ====================================================== */

/* ── 타이밍 설정값 ── */
#define POLL_INTERVAL_MS   200   /* 폴링 주기: 이 시간마다 요청 1개 전송 */
#define RESP_TIMEOUT_MS    100   /* 응답 대기 한계: 넘으면 이번 요청 실패 */
#define FRAME_GAP_MS       5     /* 마지막 바이트 후 이만큼 조용하면 프레임 끝 */
#define MAX_FAIL_COUNT     3     /* 연속 실패가 이 값 이상이면 link_ok=0 */
#define MAX_WRITE_RETRY    5     /* 쓰기 요청이 이 횟수만큼 응답 없으면 포기하고 읽기로 복귀 */

/* ── 상태머신 상태 ── */
#define ST_IDLE     0
#define ST_WAITING  1

/* ── 방금 보낸 요청의 종류 ── */
#define REQ_READ          0   /* 0x03 읽기 요청 */
#define REQ_WRITE_TARGET  1   /* 0x06 목표층 쓰기 */
#define REQ_WRITE_CLEAR   2   /* 0x06 비상해제 쓰기 */
#define REQ_WRITE_INSPECT 3   /* 0x06 점검 시작/해제 쓰기 */

/* ── 수신 버퍼 ── */
#define RX_BUF_SIZE   64
static uint8_t  s_rx_buf[RX_BUF_SIZE];
static volatile uint8_t  s_rx_len = 0;
static volatile uint32_t s_last_byte_tick = 0;
static uint8_t  s_rx_byte;   /* 인터럽트로 1바이트씩 받는 임시 변수 */

/* ── 상태머신 변수 ── */
static uint8_t  s_state = ST_IDLE;
static uint8_t  s_last_req = REQ_READ;   /* 방금 보낸 요청 종류 */
static uint32_t s_last_poll_tick = 0;    /* 마지막 요청 전송 시각 */
static uint32_t s_req_send_tick = 0;     /* 응답 타임아웃 계산용 전송 시각 */
static uint8_t  s_fail_count = 0;        /* 연속 실패 횟수 */
static uint8_t  s_link_ok = 0;           /* 통신 상태: 1=정상, 0=끊김 */
static uint8_t  s_write_fail_count = 0;  /* 쓰기 요청이 연속으로 응답을 못 받은 횟수 */

/* ── 쓰기 요청 큐(아주 단순하게 플래그+값으로만) ── */
static uint8_t  s_pending_write_target = 0;   /* 1이면 목표층 쓰기 대기 중 */
static uint8_t  s_pending_target_value = 0;   /* 쓸 목표층 값 */
static uint8_t  s_pending_clear_emergency = 0;/* 1이면 비상해제 쓰기 대기 중 */
static uint8_t  s_pending_write_inspect = 0;  /* 1이면 점검 관련 쓰기 대기 중 */
static uint8_t  s_pending_inspect_value = 0;  /* 쓸 값(1=점검 시작, 0=점검 해제) */

/* ── shadow 변수(슬레이브에서 읽어온 최근 값) ── */
static uint8_t  s_current_floor = 0;
static uint8_t  s_target_floor  = 0;
static uint8_t  s_elev_state    = 0;
static uint8_t  s_emergency     = 0;
static uint16_t s_temp_x10      = 0;
static uint16_t s_humid_x10     = 0;
static uint8_t  s_dht_status    = 0;
static uint8_t  s_inspection    = 0;
static uint16_t s_error_code    = 0;   /* 통합 오류코드(REG_ERROR_CODE) */

/* ======================================================
 * CRC16 계산 (Modbus 표준 방식 - 슬레이브와 동일)
 * ====================================================== */
static uint16_t CRC16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t pos = 0; pos < len; pos++)
    {
        crc ^= buf[pos];
        for (int i = 0; i < 8; i++)
        {
            if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
            else              { crc >>= 1; }
        }
    }
    return crc;
}

/* ======================================================
 * 요청 프레임 전송 (CRC 붙여서 UART5로 송신)
 * 보내기 전에 수신 버퍼를 비워서 지난 응답 찌꺼기를 지운다.
 * ====================================================== */
static void Send_Request(uint8_t *frame, uint16_t len)
{
    uint16_t crc = CRC16(frame, len);
    frame[len]     = (uint8_t)(crc & 0xFF);   /* CRC 하위바이트 먼저 (wire 순서) */
    frame[len + 1] = (uint8_t)(crc >> 8);     /* CRC 상위바이트 */

    s_rx_len = 0;   /* 새 응답을 받기 위해 버퍼 초기화 */

    /* 임시 진단: 실제로 언제, 무엇을 내보냈는지 — 엘리베이터 쪽
       [RS485 RX] 로그와 시간을 맞춰봐서 진짜 요청이 도착하는지,
       아니면 지금 보이는 게 그냥 버스 잡음인지 구분하기 위함 */
    printf("[RS485 TX] t=%lu len=%u raw:", (unsigned long)HAL_GetTick(), (unsigned)(len + 2));
    for (uint16_t i = 0; i < len + 2; i++)
    {
        printf(" %02X", frame[i]);
    }
    printf("\r\n");

    HAL_UART_Transmit(&huart5, frame, len + 2, 100);
}

/* ======================================================
 * 0x03 읽기 요청 전송: 레지스터 0번부터 9개(0~8) 전부 읽기
 * 프레임: 01 03 00 00 00 09 + CRC
 * ====================================================== */
static void Send_ReadRequest(void)
{
    uint8_t frame[8];
    frame[0] = MODBUS_SLAVE_ID;
    frame[1] = 0x03;
    frame[2] = 0x00;   /* 시작주소 상위 */
    frame[3] = 0x00;   /* 시작주소 하위 = 0번 */
    frame[4] = 0x00;   /* 개수 상위 */
    frame[5] = 0x09;   /* 개수 하위 = 9개(0~8번) */
    Send_Request(frame, 6);
    s_last_req = REQ_READ;
}

/* ======================================================
 * 0x06 단일 레지스터 쓰기 요청 전송
 * 프레임: 01 06 addr_hi addr_lo val_hi val_lo + CRC
 * ====================================================== */
static void Send_WriteRequest(uint16_t addr, uint16_t value)
{
    uint8_t frame[8];
    frame[0] = MODBUS_SLAVE_ID;
    frame[1] = 0x06;
    frame[2] = (uint8_t)(addr >> 8);
    frame[3] = (uint8_t)(addr & 0xFF);
    frame[4] = (uint8_t)(value >> 8);
    frame[5] = (uint8_t)(value & 0xFF);
    Send_Request(frame, 6);
}

/* ======================================================
 * 읽기 응답 파싱: 값 18바이트(레지스터 9개)를 shadow 변수에 저장
 * 응답형식: 01 03 12 <데이터18> CRC_lo CRC_hi  (각 레지스터는 상위바이트 먼저)
 * 성공하면 1, 형식이 안 맞으면 0
 * ====================================================== */
static uint8_t Parse_ReadResponse(uint8_t *buf, uint8_t len)
{
    /* 최소 길이: 주소1 + 기능1 + 바이트수1 + 데이터18 + CRC2 = 23 */
    if (len < 23) return 0;
    if (buf[1] != 0x03) return 0;
    if (buf[2] != 18)   return 0;   /* 데이터 바이트 수는 18이어야 함(9개×2바이트) */

    uint16_t reg[9];
    for (int i = 0; i < 9; i++)
    {
        reg[i] = (uint16_t)((buf[3 + i * 2] << 8) | buf[3 + i * 2 + 1]);
    }

    s_current_floor = (uint8_t)reg[REG_CURRENT_FLOOR];
    s_target_floor  = (uint8_t)reg[REG_TARGET_FLOOR];
    s_elev_state    = (uint8_t)reg[REG_PENDING_ACTION];
    s_emergency     = (uint8_t)reg[REG_EMERGENCY];
    s_temp_x10      = reg[REG_TEMPERATURE_X10];
    s_humid_x10     = reg[REG_HUMIDITY_X10];
    s_dht_status    = (uint8_t)reg[REG_DHT_STATUS];
    s_inspection    = (uint8_t)reg[REG_INSPECTION];
    s_error_code    = reg[REG_ERROR_CODE];
    return 1;
}

/* ======================================================
 * 도착한 응답 프레임을 검사하고 처리한다.
 * 성공(내가 기대한 응답이 CRC까지 정상)하면 1, 아니면 0을 반환.
 * ====================================================== */
static uint8_t Process_Response(uint8_t *buf, uint8_t len)
{
    if (len < 5) return 0;                  /* 너무 짧으면 무시 */
    if (buf[0] != MODBUS_SLAVE_ID) return 0; /* 상대 주소가 아니면 무시 */

    /* CRC 확인 (마지막 2바이트, wire상 하위바이트 먼저) */
    uint16_t recv_crc = (uint16_t)((buf[len - 1] << 8) | buf[len - 2]);
    uint16_t calc_crc = CRC16(buf, len - 2);
    if (recv_crc != calc_crc) return 0;      /* CRC 틀리면 실패 */

    if (s_last_req == REQ_READ)
    {
        /* 읽기 요청을 보냈으니 0x03 응답이어야 함 */
        return Parse_ReadResponse(buf, len);
    }
    else
    {
        /* 쓰기 요청을 보냈으니 0x06 에코(6바이트+CRC=8)여야 함 */
        if (buf[1] != 0x06) return 0;
        if (len < 8)        return 0;

        /* 어떤 쓰기였는지에 따라 해당 예약을 완료 처리 */
        if (s_last_req == REQ_WRITE_TARGET)
        {
            s_pending_write_target = 0;
        }
        else if (s_last_req == REQ_WRITE_CLEAR)
        {
            s_pending_clear_emergency = 0;
        }
        else if (s_last_req == REQ_WRITE_INSPECT)
        {
            s_pending_write_inspect = 0;
        }
        return 1;
    }
}

/* ======================================================
 * 쓰기 요청이 계속 응답을 못 받을 때 무한 재시도에 빠지지 않도록,
 * 일정 횟수 넘으면 그 쓰기를 포기하고 예약을 지운다.
 * (포기해도 사용자가 다시 시도하면 RS485_SetTargetFloor 등을 또 호출하면 됨)
 * ====================================================== */
static void Give_Up_Stuck_Write(void)
{
    if (s_write_fail_count < 255) s_write_fail_count++;

    if (s_write_fail_count >= MAX_WRITE_RETRY)
    {
        s_pending_write_target = 0;
        s_pending_clear_emergency = 0;
        s_pending_write_inspect = 0;
        s_write_fail_count = 0;
    }
}

/* ======================================================
 * UART 수신 인터럽트 콜백 — 1바이트 받을 때마다 자동 호출됨
 * 이 보드에는 huart1도 있으므로 반드시 UART5만 처리한다.
 * ====================================================== */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != UART5) return;   /* UART5(Modbus 마스터)만 처리 */

    if (s_rx_len < RX_BUF_SIZE)
    {
        s_rx_buf[s_rx_len++] = s_rx_byte;
        s_last_byte_tick = HAL_GetTick();
    }

    HAL_UART_Receive_IT(&huart5, &s_rx_byte, 1);   /* 다음 1바이트 또 받기 시작 */
}

/* ======================================================
 * 공개 API
 * ====================================================== */
void RS485_Init(void)
{
    s_rx_len = 0;
    s_state = ST_IDLE;
    s_last_poll_tick = HAL_GetTick();
    HAL_UART_Receive_IT(&huart5, &s_rx_byte, 1);   /* 수신 인터럽트 시작 */
}

/* 메인루프에서 매번 호출 — 폴링 상태머신을 한 스텝 처리 */
void RS485_Update(void)
{
    uint32_t now = HAL_GetTick();

    if (s_state == ST_IDLE)
    {
        /* 폴링 주기가 지났을 때만 다음 요청을 보낸다 */
        if (now - s_last_poll_tick < POLL_INTERVAL_MS) return;

        /* 쓰기 예약이 있으면 쓰기를 먼저, 없으면 읽기 */
        if (s_pending_write_target)
        {
            Send_WriteRequest(REG_TARGET_FLOOR, s_pending_target_value);
            s_last_req = REQ_WRITE_TARGET;
        }
        else if (s_pending_clear_emergency)
        {
            Send_WriteRequest(REG_EMERGENCY, 0);
            s_last_req = REQ_WRITE_CLEAR;
        }
        else if (s_pending_write_inspect)
        {
            Send_WriteRequest(REG_INSPECTION, s_pending_inspect_value);
            s_last_req = REQ_WRITE_INSPECT;
        }
        else
        {
            Send_ReadRequest();   /* 내부에서 s_last_req = REQ_READ 설정 */
        }

        s_last_poll_tick = now;
        s_req_send_tick  = now;
        s_state = ST_WAITING;
        return;
    }

    /* ── ST_WAITING ── */

    /* 1) 응답 프레임이 다 도착했는지: 마지막 바이트 후 5ms 조용하면 완성 */
    if (s_rx_len > 0 && (now - s_last_byte_tick) >= FRAME_GAP_MS)
    {
        uint8_t len = s_rx_len;
        s_rx_len = 0;   /* 버퍼 소비 */

        /* 임시 진단: 응답으로 뭐가 왔는지 그대로 찍어본다 */
        printf("[RS485 RESP] t=%lu len=%u raw:", (unsigned long)now, len);
        for (uint8_t i = 0; i < len; i++) printf(" %02X", s_rx_buf[i]);
        printf("\r\n");

        if (Process_Response(s_rx_buf, len))
        {
            /* 통신 성공 */
            s_link_ok = 1;
            s_fail_count = 0;
            s_write_fail_count = 0;
        }
        else if (s_last_req != REQ_READ)
        {
            /* 쓰기 요청인데 응답이 형식/CRC 오류로 실패 — 재시도 횟수 누적 */
            Give_Up_Stuck_Write();
        }
        /* 성공이든(정상응답) 실패든(형식/CRC오류) 이번 요청은 끝났으니 IDLE로 */
        s_state = ST_IDLE;
        return;
    }

    /* 2) 타임아웃: 정해진 시간 동안 응답이 없으면 실패 처리 */
    if (now - s_req_send_tick >= RESP_TIMEOUT_MS)
    {
        /* 임시 진단: 타임아웃까지 아무 응답도 못 받았음을 명시 */
        printf("[RS485 RESP] t=%lu TIMEOUT (no response)\r\n", (unsigned long)now);
        s_rx_len = 0;
        if (s_fail_count < 255) s_fail_count++;
        if (s_fail_count >= MAX_FAIL_COUNT) s_link_ok = 0;

        /* 쓰기 요청이 타임아웃났으면 재시도 횟수 누적 (읽기는 어차피 다음
           주기에 새로 요청하니 별도 카운트 불필요) */
        if (s_last_req != REQ_READ)
        {
            Give_Up_Stuck_Write();
        }
        s_state = ST_IDLE;
    }
}

/* ── shadow 값 조회 ── */
uint8_t RS485_GetCurrentFloor(void) { return s_current_floor; }
uint8_t RS485_GetTargetFloor(void)  { return s_target_floor;  }
uint8_t RS485_GetElevState(void)    { return s_elev_state;    }
uint8_t RS485_GetEmergency(void)    { return s_emergency;     }
float   RS485_GetTemperature(void)  { return s_temp_x10  / 10.0f; }
float   RS485_GetHumidity(void)     { return s_humid_x10 / 10.0f; }
uint8_t RS485_GetDhtStatus(void)    { return s_dht_status;    }
uint8_t RS485_GetInspection(void)   { return s_inspection;    }
uint16_t RS485_GetErrorCode(void)   { return s_error_code;    }
uint8_t RS485_IsLinkOk(void)        { return s_link_ok;       }

/* ── 쓰기 명령 예약(실제 전송은 다음 폴링 주기에) ── */
void RS485_SetTargetFloor(uint8_t floor)
{
    /* 층 범위(1~3)만 예약 — 슬레이브도 이 범위만 허용 */
    if (floor >= 1 && floor <= 3)
    {
        s_pending_target_value = floor;
        s_pending_write_target = 1;
    }
}

void RS485_ClearEmergency(void)
{
    s_pending_clear_emergency = 1;
}

/* 점검 시작 예약 — 다음 폴링 주기에 REG_INSPECTION에 1을 쓴다 */
void RS485_SetInspection(void)
{
    s_pending_inspect_value = 1;
    s_pending_write_inspect = 1;
}

/* 점검 해제 예약 — 다음 폴링 주기에 REG_INSPECTION에 0을 쓴다 */
void RS485_ClearInspection(void)
{
    s_pending_inspect_value = 0;
    s_pending_write_inspect = 1;
}
