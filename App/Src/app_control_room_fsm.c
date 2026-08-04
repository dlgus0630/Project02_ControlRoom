#include "app_control_room_fsm.h"
#include "app_rs485_ctrl.h"     /* 엘리베이터 값 조회 / 원격 명령 */
#include "dev_rfid.h"           /* RC522 RFID 카드 리더 */
#include "dev_rtc.h"            /* DS1302 시계 */
#include "dev_rgb_led.h"        /* 상태 표시 RGB LED */
#include "dev_fan.h"            /* 온도 기반 팬 PWM 제어 */
#include "stm32f429i_discovery_lcd.h"   /* LCD 그리기 */
#include "app_kor_glyphs.h"   /* 한글 비트맵 글리프 데이터 */
#include "dev_display_kr.h"   /* 한글 글리프 그리기 헬퍼(KOR_DrawGlyph) */
#include "stm32f429i_discovery_ts.h"    /* 터치스크린 */
#include <stdio.h>              /* snprintf */
#include <string.h>             /* memcmp, strcmp, strcpy */

/* ======================================================
 * [동작 요약]
 *  - CR_STATE_LOCKED   : 잠금화면, "카드 태그" 안내.
 *                        RC522로 카드가 감지되면 UID를 저장하고 AUTH_CHECK로.
 *  - CR_STATE_AUTH_CHECK: 저장한 UID를 허용목록과 대조.
 *                        일치 -> UNLOCKED, 불일치 -> 실패표시(약1초) 후 LOCKED.
 *  - CR_STATE_UNLOCKED : 대시보드(현재/목표층, 상태, 온습도, 통신, 시각).
 *                        아래 터치 버튼으로 목표층 지정/점검 시작.
 *                        비상 or 오류가 뜨면 ALERT로, 점검 모드면 INSPECT로.
 *  - CR_STATE_ALERT    : 대시보드 위에 경고 팝업을 그림. 해제 버튼으로
 *                        비상해제를 예약하고, 정상으로 돌아오면 UNLOCKED로.
 *  - CR_STATE_INSPECT  : 대시보드 위에 점검 팝업을 그림. 점검해제 버튼으로
 *                        점검해제를 예약하고, 해제되면 UNLOCKED로.
 *                        점검 중 비상이 겹치면 ALERT가 우선한다.
 *
 *  모든 처리는 논블로킹이다. HAL_Delay는 절대 쓰지 않고 HAL_GetTick()으로만
 *  시간을 잰다.
 *
 *  [LCD 한글 표시 방법]
 *  "관제실", "잠금", "카드 태그", "비상 상황" 같은 고정 문구는 한글 비트맵
 *  폰트(app_kor_glyphs.h의 KOR_* 글리프)를 KOR_DrawGlyph()로 그린다.
 *  숫자(층/온도/습도)·시각·카드 UID 같은 ASCII 문자열은 BSP 기본 비트맵
 *  폰트(Font8~Font24)를 써서 BSP_LCD_DisplayStringAt()로 그린다.
 *  한 줄에 한글과 ASCII를 나란히 붙일 때는 draw_kor_advance()/
 *  draw_ascii_advance() 헬퍼로 x좌표를 누적해 가며 그린다.
 * ====================================================== */

/* ── 허용된 카드 UID 목록 ──
   실제 사용할 카드를 RC522로 한 번 태그하면, 잠금화면 하단에 그 카드의
   UID가 "UID: XX XX XX XX" 형태로 표시된다(또는 아래 s_last_uid 확인).
   그 값을 여기에 그대로 채워 넣으면 그 카드로 인증이 된다. */
static const uint8_t s_allowed_uids[][4] = {
    {0xC2, 0x6D, 0xAC, 0x1B},   /* 태그해서 확인한 실제 카드 UID (1번 카드) */
    {0x21, 0x5A, 0x8C, 0x4C},   /* 태그해서 확인한 실제 카드 UID (2번 카드) */
};
#define ALLOWED_UID_COUNT  (sizeof(s_allowed_uids) / sizeof(s_allowed_uids[0]))

/* ── 타이밍 상수 ── */
#define AUTH_FAIL_SHOW_MS   1000   /* 인증 실패 문구를 보여줄 시간(약 1초) */
#define INFO_REFRESH_MS      300   /* 대시보드 정보 갱신 주기(깜빡임 방지) */

/* ── 레이아웃 상수 ── */
#define LABEL_GAP             6    /* "라벨 : 값" 에서 콜론과 값 사이 픽셀 간격 */

/* ── 화면 색상 (다크테마, 포맷은 0xFF RR GG BB = ARGB8888, 알파는 항상 FF) ── */
#define COL_BG        0xFF12161F   /* 대시보드 배경: 짙은 남색 계열 다크 */
#define COL_LOCK_BG   0xFF12161F   /* 잠금화면 배경: 대시보드 배경과 동일하게 통일 */
#define COL_TEXT      0xFFE6E9EF   /* 본문 글자: 소프트 화이트 */
#define COL_TITLETX   0xFFEAF6FA   /* 제목바 글자: 밝은 화이트 */
#define COL_WARN      0xFFFF5252   /* 경고색: 밝은 레드 (다크 배경에서 눈에 띔) */
#define COL_BTN       0xFF1E2530   /* 버튼 배경: 대시보드보다 한 톤 밝은 다크 */
#define COL_BTN_LINE  0xFF32404F   /* 버튼 테두리: 뮤트 틸그레이 */

/* ── 상태값 강조 색상(다크테마) ──
   라벨은 COL_TEXT 로 두고, "상태"/"통신" 값 글자만 아래 색으로 강조한다. */
#define COL_ST_WAIT   0xFF8FA6BF   /* 대기: 무채색 블루그레이 */
#define COL_ST_MOVE   0xFF4FC3F7   /* 이동: 밝은 시안블루 */
#define COL_ST_OPEN   0xFF4CD97B   /* 열림: 그린 */
#define COL_ST_INSPECT 0xFF4FC3F7  /* 점검: 밝은 시안블루(점검 팝업 강조색과 통일) */
#define COL_ST_ERROR  0xFFFF6B6B   /* 오류: 레드 */
#define COL_LINK_OK   0xFF4CD97B   /* 통신 정상: 그린 */
#define COL_LINK_LOST 0xFFFF6B6B   /* 통신 두절: 레드 */

/* ── 비상 팝업 배경(다크테마): 짙은 레드 ── */
#define COL_POP_BG    0xFF3D0F0F

/* ── 점검 팝업 전용 색상(다크테마): 비상 팝업의 레드와 확실히 구분되는 블루 계열 ── */
#define COL_INSPECT_BG     0xFF0F1E3D   /* 점검 팝업 배경: 짙은 블루 */
#define COL_INSPECT_ACCENT 0xFF4FC3F7   /* 점검 팝업 강조: 밝은 시안블루("이동" 상태색과 통일) */

/* ── 화면 크기(런타임에 채움; 이 보드는 240x320 세로) ── */
static uint16_t s_w = 240;
static uint16_t s_h = 320;

/* ── 버튼 사각형 좌표 (x, y, w, h) — 240x320 기준 고정 배치 ── */
/* 목표층 지정 버튼 3개 (1층/2층/3층) */
#define FBTN_Y   200
#define FBTN_H    55
#define FBTN_W    70
#define FBTN1_X    8
#define FBTN2_X   85
#define FBTN3_X  162
/* 점검 버튼 */
#define CBTN_X     8
#define CBTN_Y   262
#define CBTN_W   224
#define CBTN_H    48

/* ── 경고 팝업 상자 & 그 안의 해제 버튼 ── */
#define POP_X     20
#define POP_Y     70
#define POP_W    200
#define POP_H    180
#define UBTN_X    40
#define UBTN_Y   190
#define UBTN_W   160
#define UBTN_H    50

/* ── FSM 내부 상태 변수 ── */
static CR_State_t s_state      = CR_STATE_LOCKED;
static CR_State_t s_prev_state = CR_STATE_ALERT;  /* 첫 Update에서 반드시 진입처리 되도록 다른 값 */
static uint8_t    s_first_run  = 1;

/* 방금 태그된 카드 UID(4바이트)와 마지막으로 본 UID 문자열 */
static uint8_t s_card_uid[4] = {0, 0, 0, 0};
static char    s_last_uid[24] = "UID: --";

/* 인증 실패 표시용 타이머 */
static uint32_t s_auth_fail_tick = 0;

/* 대시보드 정보 갱신용 타이머 + 이전 표시값 캐시(바뀐 줄만 다시 그리려고) */
static uint32_t s_info_tick = 0;
static char s_prev_line[6][24] = { "", "", "", "", "", "" };

/* 터치 에지 검출용(직전 프레임에 눌려 있었는지) */
static uint8_t s_prev_touch = 0;

/* 카드 디바운스용. RC522_CheckCard()는 카드를 대고 있는 동안에도 인식이
   순간순간 끊겼다 붙었다 하기 때문에(주로 <100ms 간격), 단순히 "직전
   프레임과 비교"만 해서는 카드를 떼기도 전에 여러 번 반응해버린다.
   그래서 "카드가 CARD_ABSENT_MS 이상 연속으로 안 잡혀야 진짜로 뗀 것"으로
   보고, 그 전까지는 다시 반응하지 않도록 무장 상태(armed)를 둔다. */
static uint8_t  s_card_armed = 1;          /* 1이면 다음 태그에 반응 가능 */
static uint32_t s_card_last_seen_tick = 0; /* 카드가 마지막으로 감지됐던 시각 */
#define CARD_ABSENT_MS  400   /* 이만큼 연속으로 안 잡히면 "뗐다"고 판단 */

/* ======================================================
 * 작은 도우미 함수들
 * ====================================================== */

/* 점(px,py)이 사각형(x,y,w,h) 안에 있으면 1 */
static uint8_t point_in_rect(uint16_t px, uint16_t py,
                             uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (px < x || px >= (x + w)) return 0;
    if (py < y || py >= (y + h)) return 0;
    return 1;
}

/* 버튼의 사각형 틀(배경+테두리)만 그리는 내부 공통 함수 */
static void draw_button_shell(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    BSP_LCD_SetTextColor(COL_BTN);
    BSP_LCD_FillRect(x, y, w, h);
    BSP_LCD_SetTextColor(COL_BTN_LINE);
    BSP_LCD_DrawRect(x, y, w, h);
}

/* 한글 글리프 라벨이 붙은 버튼(틀 + 가운데정렬 한글 라벨) */
static void draw_button_kor(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const KorGlyph_t *glyph)
{
    uint16_t gx, gy;

    draw_button_shell(x, y, w, h);
    gx = x + (w - glyph->width) / 2;
    gy = y + (h - glyph->height) / 2;
    KOR_DrawGlyph(gx, gy, glyph, COL_TEXT, COL_BTN);
}

/* 층 지정 버튼(틀 + "숫자"+"층" 조합 라벨, 가운데정렬) */
static void draw_button_floor(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              uint8_t floor_num)
{
    char digit[4];  /* 층 번호는 1자리지만, snprintf 자름 경고 방지를 위해 여유있게 잡음 */
    uint16_t digit_w, total_w, start_x, mid_y;
    const uint16_t GAP = 4;   /* 숫자와 "층" 글리프 사이 간격(px) */

    draw_button_shell(x, y, w, h);

    /* 층 숫자를 문자열로 만들고 폭을 잰다 */
    snprintf(digit, sizeof(digit), "%u", floor_num);
    BSP_LCD_SetFont(&Font20);
    digit_w = (uint16_t)(BSP_LCD_GetFont()->Width * strlen(digit));
    total_w = (uint16_t)(digit_w + GAP + KOR_FLOOR_UNIT.width);
    start_x = x + (w - total_w) / 2;
    mid_y   = y + (h / 2) - 10;

    /* 숫자(ASCII)를 먼저 그린다 */
    BSP_LCD_SetBackColor(COL_BTN);
    BSP_LCD_SetTextColor(COL_TEXT);
    BSP_LCD_DisplayStringAt(start_x, mid_y, (uint8_t *)digit, LEFT_MODE);

    /* 그 오른쪽에 "층" 한글 글리프를 세로 가운데정렬로 그린다 */
    KOR_DrawGlyph(start_x + digit_w + GAP,
                  y + (h - KOR_FLOOR_UNIT.height) / 2,
                  &KOR_FLOOR_UNIT, COL_TEXT, COL_BTN);

    BSP_LCD_SetFont(&Font16);
}

/* ASCII 문자열을 (x,y)에 그리고, 그려진 폭만큼 오른쪽으로 이동한 다음 x좌표를 돌려준다. */
static uint16_t draw_ascii_advance(uint16_t x, uint16_t y, const char *str,
                                   uint32_t txt, uint32_t bg)
{
    BSP_LCD_SetBackColor(bg);
    BSP_LCD_SetTextColor(txt);
    BSP_LCD_DisplayStringAt(x, y, (uint8_t *)str, LEFT_MODE);
    return (uint16_t)(x + BSP_LCD_GetFont()->Width * strlen(str));
}

/* 한글 글리프를 (x,y)에 그리고, 그려진 폭만큼 오른쪽으로 이동한 다음 x좌표를 돌려준다. */
static uint16_t draw_kor_advance(uint16_t x, uint16_t y, const KorGlyph_t *g,
                                 uint32_t txt, uint32_t bg)
{
    KOR_DrawGlyph(x, y, g, txt, bg);
    return (uint16_t)(x + KOR_GlyphWidth(g));
}

/* 온도/습도 float 값을 "소수1자리" 문자열로 만든다.
   (nano 라이브러리에서 %f 미지원일 수 있어 정수 연산으로 직접 만든다) */
static void format_1dp(float value, const char *unit, char *out, int outsz)
{
    int x10 = (int)(value * 10.0f + 0.5f);   /* 반올림해서 10배 정수로 */
    int whole = x10 / 10;
    int frac  = x10 % 10;
    snprintf(out, outsz, "%d.%d%s", whole, frac, unit);
}

/* 관제실 입장/퇴장 로그를 DS1302 시각과 함께 시리얼로 남긴다.
   (한글 대신 영어로 찍는 이유: 터미널에 따라 한글이 깨져 보일 수 있어서.
   event에는 "ENTER"/"EXIT"가 들어온다.) */
static void log_event(const char *event)
{
    RTC_Time_t t;
    DS1302_GetTime(&t);
    printf("[LOG] %s  20%02u-%02u-%02u %02u:%02u:%02u  UID: %02X %02X %02X %02X\r\n",
           event, t.year, t.month, t.date, t.hour, t.min, t.sec,
           s_card_uid[0], s_card_uid[1], s_card_uid[2], s_card_uid[3]);
}

/* ======================================================
 * 화면 그리기 — 각 상태에 "처음 진입할 때" 한 번 전체를 그린다.
 * ====================================================== */

/* 잠금화면: 다크 배경 + "잠금"/"카드 태그" 안내 + 마지막으로 읽은 UID */
static void draw_locked_screen(void)
{
    BSP_LCD_Clear(COL_LOCK_BG);

    BSP_LCD_SetBackColor(COL_LOCK_BG);
    /* "잠금" (한글 글리프, 가로 가운데정렬) */
    KOR_DrawGlyph((s_w - KOR_LOCKED.width) / 2, 60, &KOR_LOCKED,
                  LCD_COLOR_CYAN, COL_LOCK_BG);

    /* "카드 태그" (한글 글리프, 가로 가운데정렬) */
    KOR_DrawGlyph((s_w - KOR_TAG_CARD.width) / 2, 130, &KOR_TAG_CARD,
                  LCD_COLOR_WHITE, COL_LOCK_BG);

    /* 마지막으로 읽은 카드 UID(허용목록 채울 때 참고용) */
    BSP_LCD_SetTextColor(LCD_COLOR_YELLOW);
    BSP_LCD_SetFont(&Font16);
    BSP_LCD_DisplayStringAt(0, 280, (uint8_t *)s_last_uid, CENTER_MODE);
}

/* 인증 실패 화면(약 1초 표시) */
static void draw_auth_fail_screen(void)
{
    BSP_LCD_Clear(COL_LOCK_BG);
    BSP_LCD_SetBackColor(COL_LOCK_BG);
    /* "인증 실패" (한글 글리프, 가로 가운데정렬) */
    KOR_DrawGlyph((s_w - KOR_AUTH_FAIL.width) / 2, 130, &KOR_AUTH_FAIL,
                  COL_WARN, COL_LOCK_BG);

    BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
    BSP_LCD_SetFont(&Font16);
    BSP_LCD_DisplayStringAt(0, 280, (uint8_t *)s_last_uid, CENTER_MODE);
}

/* 대시보드 고정 틀(제목바 + 버튼들). 정보 텍스트는 별도 갱신함수에서. */
static void draw_dashboard_frame(void)
{
    BSP_LCD_Clear(COL_BG);

    /* 제목바: 배경색 채우기 없이, 글자만 대시보드 배경 위에 그린다. */
    BSP_LCD_SetBackColor(COL_BG);
    /* "관제실" (제목바 영역 안에서 가로/세로 가운데정렬) */
    KOR_DrawGlyph((s_w - KOR_CONTROL_ROOM.width) / 2,
                  (34 - KOR_CONTROL_ROOM.height) / 2,
                  &KOR_CONTROL_ROOM, COL_TITLETX, COL_BG);
    /* 제목바 바로 아래 구분선(밑줄)만 순백색으로 그어서
       대시보드 본문과의 경계를 확실하게 표시한다. */
    BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
    BSP_LCD_DrawHLine(0, 34, s_w);

    /* 목표층 버튼 3개("1층"/"2층"/"3층") + 점검 버튼 */
    draw_button_floor(FBTN1_X, FBTN_Y, FBTN_W, FBTN_H, 1);
    draw_button_floor(FBTN2_X, FBTN_Y, FBTN_W, FBTN_H, 2);
    draw_button_floor(FBTN3_X, FBTN_Y, FBTN_W, FBTN_H, 3);
    draw_button_kor(CBTN_X, CBTN_Y, CBTN_W, CBTN_H, &KOR_INSPECT_BTN);  /* 점검 */

    BSP_LCD_SetFont(&Font16);
}

/* 대시보드 정보 영역 갱신 — 바뀐 줄만 다시 그려 깜빡임을 막는다. */
static void refresh_dashboard_info(uint8_t force)
{
    char line[6][24];
    char tmp[16];
    char temp_txt[16];   /* 온도줄 화면 표출용(오류코드 suffix 없는 순수 "23.5C") */
    uint8_t cur   = RS485_GetCurrentFloor();
    uint8_t tgt   = RS485_GetTargetFloor();
    uint8_t st    = RS485_GetElevState();
    uint8_t link  = RS485_IsLinkOk();
    uint16_t err  = RS485_GetErrorCode();   /* 이 tick의 오류코드 — 아래 스냅샷/렌더링에서 재사용 */
    RTC_Time_t t;
    int i;

    /* 이번 tick의 값들을 문자열 스냅샷으로 만든다.
       주 용도는 "이전 값과 비교해서 바뀐 줄만 다시 그리기"이고,
       습도(line[3])·시각(line[5])처럼 ASCII 그대로인 줄은 화면에도 그대로 쓴다. */
    snprintf(line[0], sizeof(line[0]), "C%u T%u", cur, tgt);   /* 현재/목표층 */
    snprintf(line[1], sizeof(line[1]), "S%u", st);             /* 상태 */
    format_1dp(RS485_GetTemperature(), "C", temp_txt, sizeof(temp_txt));   /* 화면에 찍을 온도값(순수) */
    /* 비교용 스냅샷에는 온도값 뒤에 오류코드도 함께 넣는다(화면엔 안 찍힘).
       경고(101/102/103)가 켜지거나 꺼질 때 온도 숫자가 그대로여도 이 줄을
       다시 그리도록 변경 감지를 트리거하기 위함. */
    snprintf(line[2], sizeof(line[2]), "%s|%u", temp_txt, (unsigned)err);   /* 온도 + 오류코드(비교용) */
    format_1dp(RS485_GetHumidity(), "%", tmp, sizeof(tmp));
    snprintf(line[3], sizeof(line[3]), "%s", tmp);             /* 습도 */
    snprintf(line[4], sizeof(line[4]), "%u", link);            /* 통신 상태 */
    DS1302_GetTime(&t);
    snprintf(line[5], sizeof(line[5]), "%02u:%02u:%02u",
             t.hour, t.min, t.sec);                            /* 현재 시각 */

    BSP_LCD_SetFont(&Font16);

    /* 각 줄을 이전 값과 비교해 바뀐 것만 다시 그림(한글 글리프 + ASCII 숫자 조합) */
    for (i = 0; i < 6; i++)
    {
        uint32_t txt = COL_TEXT;   /* 라벨/일반 값 글자색(다크테마 소프트 화이트) */
        uint16_t x = 8;
        uint16_t y = 44 + (uint16_t)(i * 25);
        char numbuf[16];

        /* 강제 그리기가 아니고 값이 그대로면 그 줄은 건너뛴다 */
        if (!force && strcmp(line[i], s_prev_line[i]) == 0) continue;

        /* 줄 영역 전체를 배경색으로 지운 뒤 통째로 다시 그린다 */
        BSP_LCD_SetTextColor(COL_BG);
        BSP_LCD_FillRect(8, y, s_w - 16, 22);

        switch (i)
        {
        case 0:   /* "현재 : "cur"층"  "목표 : "tgt"층" */
            x = draw_kor_advance(x, y, &KOR_CURRENT_LABEL, txt, COL_BG);
            x += LABEL_GAP;   /* 콜론 뒤 간격 */
            snprintf(numbuf, sizeof(numbuf), "%u", cur);
            x = draw_ascii_advance(x, y, numbuf, txt, COL_BG);
            x = draw_kor_advance(x, y, &KOR_FLOOR_UNIT, txt, COL_BG);
            x += 14;   /* 현재/목표 사이 간격 */
            x = draw_kor_advance(x, y, &KOR_TARGET_LABEL, txt, COL_BG);
            x += LABEL_GAP;   /* 콜론 뒤 간격 */
            snprintf(numbuf, sizeof(numbuf), "%u", tgt);
            x = draw_ascii_advance(x, y, numbuf, txt, COL_BG);
            x = draw_kor_advance(x, y, &KOR_FLOOR_UNIT, txt, COL_BG);
            break;
        case 1:   /* "상태 : " + 상태 한글 (값 글자만 상태별 강조색) */
            x = draw_kor_advance(x, y, &KOR_STATE_LABEL, txt, COL_BG);
            x += LABEL_GAP;   /* 콜론 뒤 간격 */
            if (st == 0) x = draw_kor_advance(x, y, &KOR_STATE_WAIT, COL_ST_WAIT, COL_BG);
            else if (st == 1) x = draw_kor_advance(x, y, &KOR_STATE_MOVE, COL_ST_MOVE, COL_BG);
            else if (st == 2) x = draw_kor_advance(x, y, &KOR_STATE_OPEN, COL_ST_OPEN, COL_BG);
            else if (st == 4) x = draw_kor_advance(x, y, &KOR_STATE_INSPECT, COL_ST_INSPECT, COL_BG);
            else x = draw_kor_advance(x, y, &KOR_STATE_ERROR, COL_ST_ERROR, COL_BG);
            break;
        case 2:   /* "온도 : " + 값(단위 C 포함) + (경고 있으면 문구 추가) */
        {
            x = draw_kor_advance(x, y, &KOR_TEMP_LABEL, txt, COL_BG);
            x += LABEL_GAP;   /* 콜론 뒤 간격 */
            x = draw_ascii_advance(x, y, temp_txt, txt, COL_BG);   /* 순수 온도값(suffix 없음) */
            /* 운행 중 경고(101/102/103)면 온도값 뒤에 경고 문구를 이어붙인다.
               201/301/401/402(잠금)는 여기가 아니라 경고 팝업에서 표시한다. */
            if (err == 101)      { x += LABEL_GAP; x = draw_kor_advance(x, y, &KOR_TEMP_WARN, COL_WARN, COL_BG); }
            else if (err == 102) { x += LABEL_GAP; x = draw_kor_advance(x, y, &KOR_SENSOR_TIMEOUT, COL_WARN, COL_BG); }
            else if (err == 103) { x += LABEL_GAP; x = draw_kor_advance(x, y, &KOR_SENSOR_DATA_ERR, COL_WARN, COL_BG); }
            break;
        }
        case 3:   /* "습도 : " + 값(단위 % 포함) */
            x = draw_kor_advance(x, y, &KOR_HUMI_LABEL, txt, COL_BG);
            x += LABEL_GAP;   /* 콜론 뒤 간격 */
            x = draw_ascii_advance(x, y, line[3], txt, COL_BG);
            break;
        case 4:   /* "통신 정상"(그린) 또는 "통신 두절"(레드) */
            if (link) x = draw_kor_advance(x, y, &KOR_LINK_OK, COL_LINK_OK, COL_BG);
            else x = draw_kor_advance(x, y, &KOR_LINK_LOST, COL_LINK_LOST, COL_BG);
            break;
        case 5:   /* "시각" + 시:분:초 (콜론은 없지만 라벨과 값 사이 간격은 동일하게) */
            x = draw_kor_advance(x, y, &KOR_TIME_LABEL, txt, COL_BG);
            x += LABEL_GAP;
            x = draw_ascii_advance(x, y, line[5], txt, COL_BG);
            break;
        }

        strcpy(s_prev_line[i], line[i]);
    }
}

/* 경고 팝업 상자를 대시보드 위에 그린다. */
static void draw_alert_popup(void)
{
    uint16_t err = RS485_GetErrorCode();   /* 세분화 오류코드(잠금 유형 구분용) */

    /* 상자 배경(짙은 레드, 다크테마) + 두꺼운 테두리 */
    BSP_LCD_SetTextColor(COL_POP_BG);
    BSP_LCD_FillRect(POP_X, POP_Y, POP_W, POP_H);
    BSP_LCD_SetTextColor(COL_WARN);
    BSP_LCD_DrawRect(POP_X,     POP_Y,     POP_W,     POP_H);
    BSP_LCD_DrawRect(POP_X + 1, POP_Y + 1, POP_W - 2, POP_H - 2);
    BSP_LCD_DrawRect(POP_X + 2, POP_Y + 2, POP_W - 4, POP_H - 4);

    BSP_LCD_SetBackColor(COL_POP_BG);
    /* "비상 상황" (제목: 밝은 레드, 가로 가운데정렬) */
    KOR_DrawGlyph((s_w - KOR_EMERGENCY_TITLE.width) / 2, POP_Y + 20,
                  &KOR_EMERGENCY_TITLE, COL_WARN, COL_POP_BG);

    /* 본문 텍스트를 오류코드별로 분기 (모두 가로 가운데정렬, POP_Y+70 위치).
       301=위치 인식 오류, 201=이동 타임아웃, 비상정지(REG_EMERGENCY=1)=비상정지 눌림,
       그 외는 폴백 문구 "엘리베이터 오류"를 띄운다. */
    if (err == 301)
        /* "위치 인식 오류" (본문: 밝은 화이트) */
        KOR_DrawGlyph((s_w - KOR_POS_ERROR.width) / 2, POP_Y + 70,
                      &KOR_POS_ERROR, COL_TITLETX, COL_POP_BG);
    else if (err == 201)
        /* "이동 타임아웃" (본문: 밝은 화이트) */
        KOR_DrawGlyph((s_w - KOR_MOVE_TIMEOUT.width) / 2, POP_Y + 70,
                      &KOR_MOVE_TIMEOUT, COL_TITLETX, COL_POP_BG);
    else if (RS485_GetEmergency() == 1)
        /* "비상정지 눌림" (본문: 밝은 화이트) */
        KOR_DrawGlyph((s_w - KOR_ESTOP_PRESSED.width) / 2, POP_Y + 70,
                      &KOR_ESTOP_PRESSED, COL_TITLETX, COL_POP_BG);
    else
        /* "엘리베이터 오류" (본문: 밝은 화이트, 그 외 폴백) */
        KOR_DrawGlyph((s_w - KOR_ELEVATOR_ERROR.width) / 2, POP_Y + 70,
                      &KOR_ELEVATOR_ERROR, COL_TITLETX, COL_POP_BG);

    /* 해제 버튼: "해제" */
    draw_button_kor(UBTN_X, UBTN_Y, UBTN_W, UBTN_H, &KOR_UNLOCK_BTN);
    BSP_LCD_SetFont(&Font16);
}

/* 점검 팝업 상자를 대시보드 위에 그린다.
   비상 팝업(draw_alert_popup)과 좌표는 같지만, 색을 짙은 레드가 아닌
   블루 계열로 써서 "점검 중"임을 한눈에 구분되게 한다. */
static void draw_inspect_popup(void)
{
    /* 상자 배경(짙은 블루, 다크테마) + 두꺼운 테두리(밝은 시안블루) */
    BSP_LCD_SetTextColor(COL_INSPECT_BG);
    BSP_LCD_FillRect(POP_X, POP_Y, POP_W, POP_H);
    BSP_LCD_SetTextColor(COL_INSPECT_ACCENT);
    BSP_LCD_DrawRect(POP_X,     POP_Y,     POP_W,     POP_H);
    BSP_LCD_DrawRect(POP_X + 1, POP_Y + 1, POP_W - 2, POP_H - 2);
    BSP_LCD_DrawRect(POP_X + 2, POP_Y + 2, POP_W - 4, POP_H - 4);

    BSP_LCD_SetBackColor(COL_INSPECT_BG);
    /* "점검 중" (제목: 밝은 시안블루, 가로 가운데정렬) */
    KOR_DrawGlyph((s_w - KOR_INSPECT_TITLE.width) / 2, POP_Y + 20,
                  &KOR_INSPECT_TITLE, COL_INSPECT_ACCENT, COL_INSPECT_BG);

    /* 해제 버튼: "점검해제" */
    draw_button_kor(UBTN_X, UBTN_Y, UBTN_W, UBTN_H, &KOR_CLEAR_INSPECT_BTN);
    BSP_LCD_SetFont(&Font16);
}

/* ======================================================
 * 터치 처리(에지 검출) — 각 상태에서 눌린 순간 1번만 반응
 * ====================================================== */

/* 대시보드에서 목표층 버튼(1~3층)과 점검 버튼 처리 */
static void handle_dashboard_touch(uint16_t px, uint16_t py)
{
    if (point_in_rect(px, py, FBTN1_X, FBTN_Y, FBTN_W, FBTN_H))
        RS485_SetTargetFloor(1);
    else if (point_in_rect(px, py, FBTN2_X, FBTN_Y, FBTN_W, FBTN_H))
        RS485_SetTargetFloor(2);
    else if (point_in_rect(px, py, FBTN3_X, FBTN_Y, FBTN_W, FBTN_H))
        RS485_SetTargetFloor(3);
    else if (point_in_rect(px, py, CBTN_X, CBTN_Y, CBTN_W, CBTN_H))
        RS485_SetInspection();   /* 점검 시작 예약 */
}

/* ======================================================
 * 공개 API
 * ====================================================== */
void ControlRoom_FSM_Init(void)
{
    /* LCD 초기화 + 배경 레이어 설정 + 화면 켜기 */
    BSP_LCD_Init();

    BSP_LCD_LayerDefaultInit(LCD_BACKGROUND_LAYER, LCD_FRAME_BUFFER);
    BSP_LCD_SelectLayer(LCD_BACKGROUND_LAYER);
    BSP_LCD_DisplayOn();

    s_w = (uint16_t)BSP_LCD_GetXSize();
    s_h = (uint16_t)BSP_LCD_GetYSize();

    /* 터치스크린 초기화(화면 크기 전달) */
    BSP_TS_Init(s_w, s_h);

    /* 주변장치 초기화 */
    RC522_Init();
    DS1302_Init();
    RGB_Init();
    Fan_Init();

    /* 첫 화면: 잠금 */
    s_state      = CR_STATE_LOCKED;
    s_prev_state = CR_STATE_ALERT;   /* 첫 Update에서 진입처리 강제 */
    s_first_run  = 1;
    s_prev_touch = 0;
    s_card_armed = 1;
    s_card_last_seen_tick = HAL_GetTick();
}

void ControlRoom_FSM_Update(void)
{
    TS_StateTypeDef ts;
    uint8_t  touched;
    uint8_t  rising;      /* 이번에 새로 눌린 순간이면 1 */
    uint16_t px = 0, py = 0;
    uint8_t  entering;    /* 이 상태에 처음 진입한 프레임이면 1 */
    uint8_t  card_present;
    uint8_t  card_rising; /* 카드를 새로 갖다댄 순간에만 1 (계속 대고 있으면 0) */
    uint32_t now = HAL_GetTick();

    /* 터치 상태 읽고 에지(rising) 검출 */
    BSP_TS_GetState(&ts);
    touched = (ts.TouchDetected > 0) ? 1 : 0;
    /* 터치 컨트롤러의 Y축이 LCD 화면 방향과 반대로 뒤집혀 있어서
       버튼 히트 판정 전에 세로로 뒤집어 보정한다. */
    if (touched) { px = ts.X; py = (uint16_t)(s_h - ts.Y); }
    rising = (touched && !s_prev_touch) ? 1 : 0;
    s_prev_touch = touched;

    /* RFID 카드 디바운스 — 인식이 순간순간 끊겨도(<CARD_ABSENT_MS) 계속
       대고 있는 것으로 보고, CARD_ABSENT_MS 이상 연속으로 안 잡혀야
       진짜로 뗀 것으로 보고 다음 태그에 반응할 수 있게(armed) 만든다. */
    card_present = (RC522_CheckCard(s_card_uid) > 0) ? 1 : 0;
    if (card_present)
    {
        s_card_last_seen_tick = now;
    }
    else if (!s_card_armed && (now - s_card_last_seen_tick >= CARD_ABSENT_MS))
    {
        s_card_armed = 1;   /* 충분히 오래 안 잡혔음 -> 카드가 진짜로 뗀 것으로 판단 */
    }
    card_rising = (card_present && s_card_armed) ? 1 : 0;
    if (card_rising)
    {
        s_card_armed = 0;   /* 이번 태그는 이미 처리 -> 뗄 때까지 다시 반응 안 함 */
    }

    /* 상태 진입 감지 */
    entering = (s_state != s_prev_state || s_first_run) ? 1 : 0;
    s_prev_state = s_state;
    s_first_run  = 0;

    switch (s_state)
    {
    /* ── 잠금: RFID 대기 ── */
    case CR_STATE_LOCKED:
        if (entering)
        {
            draw_locked_screen();
            RGB_SetColor(RGB_BLUE);   /* 잠금 상태는 파랑(해제 상태의 초록과 구분) */
        }
        /* 카드를 새로 갖다댄 순간에만 반응(계속 대고 있어도 한 번만) */
        if (card_rising)
        {
            /* 참고용으로 UID 문자열 저장(잠금화면 하단에 표시됨) */
            snprintf(s_last_uid, sizeof(s_last_uid), "UID: %02X %02X %02X %02X",
                     s_card_uid[0], s_card_uid[1], s_card_uid[2], s_card_uid[3]);
            s_state = CR_STATE_AUTH_CHECK;
        }
        break;

    /* ── 인증 대조(짧은 전환 상태) ── */
    case CR_STATE_AUTH_CHECK:
        if (entering)
        {
            uint8_t matched = 0;
            RGB_SetColor(RGB_BLUE);
            for (uint32_t i = 0; i < ALLOWED_UID_COUNT; i++)
            {
                if (memcmp(s_card_uid, s_allowed_uids[i], 4) == 0)
                {
                    matched = 1;
                    break;
                }
            }
            if (matched)
            {
                /* 인증 성공 -> 입장 로그 남기고 대시보드로 */
                log_event("ENTER");
                s_state = CR_STATE_UNLOCKED;
            }
            else
            {
                /* 인증 실패 -> 실패문구 표시 후 타이머 시작 */
                draw_auth_fail_screen();
                s_auth_fail_tick = now;
            }
        }
        else
        {
            /* 실패 표시를 약 1초 유지한 뒤 잠금화면으로 복귀 */
            if (now - s_auth_fail_tick >= AUTH_FAIL_SHOW_MS)
                s_state = CR_STATE_LOCKED;
        }
        break;

    /* ── 해제: 모니터링/원격제어 대시보드 ── */
    case CR_STATE_UNLOCKED:
        if (entering)
        {
            draw_dashboard_frame();
            refresh_dashboard_info(1);   /* 전체 강제 그리기 */
            s_info_tick = now;
            /* 링크 상태로 색 결정: 엘리베이터 통신 정상이면 초록, 두절이면 흰색 */
            RGB_SetColor(RS485_IsLinkOk() ? RGB_GREEN : RGB_WHITE);
        }

        /* 비상 또는 오류가 오면 경고 상태로 전환 */
        if (RS485_GetEmergency() == 1 || RS485_GetElevState() == 3)
        {
            s_state = CR_STATE_ALERT;
            break;
        }

        /* 점검 모드 수신 -> 점검 팝업으로 전환 */
        if (RS485_GetInspection() == 1)
        {
            s_state = CR_STATE_INSPECT;
            break;
        }

        /* 카드를 새로 갖다댄 순간에만 반응 -> 등록된 카드면 퇴장 로그 남기고 잠금화면으로 복귀 */
        if (card_rising)
        {
            uint8_t matched = 0;
            for (uint32_t i = 0; i < ALLOWED_UID_COUNT; i++)
            {
                if (memcmp(s_card_uid, s_allowed_uids[i], 4) == 0)
                {
                    matched = 1;
                    break;
                }
            }
            if (matched)
            {
                log_event("EXIT");
                s_state = CR_STATE_LOCKED;
                break;
            }
        }

        /* 버튼 터치 처리(눌린 순간 1번만) */
        if (rising)
            handle_dashboard_touch(px, py);

        /* 정보 주기 갱신(바뀐 줄만) */
        if (now - s_info_tick >= INFO_REFRESH_MS)
        {
            refresh_dashboard_info(0);
            /* 대시보드에 머무는 동안에도 링크 상태로 색을 갱신: 통신 두절되면 흰색으로 즉시 반영 */
            RGB_SetColor(RS485_IsLinkOk() ? RGB_GREEN : RGB_WHITE);
            s_info_tick = now;
        }
        break;

    /* ── 경고 팝업 ── */
    case CR_STATE_ALERT:
        if (entering)
        {
            draw_alert_popup();
            RGB_SetColor(RGB_RED);
        }

        /* 해제 버튼 터치 -> 비상해제 명령 예약 */
        if (rising && point_in_rect(px, py, UBTN_X, UBTN_Y, UBTN_W, UBTN_H))
            RS485_ClearEmergency();

        /* 비상/오류가 모두 정상으로 돌아오면 대시보드로 복귀 */
        if (RS485_GetEmergency() == 0 && RS485_GetElevState() != 3)
            s_state = CR_STATE_UNLOCKED;
        break;

    /* ── 점검 팝업 ── */
    case CR_STATE_INSPECT:
        if (entering)
        {
            draw_inspect_popup();
            RGB_SetColor(RGB_YELLOW);
        }

        /* 점검해제 버튼 터치 -> 점검해제 명령 예약 */
        if (rising && point_in_rect(px, py, UBTN_X, UBTN_Y, UBTN_W, UBTN_H))
            RS485_ClearInspection();

        /* 점검 해제되고 비상/오류도 없으면 대시보드로 복귀 */
        if (RS485_GetInspection() == 0 && RS485_GetEmergency() == 0 && RS485_GetElevState() != 3)
            s_state = CR_STATE_UNLOCKED;
        /* 점검 도중 실제 비상이 겹치면 경고 팝업 우선 */
        else if (RS485_GetEmergency() == 1 || RS485_GetElevState() == 3)
            s_state = CR_STATE_ALERT;
        break;

    default:
        s_state = CR_STATE_LOCKED;
        break;
    }

    /* 온도 기반 팬 PWM 갱신(논블로킹) */
    Fan_Update();
}
