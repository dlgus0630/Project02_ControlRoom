#ifndef __DEV_FAN_H__
#define __DEV_FAN_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 온도 기반 DC 팬 PWM 제어 드라이버 (TIM10_CH1 = PF6 출력, 논블로킹 동작)
   자세한 제어 규칙(온도 구간표, 고온 래치, 킥스타트)은 dev_fan.c 상단 주석 참고. */

void Fan_Init(void);    /* 팬 PWM 출력 시작 + 내부 상태 초기화 (부팅 시 한 번만 호출) */
void Fan_Update(void);  /* 메인루프에서 매 tick 호출 — 온도를 읽어 duty를 갱신 (논블로킹) */

#ifdef __cplusplus
}
#endif

#endif /* __DEV_FAN_H__ */
