#ifndef APP_RS485_CTRL_H_
#define APP_RS485_CTRL_H_

#include "main.h"

/* ======================================================
 * Modbus RTU 마스터 (관리실 = F429)
 * 통신: UART5 (PC12=TX, PD2=RX), 115200bps
 * 역할: 슬레이브(엘리베이터 카)에게 주기적으로 요청을 보내고
 *       응답을 받아서 화면에 보여줄 값들을 보관(shadow)한다.
 * 지원 기능코드: 0x03 (레지스터 읽기), 0x06 (단일 레지스터 쓰기)
 * ====================================================== */

#define MODBUS_SLAVE_ID   1   /* 통신 상대(엘리베이터 카)의 Modbus 주소 */

/* ── 레지스터 주소 (슬레이브와 반드시 동일하게) ── */
typedef enum {
    REG_CURRENT_FLOOR   = 0,   /* 현재층            (R)                    */
    REG_TARGET_FLOOR    = 1,   /* 목표층            (R/W)                  */
    REG_PENDING_ACTION  = 2,   /* 진행 단계(FSM상태)(R)                    */
    REG_EMERGENCY       = 3,   /* 비상정지 상태     (R/W, 0=정상)          */
    REG_TEMPERATURE_X10 = 4,   /* 온도*10           (R)  예: 23.5도 -> 235 */
    REG_HUMIDITY_X10    = 5,   /* 습도*10           (R)                    */
    REG_DHT_STATUS      = 6,   /* DHT 상태          (R)                    */
    REG_INSPECTION      = 7,   /* 점검 모드 상태     (R/W, 0=정상, 1=점검중) */
    REG_ERROR_CODE      = 8,   /* 통합 오류코드      (R) — 0/101/102/103/201/301/401/402 */
} ModbusReg_t;

/* ── 공개 API ── */
void RS485_Init(void);      /* 한 번 호출 — UART5 수신 인터럽트 시작 */
void RS485_Update(void);    /* 메인루프에서 매번 호출 — 폴링 상태머신 처리 */

/* 최근에 슬레이브에서 읽어온 값들 (읽기 전용 조회) */
uint8_t  RS485_GetCurrentFloor(void);
uint8_t  RS485_GetTargetFloor(void);
uint8_t  RS485_GetElevState(void);    /* REG_PENDING_ACTION = 엘리베이터 FSM 상태 */
uint8_t  RS485_GetEmergency(void);
float    RS485_GetTemperature(void);  /* temp_x10 / 10.0f */
float    RS485_GetHumidity(void);     /* humid_x10 / 10.0f */
uint8_t  RS485_GetDhtStatus(void);
uint8_t  RS485_GetInspection(void);   /* 점검 모드 상태(0=정상, 1=점검중) */
uint16_t RS485_GetErrorCode(void);    /* 통합 오류코드(0/101/102/103/201/301/401/402) */
uint8_t  RS485_IsLinkOk(void);        /* 최근 폴링 성공 1, 연속실패면 0 */

/* 슬레이브에 명령을 보내달라고 예약 (다음 폴링 주기에 실제 전송) */
void RS485_SetTargetFloor(uint8_t floor);  /* 목표층 쓰기 예약 */
void RS485_ClearEmergency(void);           /* 비상해제 쓰기 예약 */
void RS485_SetInspection(void);            /* 점검 시작 쓰기 예약(값=1) */
void RS485_ClearInspection(void);          /* 점검 해제 쓰기 예약(값=0) */

#endif /* APP_RS485_CTRL_H_ */
