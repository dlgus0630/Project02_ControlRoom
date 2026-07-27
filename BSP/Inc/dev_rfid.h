#ifndef __DEV_RFID_H__
#define __DEV_RFID_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* MFRC522 (RC522) 13.56MHz RFID 리더, SPI4로 통신하며 CS/RST는 일반 GPIO로 제어.
   논블로킹 스타일: CheckCard는 짧은 동기식 SPI 폴링으로, 메인 루프에서
   호출하도록 만들어짐. */

void    RC522_Init(void);                 /* 리셋 해제, 소프트 리셋, 레지스터 초기화, 안테나 ON */
uint8_t RC522_CheckCard(uint8_t *uidBuf); /* UID 4바이트를 채우고 UID 길이 반환; 카드 없으면 0 */

#ifdef __cplusplus
}
#endif

#endif /* __DEV_RFID_H__ */
