#ifndef APP_KOR_GLYPHS_H
#define APP_KOR_GLYPHS_H

#include <stdint.h>

/* 한글 비트맵 글리프 구조체.
   bitmap: 1비트 팩킹(행 stride=(width+7)/8, MSB=왼쪽픽셀). */
typedef struct {
    uint16_t       width;
    uint16_t       height;
    const uint8_t *bitmap;
} KorGlyph_t;

extern const KorGlyph_t KOR_LOCKED;   /* "잠금" */
extern const KorGlyph_t KOR_TAG_CARD;   /* "카드를 태그하세요" */
extern const KorGlyph_t KOR_AUTH_FAIL;   /* "인증 실패" */
extern const KorGlyph_t KOR_CONTROL_ROOM;   /* "관제실" */
extern const KorGlyph_t KOR_CURRENT_LABEL;   /* "현재" */
extern const KorGlyph_t KOR_FLOOR_UNIT;   /* "층" */
extern const KorGlyph_t KOR_TARGET_LABEL;   /* "목표" */
extern const KorGlyph_t KOR_STATE_LABEL;   /* "상태" */
extern const KorGlyph_t KOR_STATE_WAIT;   /* "대기" */
extern const KorGlyph_t KOR_STATE_MOVE;   /* "이동" */
extern const KorGlyph_t KOR_STATE_OPEN;   /* "열림" */
extern const KorGlyph_t KOR_STATE_ERROR;   /* "오류" */
extern const KorGlyph_t KOR_TEMP_LABEL;   /* "온도" */
extern const KorGlyph_t KOR_HUMI_LABEL;   /* "습도" */
extern const KorGlyph_t KOR_LINK_OK;   /* "통신 정상" */
extern const KorGlyph_t KOR_LINK_LOST;   /* "통신 두절" */
extern const KorGlyph_t KOR_TIME_LABEL;   /* "시각" */
extern const KorGlyph_t KOR_EMERGENCY_TITLE;   /* "비상 상황" */
extern const KorGlyph_t KOR_ESTOP_PRESSED;   /* "비상정지 눌림" */
extern const KorGlyph_t KOR_ELEVATOR_ERROR;   /* "엘리베이터 오류" */
extern const KorGlyph_t KOR_UNLOCK_BTN;   /* "해제" */
extern const KorGlyph_t KOR_INSPECT_BTN;   /* "점검" */
extern const KorGlyph_t KOR_STATE_INSPECT;   /* "점검" */
extern const KorGlyph_t KOR_CLEAR_INSPECT_BTN;   /* "점검해제" */
extern const KorGlyph_t KOR_INSPECT_TITLE;   /* "점검 중" */
extern const KorGlyph_t KOR_POS_ERROR;   /* "위치 인식 오류" */
extern const KorGlyph_t KOR_MOVE_TIMEOUT;   /* "이동 타임아웃" */
extern const KorGlyph_t KOR_TEMP_WARN;   /* "고온 경고" */
extern const KorGlyph_t KOR_SENSOR_TIMEOUT;   /* "센서 타임아웃" */
extern const KorGlyph_t KOR_SENSOR_DATA_ERR;   /* "센서 데이터 오류" */

#endif /* APP_KOR_GLYPHS_H */
