/* 한글 글리프를 LCD 에 그려 주는 헬퍼 구현.
   글리프는 1비트 팩킹 비트맵이다:
     - 행 stride = (width + 7) / 8 바이트
     - 각 바이트의 MSB(0x80) 가 왼쪽 픽셀
     - 비트=1 이 글자 픽셀, 비트=0 이 배경 픽셀 */

#include "dev_display_kr.h"

void KOR_DrawGlyph(uint16_t x, uint16_t y, const KorGlyph_t *glyph,
                   uint32_t fgColor, uint32_t bgColor)
{
    uint16_t stride;
    uint16_t row;
    uint16_t col;

    /* 방어 코드: 글리프나 비트맵이 없으면 아무것도 하지 않는다. */
    if (glyph == 0)
    {
        return;
    }
    if (glyph->bitmap == 0)
    {
        return;
    }

    /* 한 행이 차지하는 바이트 수(바이트 경계로 패딩됨). */
    stride = (glyph->width + 7) / 8;

    /* 각 픽셀을 순회하며 켜짐/꺼짐에 따라 색을 찍는다. */
    for (row = 0; row < glyph->height; row++)
    {
        for (col = 0; col < glyph->width; col++)
        {
            uint16_t byte_index;
            uint8_t  bit_mask;
            uint8_t  byte_val;

            /* (row, col) 픽셀이 들어있는 바이트 위치와 비트 위치 계산. */
            byte_index = (row * stride) + (col / 8);
            bit_mask   = (uint8_t)(0x80 >> (col % 8)); /* MSB 가 왼쪽 픽셀 */
            byte_val   = glyph->bitmap[byte_index];

            if ((byte_val & bit_mask) != 0)
            {
                /* 비트=1 -> 글자 픽셀 */
                BSP_LCD_DrawPixel((uint16_t)(x + col), (uint16_t)(y + row), fgColor);
            }
            else
            {
                /* 비트=0 -> 배경 픽셀 */
                BSP_LCD_DrawPixel((uint16_t)(x + col), (uint16_t)(y + row), bgColor);
            }
        }
    }
}

uint16_t KOR_GlyphWidth(const KorGlyph_t *glyph)
{
    /* 방어 코드: 글리프가 없으면 폭 0. */
    if (glyph == 0)
    {
        return 0;
    }
    return glyph->width;
}
