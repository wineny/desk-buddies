# 선물 빌드 공통 assets

`CONFIG_USAGE_CHARACTER_PACK` 에 `bboketer` 가 있는 빌드는 이 폴더의 `*.bin`·`*.json` 을 assets 파티션에
같이 넣는다(`main/CMakeLists.txt`).

## font_galmuri11_12_1.bin

흐르는 줄 글꼴. Galmuri11(SIL OFL 1.1, https://github.com/quiple/galmuri, 라이선스 원문 `OFL-Galmuri.txt`) 12px 1bpp, ASCII + KS X 1001
한글 2,350자 + 문장부호. 85KB. `78/lv_font_conv` 커밋 `c420999f` 로 만들었다.

```bash
git clone https://github.com/78/lv_font_conv && cd lv_font_conv && git checkout c420999f
npm ci --omit=dev --ignore-scripts
node lv_font_conv.js --no-compress --no-prefilter --force-fast-kern-format --font Galmuri11.ttf \
  --format cbin --bpp 1 --size 12 -r <유니코드 범위> -o font_galmuri11_12_1.bin
```

표 밖의 글자(쀍 같은 희귀 한글, 이모지)는 빈칸으로 나온다.

## tips.json (없으면 D-day 만 흐른다)

```json
[{"week_from": 27, "week_to": 30, "slot": "morning", "text": "잘 때는 옆으로 누우면 숨이 덜 차요"}]
```

`slot` 은 `morning`(05~11시)·`noon`(11~17시)·`evening`(17~05시). 없으면 언제나. 주수는 `POST /usage
{"due":"YYYY-MM-DD"}` 로 저장한 예정일에서 40주로 계산한다. 날짜는 코드·레포에 쓰지 않는다.
