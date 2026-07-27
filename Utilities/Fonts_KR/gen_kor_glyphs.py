#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
한글 비트맵 글리프 생성기  (app_kor_glyphs.h / app_kor_glyphs.c 자동 생성)
=====================================================================
[역할]
  관제실 LCD 대시보드에 찍는 고정 한글 문구들을 1비트 비트맵으로 만들어
  C 소스(app_kor_glyphs.h/.c)로 내보낸다.

[비트맵 포맷]  (dev_display_kr.c 의 KOR_DrawGlyph 와 반드시 일치)
  - 행 stride = (width + 7) / 8 바이트
  - 각 바이트의 MSB(0x80) 가 왼쪽 픽셀
  - 비트=1 이 글자 픽셀, 비트=0 이 배경 픽셀

[렌더 방식]
  - 제목 글리프(KOR_CONTROL_ROOM = "관제실")는 Noto Sans CJK KR Regular,
    크기 28, 임계값(threshold) 96 으로 폰트에서 직접 렌더한다.
  - 나머지 21개 글리프는 이미 확립되어 검증된 비트맵(GOLDEN)을 그대로
    보존한다. (원래 생성 툴의 글자별 자간 처리를 1:1 로 재현할 수 없어,
    확립된 비트맵을 유지하는 것이 가장 안전하다. 폰트에서 다시 뽑으면
    1px 씩 어긋나 21개가 바뀌어 버린다.)

[안전장치]  ★★★ 과거에 빈 비트맵/더미 데이터로 화면이 하얗게 나온 사고가
  두 번 있었다. 그래서 아래 검사에서 하나라도 걸리면 파일을 쓰지 않고
  즉시 중단한다:
    - width==0 또는 height==0
    - 켜진 픽셀(비트=1)이 하나도 없음  (빈 비트맵)
    - 데이터 길이가 stride*height 와 다름
"""
import os, sys

# ── 폰트/렌더 파라미터 (원본과 동일하게 복원됨) ──
FONT_PATH = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
FONT_INDEX = 0
THRESHOLD = 96
TITLE_SIZE = 28

HERE = os.path.dirname(os.path.abspath(__file__))

# ── .h/.c 에 나열할 순서 (이름, 원본 한글 텍스트) ──
ORDER = [
    ("KOR_LOCKED", "잠금"),
    ("KOR_TAG_CARD", "카드를 태그하세요"),
    ("KOR_AUTH_FAIL", "인증 실패"),
    ("KOR_CONTROL_ROOM", "관제실"),
    ("KOR_CLEAR_EMG_BTN", "비상해제"),
    ("KOR_CURRENT_LABEL", "현재"),
    ("KOR_FLOOR_UNIT", "층"),
    ("KOR_TARGET_LABEL", "목표"),
    ("KOR_STATE_LABEL", "상태"),
    ("KOR_STATE_WAIT", "대기"),
    ("KOR_STATE_MOVE", "이동"),
    ("KOR_STATE_OPEN", "열림"),
    ("KOR_STATE_ERROR", "오류"),
    ("KOR_TEMP_LABEL", "온도"),
    ("KOR_HUMI_LABEL", "습도"),
    ("KOR_LINK_OK", "통신 정상"),
    ("KOR_LINK_LOST", "통신 두절"),
    ("KOR_TIME_LABEL", "시각"),
    ("KOR_EMERGENCY_TITLE", "비상 상황"),
    ("KOR_ESTOP_PRESSED", "비상정지 눌림"),
    ("KOR_ELEVATOR_ERROR", "엘리베이터 오류"),
    ("KOR_UNLOCK_BTN", "해제"),
    # ── 점검(정비) 기능용으로 새로 추가한 글리프 4개 ──
    #   GOLDEN 에 없는 이름들이라 build() 에서 폰트로 직접 렌더한다.
    #   같은 "점검" 글자를 버튼용(큰 것)과 상태줄용(작은 것) 두 크기로 쓰므로
    #   이름마다 폰트 크기를 따로 지정한다(아래 EXTRA_FONT_SIZE 참고).
    ("KOR_INSPECT_BTN", "점검"),          # 대시보드 점검 버튼 라벨 (높이 약 27px 목표)
    ("KOR_STATE_INSPECT", "점검"),        # 상태 줄 인라인 라벨 (높이 약 19px 목표)
    ("KOR_CLEAR_INSPECT_BTN", "점검해제"),  # 점검 팝업의 해제 버튼 라벨 (높이 약 27px 목표)
    ("KOR_INSPECT_TITLE", "점검 중"),     # 점검 팝업 제목 (높이 약 33px 목표)
    # ── 오류/경고 표시용으로 새로 추가한 글리프 5개 ──
    #   POS_ERROR/MOVE_TIMEOUT: 관리실 경고 팝업(draw_alert_popup) 본문 (오류코드 301/201)
    #   TEMP_WARN/SENSOR_TIMEOUT/SENSOR_DATA_ERR: 대시보드 온도줄 경고 접미사 (오류코드 101/102/103)
    ("KOR_POS_ERROR", "위치 인식 오류"),      # 팝업 본문 (높이 21px 목표)
    ("KOR_MOVE_TIMEOUT", "이동 타임아웃"),     # 팝업 본문 (높이 21px 목표)
    ("KOR_TEMP_WARN", "고온 경고"),          # 대시보드 라벨 (높이 18px 목표)
    ("KOR_SENSOR_TIMEOUT", "센서 타임아웃"),    # 대시보드 라벨 (높이 18px 목표)
    ("KOR_SENSOR_DATA_ERR", "센서 데이터 오류"),  # 대시보드 라벨 (높이 18px 목표)
]
TITLE_NAME = "KOR_CONTROL_ROOM"
TITLE_TEXT = "관제실"

# ── 새로 추가한(=GOLDEN 에 없는) 글리프의 폰트 크기 (이름 -> 크기) ──
#   목표 높이에 맞춘 값이며, 높이는 폰트 텍스트 상자 기준으로 대략 다음과 같다:
#     크기 26 -> 약 27px(버튼 라벨), 크기 18 -> 약 19px(상태 인라인),
#     크기 30 -> 약 33px(팝업 제목).
#   레이아웃은 glyph->width/height 로 자동 중앙정렬되므로 정확할 필요는 없다.
EXTRA_FONT_SIZE = {
    "KOR_INSPECT_BTN": 26,        # 버튼용 "점검"  (약 27px)
    "KOR_STATE_INSPECT": 18,      # 상태줄용 "점검" (약 19px)
    "KOR_CLEAR_INSPECT_BTN": 26,  # 버튼용 "점검해제" (약 27px)
    "KOR_INSPECT_TITLE": 35,      # 팝업 제목 "점검 중" (약 33px)
    "KOR_POS_ERROR": 21,          # 팝업 본문 "위치 인식 오류" (21px, 124x21)
    "KOR_MOVE_TIMEOUT": 21,       # 팝업 본문 "이동 타임아웃" (21px, 118x21)
    "KOR_TEMP_WARN": 18,          # 대시보드 라벨 "고온 경고" (18px, 70x18)
    "KOR_SENSOR_TIMEOUT": 18,     # 대시보드 라벨 "센서 타임아웃" (18px, 103x18)
    "KOR_SENSOR_DATA_ERR": 18,    # 대시보드 라벨 "센서 데이터 오류" (18px, 123x18)
}

# ── 확립된(GOLDEN) 21개 글리프: (width, height, hex 비트맵) ──
GOLDEN = {
    "KOR_LOCKED": (81, 42,
        "00000007800000000000000000000780000000000000000000078001ffffffe0007fffff"
        "078001ffffffe0007fffff078001ffffffe0007fffff078000000001e000003e00078000"
        "000001e000003c00078000000001e000003e00078000000001e000003e00078000000001"
        "e000003e00078000000001e000003e0007fe00000001e000007e0007fe00000001e00000"
        "7f0007fe00000001e000007f80078000000001e00000f780078000000003e00001f3c007"
        "8000000003c00003e3e0078000000003c00007c1f807803fffffffff000f80fc07803fff"
        "ffffff003f807f07803fffffffff00fe003f07800000000000007c000e07800000000000"
        "007800020780000000000000000000078000000000000000000000000000000000000000"
        "00000000000000000003ffffff8001ffffffe00003ffffff8001ffffffe00003ffffff80"
        "01ffffffe00003c000078001e00001e00003c000078001e00001e00003c000078001e000"
        "01e00003c000078001e00001e00003c000078001e00001e00003c000078001e00001e000"
        "03c000078001e00001e00003c000078001e00001e00003c000078001e00001e00003ffff"
        "ff8001ffffffe00003ffffff8001ffffffe00003ffffff8001ffffffe000"),
    "KOR_TAG_CARD": (172, 23,
        "0000000000000000000000000000000000000000000000030000000000000000c6000000"
        "0c060000cc00000000030000000fffe00000c60000000c060000cc01fc007fe301fffc00"
        "006003fcc60fffe00c0600c0cc07ff003fe301fff80000e003fcc60fffe1ffe600c0cc0e"
        "0380006301c0000fffe00300c60000e1ffe600c0cc1c01c000e301c0000e00000300c600"
        "00e0000600c0cc1800c000e301c0000e00000300c60000e00c0600c0cc1800c00fc301c0"
        "000fffe00300c60000c03f0600efcc1800c0ffc3e1c00007ffc003fcfe0000c0738600ef"
        "cc1800c061c3f1c00000000003fcfe0000c060c7e0e0cc0c0180018301fffc3ffff80300"
        "c60000c0c0c7c1e0cc0f0780038301fffc3ffff80300c60000c0c0c601b0cc03ff000703"
        "0000000000000300c60000c0c0c603b8cc0376000e030000000fffe00300c60000c0e0c6"
        "0318cc0306001c0300000007ffe003fec60000c071c6060ccc0306007803000000000060"
        "03fec60000003f860e0ccc030600f0030000000000e00000c63ffff80e060400cc030600"
        "400307ffff0fffe00000c63ffff800060000cc7ffff0000307ffff0c00000000c6000000"
        "00060000cc7ffff000030000000e00000000c600000000060000cc00000000030000000f"
        "ffe0000006000000000600000c0000000001000000000000000006000000000200000c00"
        "0000"),
    "KOR_AUTH_FAIL": (103, 27,
        "000000000000000000000000000000300000000000003000010c0000301ffff800018030"
        "00018c0fc0301ffffc0001803000018c1ff030001c00000380303ff98c387030003c0000"
        "0380307ff98c703830007e000003c0300c618c70183000f7000007c0300c618c601c3003"
        "e3c00006e0300c618c601c301f81fc000e70300c618c7018301e007c001c38300c618c70"
        "383000000000781e300c61fc38783000000000700e300c61fc1ff0307fffff004000300c"
        "618c0fc0307ffffe000000000c618c0000300000000007fff00c618c000030000000000f"
        "fff00c618c060030007f00000000300c698c06003003ffe0000000307ffd8c0600300780"
        "f0000000307ff18c0600000e00380007fff000018c0600000c00380007fff000018c0600"
        "000e00380006000000018c0600000f00700006000000018c07fff807ffe00007fff80001"
        "8c07fff800ff800007fff800000c00000000000000000000000000"),
    "KOR_CLEAR_EMG_BTN": (96, 27,
        "00000000000000000000000000006000006000010c0000c600006003006003818c0000c6"
        "60306003806003818c0000c660306003806003818c1ffcc66030600380607ff98c1ffcc6"
        "6030600380607ffd8c01c0c660306003806000018c01c0c660306007807e00018c01c0c6"
        "60306007c07e07c18c01c0c67ff0600ee0600fe18c01cfc67ff0600e70601c718c01cfc6"
        "6030603c78603831fc01c0c6603060781c603031fc03e0c6603060700c6030398c0360c6"
        "60306000006030398c0770c660306000000038318c0630c660306001ff001c718c0e38c6"
        "7ff06007ffc00fe18c1c1cc67ff0600f01e007c18c380cc60000600c006000018c1000c6"
        "0000601c007000018c0000c60000600c006000018c0000c60000600f01e000018c0000c6"
        "00006007ffc000018c0000c600006001ff0000000c000006000000000000000000000000"),
    "KOR_CURRENT_LABEL": (33, 19,
        "0002000100060600198006060019807fe63fd98000060619800f060619801bbe06198030"
        "c606198030c6071f8030fe07198039860d19800f060d9980000618d9800c063079800c02"
        "2019800c000019800c000019800fff0019800000000180"),
    "KOR_FLOOR_UNIT": (17, 19,
        "00800000c0003ffe0001c00001c00003e0000f38003c1e000002000000007fff80000000"
        "0000000ff8001c0c001806001c0c000ff800000000"),
    "KOR_TARGET_LABEL": (34, 19,
        "00000000001ffe00000018061fff001806031c00180603080018060308001ffe03080000"
        "c003080000c003080000c0031c007fff9fff00000003180000000318001ffe0318000006"
        "03180000063fffc0000600000000060000000004000000"),
    "KOR_STATE_LABEL": (33, 19,
        "00040001000604001980060400198006041fd980060410198006079019800f0610198019"
        "8410198039c41f9f8070e41019804004101980000410198007f019d9800e3c1fd980180c"
        "00198018040019800c1c00198007f80019800000000100"),
    "KOR_STATE_WAIT": (32, 19,
        "000400000066000600660006fe663fc6c06600c6c06600c6c06600c6c0660186c07e0186"
        "c0660306c0660306c0660606ef661c06ff66380600667006006600060066000600660006"
        "00040002"),
    "KOR_STATE_MOVE": (33, 19,
        "0004000000000c0000001e0c1ffe003f0c180000618c180000618c180000418c1ffe0040"
        "8c00c000c08c00c000408c00c000618c7fff80618c000000330c07f0003e0c1e3c00000c"
        "180400000c180600000c1c1c00000c0ff800000c000000"),
    "KOR_STATE_OPEN": (32, 19,
        "000000001e0c0006770c3fc661fc00c6c18c00c6c18c3fc6c18c300661fc2006770c3026"
        "3c0c3ff6000000061ffc0000000c0ffe000c0c061ffc0c0618000c0618000c061ffe0ffe"
        "00000000"),
    "KOR_STATE_ERROR": (34, 19,
        "000000000000000fff0007f00003000e1c000300180e00030010060fff0030060c000010"
        "060c0000180e0fff000e1c00000007f000000000c000000000c03fffc000c0030c0000c0"
        "030c007fff830c000000030c000000030c000000030800"),
    "KOR_TEMP_LABEL": (34, 19,
        "000000000007f00000000f3c0fff00180e0c000010060c000018060c00001c1c0c00000f"
        "f80c000000c00c000000c00fff007fff8060000000006000000000600018000060001800"
        "00600018003fffc018000000001ffe0000000000000000"),
    "KOR_HUMI_LABEL": (34, 19,
        "000000000000c000000000c00fff0001e00c000003700c00000e3c0c00003c0e0c000000"
        "000c000000000c00007fff8fff0000000060000000006000180600600018060060001ffe"
        "00600018063fffc018060000001ffe0000000000000000"),
    "KOR_LINK_OK": (74, 19,
        "000000010000000002001ffe010300000c0302001800030300ffcc03020018000303000c"
        "0c0302001ffc0303000c0c03020018000383000c7c0303c018000383001e0c0783001ffe"
        "06c3001b0c0cc20000c00ce300338c1ce20000c01c7300e1cc3872007fffb03300c00c20"
        "020000000003000004000200000000030007f003f8000ff80603001e38071e001c0c0600"
        "00180c0c06001806060000180c0c02001c0c0600001c1c060e000ff807ff800ff803fc00"
        "00000000000000000000"),
    "KOR_LINK_LOST": (73, 19,
        "000000010000000000001ffe010300000000030018000303003ff83ff300180003030030"
        "000303001ffc030300300003030018000383003000039f00180003830030000783001ffe"
        "06c3003ffc0cc30000c00ce300000038730000c01c730000003003007fffb03300000000"
        "00000000000300ffff07ff00000000030001800003000ff806030001800003001c0c0600"
        "00018007ff00180606000001800600001c0c06000001800600000ff807ff80018007ff80"
        "00000000000100000000"),
    "KOR_TIME_LABEL": (34, 19,
        "0000000200000600020002061fe2000606006200060600c200060600c3c0060601830006"
        "0603020007060e02000f063c02000d8630020019c600000030e60ffe0070660006000006"
        "0002000006000200000600020000060002000000000200"),
    "KOR_EMERGENCY_TITLE": (129, 33,
        "000007000000180000000030000e0070000000070000001c0000000038000e0070000000"
        "070007001c00000e0038000e007000e00e070007001c00000e00380ffffc7000e00e0700"
        "07001c00000e00380ffffc7000e00e070007001c00000e00380000007000e00e07000700"
        "1c00000e00380000007000e00e070007001c00000e0038007f807000e00e070007801c00"
        "000f003801ffe07000e00e07000f801fc0001f003f83c0f07000e00e07000fc01fc0001f"
        "803f8380707f00f00e07001dc01c00003b80380380707f00fffe07003ce01c000079c038"
        "0380707000fffe070078f01c0000f1e03801e1f07000e00e0700f87c1c0001f0f83801ff"
        "e07000e00e0701f03e1c0003e07c38007f807000e00e0703e01e1c0007c03c38000e0070"
        "00e00e070380041c0007000838000e7e7000e00e070100001c00020000381ffffe7000e0"
        "0e070000001c00000000380ffff07000e00e070000000000000000000000007000e00e07"
        "0003ffc0000007ff800000007000fffe07000ffff000001fffe0000ffe0000fffe07001f"
        "01f800003e03f0003fffc000000007003c0038000078007000fc07e0000000070038001c"
        "000070003800f000e0000000070038001c000070003800e00070000000070038001c0000"
        "70003800e0007000000007003c007800007800f000e000f000000007001fc3f800003f87"
        "f000fc07e000000007000fffe000001fffc0003fffc0000000070001ff80000003ff0000"
        "0ffe00000000000000000000000000000000000000"),
    "KOR_ESTOP_PRESSED": (160, 21,
        "00000000000000000100000000000000000000000006000180000c000180000180000003"
        "00000003c186018181ff8c3ff180306183ffe00300007fc3c186018181ff0c3ff1803061"
        "8300000300007fc3c186038180180c030180306183000003000000c3c186038180180c03"
        "0180306183ffe003ffe000c3c1860381f038fc030180306183000003ffe07fc3c18607c1"
        "e03c7c03018030e18300000000006003ff8606e1806e0c0301803fff8380000ffff86003"
        "c1860c7180e70c078180307f83ffe00ffff86023c186383981c38c078180306180000000"
        "0c007ff3c186100183808c07c18030618ffff8000c007c03c18600018000080cc1803061"
        "8ffff803ffe00002c18601fc000fe018618030618000000000600ffeff8607ff003ff838"
        "39803fe18300000000601fffff86060380701c7019803fc1830000000060180300060c01"
        "80600c000180000183000003ffe018030006060180600c00018000018300000300001803"
        "00060707003838000180000183ffe00300001803000603fe001ff0000180000183fff003"
        "fff01fff00040000000000000100000180000001ffe00ffe"),
    "KOR_ELEVATOR_ERROR": (139, 21,
        "000000000000000000000000000000000000001b00006000260000c000180000001fff00"
        "3e1b0ff06002360100c3fc18003f801fff00631b0ff860863607c0c3fe1800ffe0000300"
        "c19b00186086360c60c3001800c070000300c1fb00186086361830c300180180301fff00"
        "c1fb00186086361830c30018018018180000c19b001860fe361830c30018018018180000"
        "631b0ff860fff61830c3fdf80180301800003e1b0ff06087f61810c3f9f801c0301fff80"
        "081b0c006086361830c3001800e0e01fff0000000c006086361830c30018007fc0000000"
        "1fff0c006086361830c30018000e007fffc00fff0c006086360c60c300180006007fffc0"
        "00030ffe60fe360fe0c3ff18000600060c0000030ffe60fe3607c0c3ff18000600060c00"
        "1fff00006000360000c0001807fffc060c00180000006000360000c0001807fffc060c00"
        "180000006000360000c00018000000060c001fff80006000260000c00018000000060c00"
        "0fff00006000020000800018000000020c00"),
    "KOR_UNLOCK_BTN": (97, 27,
        "00000000000000000000000000000060000000000086000063000000600ffff801c0c600"
        "0063007ffc600ffff801c0c6000063007ffc6000003801c0c60ffe63000380600000383f"
        "fcc60ffe63000380600000383ffec600e0630003807e0000380000c600e0630003807e00"
        "00380000c600e0630007c06000003003e0c600e063000ee06000003007f0c600e7e3001c"
        "70600000300e38c600e7e3003c3c607fffff1c18fe00e06300f81c607fffff1818fe01f0"
        "6300600c60000000181cc601b06300000060000000181cc603b863000000000000001c18"
        "c6031863000fffe00ffff80e38c6071c63000fffe00ffff807f0c60e0e63000c00600c00"
        "3803e0c61c0663000c00600c00380000c6080063000c00600c00380000c6000063000c00"
        "600c00380000c6000063000c00600c00380000c6000063000fffe00ffff80000c6000063"
        "000fffe00ffff80000060000030000000000000000000000000000"),
}


def render_from_font(text, size):
    """Noto Sans CJK KR 에서 text 를 렌더해 (width, height, bytes) 반환.
       규칙: H = textbbox 높이, 프레임 상단 = textbbox 상단,
             임계값 96 으로 이진화, 잉크 왼쪽을 col0 에 정렬,
             W = 임계화 잉크폭 + 1(오른쪽 1px 여백)."""
    from PIL import Image, ImageFont, ImageDraw
    font = ImageFont.truetype(FONT_PATH, size, index=FONT_INDEX)
    OFF = 40
    canvas = Image.new("L", (600, 200), 0)
    d = ImageDraw.Draw(canvas)
    d.text((OFF, OFF), text, font=font, fill=255)
    bx0, by0, bx1, by1 = d.textbbox((OFF, OFF), text, font=font)
    H = by1 - by0
    px = canvas.load(); W0, H0 = canvas.size
    minx, maxx = 10**9, -1
    for y in range(H0):
        for x in range(W0):
            if px[x, y] >= THRESHOLD:
                if x < minx: minx = x
                if x > maxx: maxx = x
    if maxx < 0:
        return 0, 0, b""
    W = (maxx - minx + 1) + 1
    stride = (W + 7) // 8
    data = bytearray()
    for r in range(H):
        for bidx in range(stride):
            byte = 0
            for b in range(8):
                c = bidx * 8 + b
                sx, sy = minx + c, by0 + r
                if c < W and 0 <= sx < W0 and 0 <= sy < H0 and px[sx, sy] >= THRESHOLD:
                    byte |= (0x80 >> b)
            data.append(byte)
    return W, H, bytes(data)


def safety_check(name, w, h, data):
    """빈/잘못된 비트맵이면 즉시 중단(더미 데이터 방지)."""
    stride = (w + 7) // 8
    if w == 0 or h == 0:
        sys.exit("[중단] %s: 폭/높이가 0 입니다." % name)
    if len(data) != stride * h:
        sys.exit("[중단] %s: 데이터 길이 %d != 기대값 %d." % (name, len(data), stride * h))
    if not any(data):
        sys.exit("[중단] %s: 켜진 픽셀이 하나도 없는 빈 비트맵입니다." % name)


def build():
    glyphs = {}   # name -> (w,h,bytes)
    # 제목만 폰트에서 렌더
    w, h, data = render_from_font(TITLE_TEXT, TITLE_SIZE)
    glyphs[TITLE_NAME] = (w, h, data)
    # 나머지 처리:
    #   - GOLDEN 에 있는 확립된 글리프는 그대로 보존한다(자간 흔들림 방지).
    #   - GOLDEN 에 없는(=새로 추가한) 이름은 EXTRA_FONT_SIZE 크기로
    #     폰트에서 직접 렌더한다.
    for name, text in ORDER:
        if name == TITLE_NAME:
            continue
        if name in GOLDEN:
            gw, gh, gh_hex = GOLDEN[name]
            glyphs[name] = (gw, gh, bytes.fromhex(gh_hex))
        else:
            size = EXTRA_FONT_SIZE[name]
            w, h, data = render_from_font(text, size)
            glyphs[name] = (w, h, data)
    # 전수 안전검사
    for name, _t in ORDER:
        w, h, data = glyphs[name]
        safety_check(name, w, h, data)
    return glyphs


def emit(glyphs):
    # app_kor_glyphs.h
    h_lines = [
        "#ifndef APP_KOR_GLYPHS_H",
        "#define APP_KOR_GLYPHS_H",
        "",
        "#include <stdint.h>",
        "",
        "/* 한글 비트맵 글리프 구조체.",
        "   bitmap: 1비트 팩킹(행 stride=(width+7)/8, MSB=왼쪽픽셀). */",
        "typedef struct {",
        "    uint16_t       width;",
        "    uint16_t       height;",
        "    const uint8_t *bitmap;",
        "} KorGlyph_t;",
        "",
    ]
    for name, text in ORDER:
        h_lines.append("extern const KorGlyph_t %s;   /* \"%s\" */" % (name, text))
    h_lines += ["", "#endif /* APP_KOR_GLYPHS_H */", ""]
    with open(os.path.join(HERE, "app_kor_glyphs.h"), "w", encoding="utf-8") as f:
        f.write("\n".join(h_lines))

    # app_kor_glyphs.c
    c_lines = [
        "/* 자동 생성 파일 — gen_kor_glyphs.py 로 생성됨. 직접 수정하지 말 것. */",
        "#include \"app_kor_glyphs.h\"",
        "",
    ]
    for name, text in ORDER:
        w, h, data = glyphs[name]
        stride = (w + 7) // 8
        c_lines.append("/* \"%s\"  %dx%d */" % (text, w, h))
        c_lines.append("static const uint8_t %s_bits[] = {" % name)
        # 한 행씩(가독성) 바이트 나열
        for r in range(h):
            row = data[r*stride:(r+1)*stride]
            c_lines.append("    " + " ".join("0x%02X," % b for b in row))
        c_lines.append("};")
        c_lines.append("const KorGlyph_t %s = { %d, %d, %s_bits };" % (name, w, h, name))
        c_lines.append("")
    with open(os.path.join(HERE, "app_kor_glyphs.c"), "w", encoding="utf-8") as f:
        f.write("\n".join(c_lines))


if __name__ == "__main__":
    gl = build()
    emit(gl)
    total_on = 0
    print("생성 완료. 글리프 %d 개:" % len(gl))
    for name, _t in ORDER:
        w, h, data = gl[name]
        on = sum(bin(b).count("1") for b in data)
        total_on += on
        print("  %-22s %3dx%-3d  on=%d" % (name, w, h, on))
    print("총 켜진 픽셀: %d (모두 0 이상이어야 정상)" % total_on)
