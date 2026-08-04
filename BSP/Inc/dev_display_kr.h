#ifndef DEV_DISPLAY_KR_H
#define DEV_DISPLAY_KR_H

/* 한글 글리프를 LCD에 그려 주는 헬퍼.
   글리프 데이터 정의는 app_kor_glyphs에, 실제 픽셀 찍기는
   기존 BSP LCD 드라이버(BSP_LCD_DrawPixel)에 의존한다. */

#include <stdint.h>
#include "app_kor_glyphs.h"
#include "stm32f429i_discovery_lcd.h"

/* (x,y) 위치를 좌상단으로 삼아 한글 비트맵을 그린다.
   비트=1 픽셀은 fgColor로, 비트=0 픽셀은 bgColor로 채운다(배경까지 덮어쓰므로
   따로 지우지 않아도 글자 갱신이 가능하다). */
void KOR_DrawGlyph(uint16_t x, uint16_t y, const KorGlyph_t *glyph,
                   uint32_t fgColor, uint32_t bgColor);

/* 글리프 폭을 돌려준다(여러 글자를 가로로 이어붙일 때 사용). */
uint16_t KOR_GlyphWidth(const KorGlyph_t *glyph);

#endif /* DEV_DISPLAY_KR_H */
