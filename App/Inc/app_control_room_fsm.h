#ifndef APP_CONTROL_ROOM_FSM_H_
#define APP_CONTROL_ROOM_FSM_H_

/* ======================================================
 * 관제실(F429 디스커버리) 메인 FSM
 * - LCD/터치스크린으로 엘리베이터를 모니터링하고 원격 제어한다.
 * - RFID(RC522) 카드로 관리자 인증(잠금/해제)을 한다.
 * - 엘리베이터 오류/비상 상황이 오면 경고 팝업을 띄운다.
 * - 점검 모드가 걸리면 점검 팝업을 띄우고 화면에서 점검 해제를 지시할 수 있다.
 *
 * 공개 상태는 아래 5가지만 노출한다.
 * (세부 타이밍/서브단계는 .c 내부 static 변수로 숨긴다)
 * ====================================================== */

typedef enum {
    CR_STATE_LOCKED = 0,   /* 잠금 화면, RFID 인증 대기 */
    CR_STATE_AUTH_CHECK,   /* 카드 태그됨, 인증 목록 대조 중 (짧은 전환 상태) */
    CR_STATE_UNLOCKED,     /* 인증 성공, 모니터링/원격제어 대시보드 */
    CR_STATE_ALERT,        /* 엘리베이터 오류 수신, 팝업 표시 */
    CR_STATE_INSPECT       /* 점검 모드 수신, 점검 팝업 표시 */
} CR_State_t;

void ControlRoom_FSM_Init(void);    /* LCD/TS/RFID/RTC/RGB 초기화 + 첫 화면 그리기 */
void ControlRoom_FSM_Update(void);  /* 메인루프에서 매번 호출 */

#endif /* APP_CONTROL_ROOM_FSM_H_ */
