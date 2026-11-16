// 켜진 캐릭터 그리기 (docs/character-select-plan.md).
//
// 장면 화면의 캐릭터는 세 마리가 각자 lv_image 한 장(cast_img_)이다. 몸(figure_)은 장면에서 숨겨 둔다.
// 자리는 조합(비트 묶음: 로나 1, 로찌 2, 만복이 4)마다 tools/lineart_cast.py 가 계산해
// bboketer_cast_table.h 로 굽는다. 놀러가기의 나가고 들어오는 동작도 이 그림들을 옆으로 미는 것이다.

#include <esp_log.h>

#include "poke_link.h"
#include "usage_screen.h"


namespace {

constexpr const char* kTag = "UsageCast";
constexpr int kSeatBobPx = 2;              // 제자리에서 번갈아 오르내리는 높이
constexpr int kMoveBobPx = 6;              // 걸어갈 때 통통 뛰는 높이
constexpr uint32_t kMoveBobMs = 150;
constexpr int kStateFoot = 206;            // 상태 그림 발 줄(tools/lineart_state_compose.py FOOT)
constexpr int kStateGap = -16;             // 상태 그림 사이. 음수 = 겹쳐 앉는다(ny 「붙여서」)
constexpr int kHitPadPx = 6;
constexpr uint32_t kReactTickMs = 100;
constexpr int kSneezeBelowPercent = 22;    // 그림 위쪽 이 %까지 = 코, 재채기
constexpr int kPetBelowPercent = 40;       // 그 아래 이 %까지 = 머리, 쓰다듬기

void SetObjHidden(lv_obj_t* obj, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace

void UsageScreen::CastCreate() {
    BboketerCastLoad();
    for (lv_obj_t*& img : cast_img_) {
        img = lv_image_create(panel_);
        lv_image_set_pivot(img, 0, 0);
        lv_image_set_antialias(img, true);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        SetObjHidden(img, true);
    }
}

bool UsageScreen::CastActive() const {
    return cast_img_[0] != nullptr && IsBboketerScene();
}

uint8_t UsageScreen::CastDrawMask() const {
    const uint8_t mine = BboketerCastMask();
    switch (trip_) {
        case Trip::kAway: return mine & ~trip_away_mask_;
        case Trip::kGuestEnter:
        case Trip::kGuestStay:
        case Trip::kGuestExit: return mine | trip_guest_mask_;
        default: return mine;
    }
}

bool UsageScreen::CastMoving(int who) const {
    const uint8_t bit = static_cast<uint8_t>(1 << who);
    switch (trip_) {
        case Trip::kLeaving:
        case Trip::kReturning: return (trip_away_mask_ & bit) != 0;
        case Trip::kGuestEnter:
        case Trip::kGuestExit: return (trip_guest_mask_ & bit) != 0;
        default: return false;
    }
}

void UsageScreen::CastPut(int who, const lv_image_dsc_t* src, int x, int y) {
    if (src == nullptr) {
        SetObjHidden(cast_img_[who], true);
        return;
    }
    if (cast_src_[who] != src) {
        lv_image_set_src(cast_img_[who], src);
        cast_src_[who] = src;
    }
    lv_obj_set_pos(cast_img_[who], x, y);
    SetObjHidden(cast_img_[who], false);
}

void UsageScreen::CastHide() {
    cast_visible_ = false;
    if (cast_img_[0] == nullptr) return;
    for (lv_obj_t* img : cast_img_) SetObjHidden(img, true);
}

// 상태 화면: 켜진 캐릭터의 상태 그림을 붙여서 가운데에 놓는다. 나가 있는 캐릭터와 손님은 빠진다.
void UsageScreen::CastPlaceState() {
    uint8_t mask = BboketerCastMask();
    if (TripBodyGone()) mask &= ~trip_away_mask_;
    const lv_image_dsc_t* art[kCastCount] = {};
    int total = 0, count = 0;
    for (int who = 0; who < kCastCount; ++who) {
        if ((mask & (1 << who)) == 0) continue;
        art[who] = BboketerCastState(who, static_cast<int>(state_));
        if (art[who] == nullptr) continue;
        total += static_cast<int>(art[who]->header.w);
        ++count;
    }
    if (count > 1) total += kStateGap * (count - 1);
    int x = width_ / 2 - total / 2;
    for (int who = 0; who < kCastCount; ++who) {
        if (art[who] == nullptr) {
            SetObjHidden(cast_img_[who], true);
            continue;
        }
        CastPut(who, art[who], x, kStateFoot - static_cast<int>(art[who]->header.h));
        x += static_cast<int>(art[who]->header.w) + kStateGap;
    }
}

void UsageScreen::ApplyCast() {
    if (cast_img_[0] == nullptr) return;
    if (!cast_visible_ || !IsBboketerScene()) {
        for (lv_obj_t* img : cast_img_) SetObjHidden(img, true);
        return;
    }
    if (state_ != kUsageStateIdle) {
        CastPlaceState();
        return;
    }
    const int scene = BodyScene();
    const uint8_t mask = CastDrawMask();
    CastSpot spots[kCastCount];
    int order[kCastCount] = {0, 1, 2};
    for (int who = 0; who < kCastCount; ++who) spots[who] = BboketerCastSpot(scene, mask, who);
    // 발이 아래에 있는 캐릭터를 앞에 그린다.
    for (int i = 1; i < kCastCount; ++i) {
        for (int j = i; j > 0 && spots[order[j - 1]].by > spots[order[j]].by; --j) {
            const int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    }
    const int base = static_cast<int>(lv_obj_get_index(rozzi_img_)) + 1;
    int rank = 0;
    for (int i = 0; i < kCastCount; ++i) {
        const int who = order[i];
        if (static_cast<int>(lv_obj_get_index(cast_img_[who])) != base + i) {
            lv_obj_move_to_index(cast_img_[who], base + i);
        }
        if (!spots[who].on) {
            SetObjHidden(cast_img_[who], true);
            continue;
        }
        const bool moving = CastMoving(who);
        const lv_image_dsc_t* src = nullptr;
        if (cast_react_pose_[who] != 0) {
            src = BboketerCastPose(who, cast_react_pose_[who]);
        } else if (moving) {
            src = BboketerCastPose(who, kCastPoseIdle);   // 장면 자세는 앉은 채로 그려져 있어 걸을 때 어색하다
        }
        if (src == nullptr) src = BboketerCastBody(scene, who, spots[who].tail);
        if (src == nullptr) {
            SetObjHidden(cast_img_[who], true);
            continue;
        }
        int bob = 0;
        if (moving) {
            bob = ((trip_t_ms_ / kMoveBobMs) & 1) != 0 ? kMoveBobPx : 0;
        } else if (cast_react_pose_[who] == 0) {
            bob = (rank % 2 == 0) == scene_bob_up_ ? 0 : kSeatBobPx;   // 옆 캐릭터와 번갈아 오르내린다
        }
        ++rank;
        const int x = spots[who].cx - static_cast<int>(src->header.w) / 2 + (moving ? trip_x_ : 0);
        CastPut(who, src, x, spots[who].by - static_cast<int>(src->header.h) - bob);
    }
}

void UsageScreen::CastReact(uint8_t mask, uint8_t pose, uint32_t ms) {
    const uint32_t now = lv_tick_get();
    for (int who = 0; who < kCastCount; ++who) {
        if ((mask & (1 << who)) == 0) continue;
        cast_react_pose_[who] = pose;
        cast_react_start_[who] = now;
        cast_react_ms_[who] = ms;
    }
    if (cast_react_timer_ == nullptr) {
        cast_react_timer_ = lv_timer_create(OnCastReactTick, kReactTickMs, this);
    }
    ApplyCast();
}

void UsageScreen::CastReactStop() {
    for (uint8_t& pose : cast_react_pose_) pose = 0;
    if (cast_react_timer_ != nullptr) {
        lv_timer_delete(cast_react_timer_);
        cast_react_timer_ = nullptr;
    }
}

void UsageScreen::OnCastReactTick(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    if (self != nullptr) self->CastReactTick();
}

void UsageScreen::CastReactTick() {
    bool any = false, changed = false;
    for (int who = 0; who < kCastCount; ++who) {
        if (cast_react_pose_[who] == 0) continue;
        if (lv_tick_elaps(cast_react_start_[who]) >= cast_react_ms_[who]) {
            cast_react_pose_[who] = 0;
            changed = true;
        } else {
            any = true;
        }
    }
    if (changed) ApplyCast();
    if (!any) {
        lv_timer_delete(cast_react_timer_);
        cast_react_timer_ = nullptr;
    }
}

// 앞에 그린 캐릭터부터 본다. 맞으면 who 와 그림의 어디를 눌렀는지(pose)를 돌려준다.
bool UsageScreen::CastHit(int x, int y, int* who, uint8_t* pose) const {
    int best = -1, best_index = -1;
    for (int i = 0; i < kCastCount; ++i) {
        lv_obj_t* img = cast_img_[i];
        if (lv_obj_has_flag(img, LV_OBJ_FLAG_HIDDEN) || cast_src_[i] == nullptr) continue;
        const int left = lv_obj_get_x(img), top = lv_obj_get_y(img);
        const int w = static_cast<int>(cast_src_[i]->header.w), h = static_cast<int>(cast_src_[i]->header.h);
        if (x < left - kHitPadPx || x >= left + w + kHitPadPx || y < top || y >= top + h) continue;
        const int index = static_cast<int>(lv_obj_get_index(img));
        if (index > best_index) {
            best_index = index;
            best = i;
        }
    }
    if (best < 0) return false;
    const int h = static_cast<int>(cast_src_[best]->header.h);
    const int percent = (y - lv_obj_get_y(cast_img_[best])) * 100 / h;
    *who = best;
    *pose = percent < kSneezeBelowPercent ? kCastPoseSneeze : (percent < kPetBelowPercent ? kCastPosePet : kCastPoseTap);
    return true;
}

// 상대가 놀러 오려는데 이미 다 같이 있다. 건너가지 않고 그 자리에서 인사만 한다.
void UsageScreen::TripWave() {
    ShowPokeStatus("ALL HERE");
    CastReact(CastDrawMask(), kCastPoseTap, 1500);
}

void UsageScreen::CastReactAll(uint32_t ms) {
    CastReact(CastDrawMask(), kCastPoseTap, ms);
}

uint8_t UsageScreen::CastGoing(uint8_t asked) const {
    const uint8_t mine = BboketerCastMask();
    return asked != 0 ? asked & mine : mine;
}

// 옷장에서 켜고 끈 뒤. 놀러가기 중이었으면 끝내고 상대에게도 알린다(ny 기획: 고르면 놀러가기는 끝난다).
void UsageScreen::CastChanged() {
    if (trip_ != Trip::kNone) {
        EndTrip();
        PokeLink::GetInstance().SendTripHome();
    }
    CastReactStop();
    ApplyScenePlace();
    ESP_LOGI(kTag, "cast -> %u", BboketerCastMask());
}

