#include "dev_fan.h"          /* 이 모듈의 공개 함수 선언 */
#include "tim.h"              /* htim10 핸들 (TIM10_CH1 = PF6 PWM 출력) */
#include "app_rs485_ctrl.h"  /* RS485_GetTemperature() 온도 / RS485_GetErrorCode() 오류코드 조회 */

/* ======================================================
 * 온도 기반 DC 팬 PWM 제어 드라이버
 *  - 출력: TIM10_CH1 (PF6). Period=999 이므로 CCR 1000 = 완전 100% ON.
 *  - RS485(Modbus)로 카에서 읽어온 온도(℃)에 따라 팬 duty(%)를 정한다.
 *  - 모든 처리는 논블로킹(HAL_Delay 미사용). 메인루프에서 매 tick Fan_Update() 호출.
 *
 *  [고온 경보 히스테리시스 래치가 필요한 이유]
 *  단순 구간표만 쓰면 온도가 경계값(예: 30.0℃) 근처에서 미세하게 오르내릴 때
 *  duty가 60% <-> 100% 사이를 계속 널뛰기(채터링)한다. 이를 막기 위해
 *  "31℃ 이상이면 100% 래치를 걸고, 28℃ 이하로 충분히 떨어져야 래치를 푼다"는
 *  히스테리시스(켜는 기준과 끄는 기준을 다르게 둠)를 적용한다. 래치가 걸려 있는
 *  동안에는 구간표를 무시하고 무조건 100%로 돌려 확실히 식힌다.
 *
 *  [표시 오류코드(REG_ERROR_CODE)가 아니라 원시 온도로 재계산하는 이유]
 *  REG_ERROR_CODE 는 여러 상황(비상정지/통신오류/센서오류 등)을 우선순위에 따라
 *  하나만 덮어써서 보여주는 "표시용" 값이다. 따라서 실제 온도가 31℃ 이상이어도
 *  더 높은 우선순위의 다른 코드가 표시 중이면 고온이 코드로 안 보일 수 있다.
 *  팬은 화면 표시와 무관하게 실제 온도를 따라 돌아야 하므로, 여기서는 표시 코드에
 *  의존하지 않고 RS485_GetTemperature()로 얻은 원시 온도로 직접 duty를 계산한다.
 *  (단, 물리 비상정지 401만은 화재 확산 방지를 위해 온도보다 우선해 팬을 끈다.)
 *
 *  [킥스타트(기동 부스트)가 필요한 이유]
 *  DC 모터/팬은 완전히 멈춘 상태에서 다시 돌기 시작할 때(정지마찰을 이겨내는 순간)
 *  계속 돌고 있을 때보다 훨씬 큰 토크가 필요하다. 예를 들어 30% duty는 "이미 돌고
 *  있는" 팬을 유지하기엔 충분해도, "멈춰있던" 팬을 처음 돌리기엔 부족해서 로터가
 *  힘만 받고 못 움직인 채 "삐-" 소리(진동)만 내는 경우가 많다. 이를 막기 위해
 *  0% -> 0%가 아닌 값으로 넘어가는 순간에는 잠깐(FAN_KICKSTART_MS) 무조건 100%로
 *  힘차게 돌려서 회전을 시작시킨 뒤, 그 다음부터 원래 목표 duty로 낮춘다.
 * ====================================================== */

#define FAN_KICKSTART_MS   400   /* 정지 상태에서 기동할 때 100% 강제 구동 시간(ms) */

/* 고온 경보 래치: 1이면 온도가 충분히 내려갈 때까지 무조건 100% 유지. */
static uint8_t  s_high_temp_latch = 0;

/* 직전 tick의 "목표" duty(%) — 0에서 벗어나는 순간(=기동 시작)을 감지하는 용도.
   0으로 시작해야 부팅 직후 첫 기동 때도 킥스타트가 정상적으로 걸린다. */
static uint16_t s_prev_target_duty = 0;

/* 직전에 실제로 CCR에 반영한 duty(%) — 값이 안 바뀌면 CCR 갱신을 생략(선택적 최적화). */
static uint16_t s_prev_applied_duty = 0xFFFF;

/* 킥스타트 종료 예정 시각(0이면 킥스타트 비활성 상태). */
static uint32_t s_kickstart_end_tick = 0;

void Fan_Init(void)
{
    /* TIM10 CH1 PWM 출력 시작 (초기 CCR=0 이라 팬은 정지 상태에서 시작). */
    HAL_TIM_PWM_Start(&htim10, TIM_CHANNEL_1);

    /* 내부 상태 초기화: 래치 해제, 직전 duty 무효값, 킥스타트 비활성. */
    s_high_temp_latch    = 0;
    s_prev_target_duty   = 0;
    s_prev_applied_duty  = 0xFFFF;
    s_kickstart_end_tick = 0;
}

void Fan_Update(void)
{
    float    temp;
    uint16_t target_duty;   /* 온도/래치로 결정되는 "원래" 목표 duty(%) */
    uint16_t apply_duty;    /* 이번 tick에 실제로 CCR에 반영할 duty(%, 킥스타트 중엔 100%) */
    uint32_t pulse;

    /* ── 1) 킬스위치(최우선): 물리 비상정지(오류코드 401)면 팬을 즉시 끈다. ──
       화재 확산 방지 목적. 이때는 고온 경보 래치와 무관하게 무조건 0%로 강제한다. */
    if (RS485_GetErrorCode() == 401)
    {
        __HAL_TIM_SET_COMPARE(&htim10, TIM_CHANNEL_1, 0);
        s_prev_target_duty   = 0;
        s_prev_applied_duty  = 0;
        s_kickstart_end_tick = 0;
        return;
    }

    /* ── 2) 원시 온도(℃) 읽기 ── */
    temp = RS485_GetTemperature();

    /* ── 3) 고온 경보 히스테리시스 래치 갱신 ──
       31.0℃ 이상 -> 래치 세팅, 28.0℃ 이하 -> 래치 해제,
       그 사이 구간에서는 현재 래치 상태를 그대로 유지(채터링 방지). */
    if (temp >= 31.0f)
        s_high_temp_latch = 1;
    else if (temp <= 28.0f)
        s_high_temp_latch = 0;
    /* 28.0f 초과 ~ 31.0f 미만: 래치 상태 유지 (아무것도 하지 않음) */

    /* ── 4) 목표 duty(%) 결정 ── */
    if (s_high_temp_latch)
    {
        /* 래치가 걸려 있으면 구간표를 무시하고 무조건 100%로 확실히 식힌다. */
        target_duty = 100;
    }
    else
    {
        /* 래치 해제 상태: 온도 구간표대로 duty 결정.
           <26.0 -> 0%, 26.0~27.9 -> 30%, 28.0~29.9 -> 60%, >=30.0 -> 100% */
        if (temp < 26.0f)
            target_duty = 0;
        else if (temp < 28.0f)
            target_duty = 30;
        else if (temp < 30.0f)
            target_duty = 60;
        else
            target_duty = 100;
    }

    /* ── 5) 킥스타트 적용 여부 판단 ──
       직전엔 꺼져 있었는데(s_prev_target_duty==0) 이번에 목표가 0이 아니게 됐으면,
       정지마찰을 이기도록 잠깐 100%로 힘차게 돌리는 킥스타트를 새로 시작한다. */
    if (target_duty == 0)
    {
        s_kickstart_end_tick = 0;   /* 목표가 정지면 킥스타트 진행 중이었어도 취소 */
        apply_duty = 0;
    }
    else
    {
        if (s_prev_target_duty == 0 && s_kickstart_end_tick == 0)
            s_kickstart_end_tick = HAL_GetTick() + FAN_KICKSTART_MS;   /* 킥스타트 시작 */

        if (s_kickstart_end_tick != 0 && HAL_GetTick() < s_kickstart_end_tick)
            apply_duty = 100;                  /* 킥스타트 구간: 무조건 100% */
        else
        {
            s_kickstart_end_tick = 0;          /* 킥스타트 종료 */
            apply_duty = target_duty;          /* 원래 목표 duty로 정착 */
        }
    }
    s_prev_target_duty = target_duty;

    /* ── 6) duty(%)를 CCR 값으로 변환 ──
       Period=999 이므로 duty*10 -> 0(0%)~1000(100% 완전 ON). */
    pulse = (uint32_t)apply_duty * 10;

    /* ── 7) 직전과 실제로 반영했던 duty가 같으면 CCR 갱신 생략(선택적 최적화). ── */
    if (apply_duty != s_prev_applied_duty)
    {
        __HAL_TIM_SET_COMPARE(&htim10, TIM_CHANNEL_1, pulse);
        s_prev_applied_duty = apply_duty;
    }
}
