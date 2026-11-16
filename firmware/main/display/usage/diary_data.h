#pragma once

// 만복이 육아일기(docs/manbok-diary.md). 보드가 일기 앱의 /api/board 에서 받은
// 최근 일기와, 일기 판이 보여 줄 사진 한 장. 받기는 diary_fetcher.cc, 그리기는 diary_panel.cc.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct DiaryEntry {
    std::string id;       // "2026-10-17-mom"
    std::string date;     // "2026-10-17"
    std::string writer;   // "mom" | "dad"
    std::vector<std::string> mood;
    std::string body;     // 문단은 "\n" 으로 나뉜다. 이모지는 서버가 지웠다(보드 글꼴에 없음)
    std::string photo;    // 첫 사진을 받을 주소(앱 주소 뒤, 서명 포함). 없으면 ""
    std::string photo_key;   // 그 사진의 키(path=…). 서명은 10분마다 바뀌어도 키는 같아 캐시에 쓴다
    // 넘겨 볼 사진 전부(첫 사진 포함, 서버 상한 10장). photos[0] == photo. 옛 서버면 photo 하나만 들어 있다.
    std::vector<std::string> photos;
    std::vector<std::string> photo_keys;
    int photo_count = 0;   // 일기에 실제로 있는 사진 수(상한을 넘으면 photos 보다 많다). 「+N」 표시에 쓴다
};

struct DiaryData {
    bool valid = false;   // 한 번이라도 받았나
    std::vector<DiaryEntry> entries;   // 최신순
};

// RGB565(LE) 한 장. 줄 간격(stride)은 w*2 다. pixels 는 PSRAM 에 있고 heap_caps_free 로 지운다.
struct DiaryImage {
    uint8_t* pixels = nullptr;
    int w = 0;
    int h = 0;
};

// 디코딩한 사진 한 장을 네 크기로 미리 줄여 둔다(그릴 때 확대·축소를 안 하게).
//   big  화면 가득(320×240 안에 맞춤)   mid  글 1쪽(96×128 안)   mini  목록 줄(24×32 안)
//   thumb  만복이 메뉴 초음파 목록 썸네일(60×45 안, manbok_panel.cc)
struct DiaryPhoto {
    std::string key;
    DiaryImage big, mid, mini, thumb;
    ~DiaryPhoto();
};

using DiaryPhotoPtr = std::shared_ptr<DiaryPhoto>;

// "mom" → "엄마", 그 밖 → "아빠".
inline const char* DiaryWriterName(const std::string& writer) { return writer == "dad" ? "아빠" : "엄마"; }

// 흐르는 줄에 쓸 한 줄: 「엄마 일기 · 첫 문단」. 너무 길면 자른다(UTF-8 경계).
std::string DiaryTickerLine(const DiaryEntry& e);
