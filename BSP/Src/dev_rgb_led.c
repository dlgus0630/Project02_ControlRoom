#include "dev_rgb_led.h"
#include "main.h"   /* RGB_Red/Green/Blue 핀 + 포트 매크로, HAL_GetTick */

/* 보드가 LOW 레벨로 LED를 켜는 방식(공통 애노드)이면 1로 설정. */
#define RGB_ACTIVE_LOW   0

/* 기본색 = 지속 상태(예: 대기 시 초록, 오류 시 빨강). */
static RGB_Color_t s_base = RGB_OFF;

/* 한 채널을 구동. 배선이 액티브 로우면 레벨을 반전시킴. */
static void set_pin(GPIO_TypeDef *port, uint16_t pin, uint8_t on)
{
#if RGB_ACTIVE_LOW
    HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
    HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#endif
}

/* 색상을 세 개의 GPIO 채널에 바로 반영. */
static void apply_color(RGB_Color_t color)
{
    uint8_t r = 0, g = 0, b = 0;

    switch (color)
    {
        case RGB_BLUE:   b = 1;         break;   /* 파랑만 켬 */
        case RGB_GREEN:  g = 1;         break;
        case RGB_YELLOW: r = 1; g = 1;  break;   /* 빨강 + 초록 = 노랑 */
        case RGB_RED:    r = 1;         break;
        case RGB_WHITE:  r = 1; g = 1; b = 1; break;   /* 빨강+초록+파랑 = 흰색 */
        case RGB_OFF:
        default:         break;   /* 전부 끔 */
    }

    set_pin(RGB_Red_GPIO_Port,   RGB_Red_Pin,   r);
    set_pin(RGB_Green_GPIO_Port, RGB_Green_Pin, g);
    set_pin(RGB_Blue_GPIO_Port,  RGB_Blue_Pin,  b);
}

void RGB_Init(void)
{
    s_base = RGB_OFF;
    apply_color(RGB_OFF);
}

void RGB_SetColor(RGB_Color_t color)
{
    /* 요청한 색을 기본색으로 저장하고 즉시 반영. */
    s_base = color;
    apply_color(color);
}
