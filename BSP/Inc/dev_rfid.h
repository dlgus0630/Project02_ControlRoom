#ifndef __DEV_RFID_H__
#define __DEV_RFID_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* MFRC522 (RC522) 13.56MHz RFID 리더. SPI4로 통신하고 CS/RST는 일반 GPIO로 직접 제어함.
   블로킹 방식이지만 매우 짧음: CheckCard 한 번은 동기식 SPI 레지스터 접근과
   짧은 카운터 폴링이 전부라 메인루프에서 매 주기 호출해도 안전함.
   단 RC522_Init 은 오실레이터 안정화를 위해 HAL_Delay(5) 를 쓰므로
   부팅 시 한 번만 호출해야 함. */

void    RC522_Init(void);                 /* 리셋 해제, 소프트 리셋, 레지스터 초기화, 안테나 ON */
uint8_t RC522_CheckCard(uint8_t *uidBuf); /* UID 4바이트를 채우고 UID 길이 반환; 카드 없으면 0 */

#ifdef __cplusplus
}
#endif

#endif /* __DEV_RFID_H__ */
