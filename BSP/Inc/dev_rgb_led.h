#ifndef __DEV_RGB_LED_H__
#define __DEV_RGB_LED_H__

#ifdef __cplusplus
extern "C" {
#endif

/* RGB 상태 표시 LED 드라이버 (PB3=빨강, PB4=초록, PB7=파랑, 논블로킹) */

typedef enum {
    RGB_OFF = 0,
    RGB_BLUE,    /* 잠금(카드 태그 대기) / 인증 대조 중 */
    RGB_GREEN,   /* 관리실 정상(엘리베이터 통신 정상) */
    RGB_YELLOW,  /* 점검 팝업(INSPECT)                */
    RGB_WHITE,   /* 관리실인데 엘리베이터 통신 두절(R+G+B 전부 켬) */
    RGB_RED      /* 엘리베이터 오류 / 비상            */
} RGB_Color_t;

void RGB_Init(void);
void RGB_SetColor(RGB_Color_t color); /* 지속되는 기본 색상 설정 */

#ifdef __cplusplus
}
#endif

#endif /* __DEV_RGB_LED_H__ */
