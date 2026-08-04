#ifndef __DEV_RTC_H__
#define __DEV_RTC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* DS1302 실시간 시계(RTC) 드라이버.
   CE/SCLK/IO 3가닥 신호선을 전용 통신 주변장치 없이 일반 GPIO를 직접 토글해서
   구현한다(비트뱅잉). I2C나 SPI가 아니라 DS1302 고유의 프로토콜이다.
   함수가 끝날 때까지 기다리는 블로킹 방식이지만, 한 번의 통신이 GPIO를 몇 번
   토글하는 정도라 매우 짧아 메인루프에서 그대로 호출해도 무방하다. */

typedef struct {
    uint8_t sec;    /* 0..59 */
    uint8_t min;    /* 0..59 */
    uint8_t hour;   /* 0..23 (24시간 모드) */
    uint8_t date;   /* 1..31 */
    uint8_t month;  /* 1..12 */
    uint8_t day;    /* 1..7  (요일) */
    uint8_t year;   /* 0..99 (2000년대로 가정) */
} RTC_Time_t;

void DS1302_Init(void);                    /* 쓰기 보호(WP) 해제 + 발진기 기동(CH 해제) */
void DS1302_SetTime(const RTC_Time_t *t);  /* 시각 설정 (t가 NULL이면 아무것도 하지 않음) */
void DS1302_GetTime(RTC_Time_t *t);        /* 현재 시각 읽기 (t가 NULL이면 아무것도 하지 않음) */

#ifdef __cplusplus
}
#endif

#endif /* __DEV_RTC_H__ */
