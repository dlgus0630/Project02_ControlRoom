#ifndef __DEV_RTC_H__
#define __DEV_RTC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* DS1302 트리클 충전 RTC. 3선 인터페이스(CE/SCLK/IO)를 일반 GPIO로 비트뱅잉함.
   I2C가 아니라 자체 프로토콜임. 블로킹 방식이지만 매우 짧음: 트랜잭션 하나가
   GPIO 몇 번 토글이 전부라 메인루프에서 호출해도 안전함. */

typedef struct {
    uint8_t sec;    /* 0..59 */
    uint8_t min;    /* 0..59 */
    uint8_t hour;   /* 0..23 (24시간 모드) */
    uint8_t date;   /* 1..31 */
    uint8_t month;  /* 1..12 */
    uint8_t day;    /* 1..7  (요일) */
    uint8_t year;   /* 0..99 (2000년대로 가정) */
} RTC_Time_t;

void DS1302_Init(void);                 /* WP 해제, CH 해제(오실레이터 시작) */
void DS1302_SetTime(const RTC_Time_t *t);
void DS1302_GetTime(RTC_Time_t *t);

#ifdef __cplusplus
}
#endif

#endif /* __DEV_RTC_H__ */
