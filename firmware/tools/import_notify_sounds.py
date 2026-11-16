#!/usr/bin/env python3
"""녹음한 소리를 보드 알림·탭 소리 PCM 으로 만든다.

소리 6개(notify_sound.cc 가 EMBED_FILES 로 그대로 읽는다):

- notify_poke   : 콕·답장이 왔을 때 = 로나 야옹 1
- notify_guest  : 손님 캐릭터가 걸어 들어올 때 = 로찌 야옹 1
- tap_rona2     : 로나를 누르면 notify_poke 와 이것 중 하나 = 로나 야옹 2
- tap_rozzi2    : 로찌를 누르면 notify_guest 와 이것 중 하나 = 로찌 야옹 2
- tap_manbok1/2 : 만복이를 누르면 둘 중 하나 = 아기 옹알이

원본은 레포에 넣지 않는다. --src 폴더에 아래 이름으로 둔다.

- rona.m4a, rozzi.m4a : 집 고양이 녹음(폰 음성 녹음)
- baby_5mo.ogg  : https://commons.wikimedia.org/wiki/File:Baby_5_months_old_gabbling_laughing_practising_voice.ogg (Public domain)
- baby_14mo.ogg : https://commons.wikimedia.org/wiki/File:Even_tempered_baby_14_months_communicative.ogg (Public domain)

처리: ffmpeg 로 잘라 16kHz 모노 → 250Hz 아래·6kHz 위를 거르고 잡음 줄임(afftdn) → 최고치 PEAK 로
맞춤 → 시작 20ms·끝 80ms 페이드(딸깍 소리 방지). 크기는 보드 코덱 볼륨({"sound":{"vol":…}})으로 정한다.

    python3 tools/import_notify_sounds.py --src ~/sounds [--preview /tmp/sound]

ffmpeg 가 PATH 에 있어야 한다. 표준 라이브러리만 쓴다.
"""

import argparse
import array
import subprocess
import sys
import wave
from pathlib import Path

RATE = 16000          # notify_sound.cc kSampleRate 와 맞출 것
PEAK = 0.6
FADE_IN_S = 0.02
FADE_OUT_S = 0.08
FILTERS = "highpass=f=250,lowpass=f=6000,afftdn=nf=-35"

OUT_DIR = Path(__file__).resolve().parent.parent / "main" / "sounds"

# (출력 이름, 원본 파일, 시작 초, 끝 초)
CLIPS = (
    ("notify_poke", "rona.m4a", 3.95, 5.00),
    ("notify_guest", "rozzi.m4a", 5.20, 6.35),
    ("tap_rona2", "rona.m4a", 26.70, 27.35),
    ("tap_rozzi2", "rozzi.m4a", 1.65, 2.60),
    ("tap_manbok1", "baby_5mo.ogg", 196.00, 196.90),
    ("tap_manbok2", "baby_14mo.ogg", 103.55, 104.65),
)


def cut(src: Path, start: float, end: float) -> array.array:
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", f"{start}", "-to", f"{end}", "-i", str(src),
         "-ac", "1", "-ar", str(RATE), "-af", FILTERS, "-f", "s16le", "-"],
        check=True, capture_output=True).stdout
    pcm = array.array("h")
    pcm.frombytes(raw)
    if sys.byteorder != "little":
        pcm.byteswap()
    return pcm


def shape(pcm: array.array) -> array.array:
    peak = max(1, max(abs(v) for v in pcm))
    gain = PEAK * 32767 / peak
    n = len(pcm)
    fi, fo = int(RATE * FADE_IN_S), int(RATE * FADE_OUT_S)
    out = array.array("h", bytes(2 * n))
    for i, v in enumerate(pcm):
        g = gain
        if i < fi:
            g *= i / fi
        if i >= n - fo:
            g *= (n - 1 - i) / fo
        out[i] = max(-32768, min(32767, round(v * g)))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", type=Path, required=True, help="원본 녹음 폴더")
    ap.add_argument("--preview", type=Path, help="WAV 미리듣기를 쓸 폴더")
    args = ap.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    if args.preview:
        args.preview.mkdir(parents=True, exist_ok=True)
    for name, src, start, end in CLIPS:
        pcm = shape(cut(args.src / src, start, end))
        if sys.byteorder != "little":
            pcm.byteswap()
        data = pcm.tobytes()
        path = OUT_DIR / f"{name}.pcm"
        path.write_bytes(data)
        print(f"{path}  {len(pcm) / RATE:.2f}s  {len(data)}B")
        if args.preview:
            with wave.open(str(args.preview / f"{name}.wav"), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(RATE)
                w.writeframes(data)


if __name__ == "__main__":
    main()
