// 보드 놀러가기 (docs/trip.md): 내 캐릭터가 화면 밖으로 나가 상대 보드에 손님으로 들어온다.
//
// 그림은 보내지 않는다. 두 보드가 같은 펌웨어라 팩 표가 같으므로 캐릭터 번호만 오간다.
// 움직이는 몸은 lv_image 한 장(trip_img_)이다. 나가는 내 캐릭터도, 들어오는 손님도 이 그림을
// 쓴다 — 장면 몸(assets ARGB)은 자세가 앉은 채로 박혀 있어 움직이면 어색하기 때문이다.
//
// 통신 순서와 끊겼을 때의 복구는 poke_link.cc, 사람이 하는 일은 docs/trip.md 에 있다.

#include <esp_log.h>

#include "poke_link.h"
#include "usage_screen.h"

namespace {

constexpr const char* kTag = "UsageTrip";
constexpr uint32_t kTickMs = 50;
constexpr int kStepPx = 6;                  // 틱당 이동 = 120px/초
constexpr uint32_t kBobMs = 150;            // 통통 뛰는 박자
constexpr int kBobPx = 6;
constexpr int kSeatedBobPx = 2;
constexpr uint32_t kGreetMs = 1600;         // 손님이 도착해서 인사하는 시간
constexpr uint32_t kTapMs = 1500;           // 손님을 만졌을 때 반응 시간
constexpr uint32_t kStayMs = 45000;         // 손님이 머무는 시간(인사 포함)
constexpr uint32_t kAwayMaxMs = kStayMs + 15000;   // 상대가 꺼져도 이 시간 뒤엔 돌아온다
constexpr int kFrameW = 98;
constexpr int kFrameH = 64;
constexpr int kFootInFrame = 62;            // 프레임에서 발이 닿는 줄
constexpr uint16_t kGuestScale256 = 512;    // 손님도 집주인과 같은 2×. 크기가 같아야 함께 있는 것처럼 보인다
constexpr int kGuestRightMarginPx = -8;     // 프레임 양옆이 비어 있어 살짝 밖으로 나가도 몸은 안 잘린다
constexpr int kBodyHitPadPx = 18;           // 프레임 가운데 몸 영역만 탭으로 친다
constexpr int kAwayLabelCenterY = 120;
constexpr int kAwayLabelScale = 2;

void SetVisible(lv_obj_t* obj, bool visible) {
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

const PackSet& PackSetOf(uint8_t character) {
    const CharacterPack& pack = kCharacterPacks[character - 1];
    return pack.sets[pack.default_set];
}

}  // namespace

// 만복이 몸·모자를 지금 방향으로 다시 그린다. 대기 자세일 때만.
void UsageScreen::RefreshHostBody() {
    if (CastActive() || state_ != kUsageStateIdle || !IsBboketerScene() || !CharacterBorrowsScenes()) return;
    figure_->SetBboketerScene(-1);
    ShowPackFrame(figure_.get(), walk_anim(), walk_leg_frame_);
}

bool UsageScreen::CanTrip() const {
    return trip_ == Trip::kNone && state_ == kUsageStateIdle && !idle_paused_ &&
           !lv_obj_has_flag(bg_image_, LV_OBJ_FLAG_HIDDEN);
}

void UsageScreen::EnsureTripObjects() {
    if (trip_img_ == nullptr) {
        trip_rozzi_img_ = lv_image_create(panel_);
        lv_image_set_antialias(trip_rozzi_img_, false);
        lv_image_set_pivot(trip_rozzi_img_, 0, 0);
        lv_obj_remove_flag(trip_rozzi_img_, LV_OBJ_FLAG_CLICKABLE);
        SetVisible(trip_rozzi_img_, false);
        trip_img_ = lv_image_create(panel_);
        lv_image_set_antialias(trip_img_, false);
        lv_image_set_pivot(trip_img_, 0, 0);
        lv_obj_remove_flag(trip_img_, LV_OBJ_FLAG_CLICKABLE);
        trip_label_ = lv_image_create(panel_);
        lv_image_set_src(trip_label_, &ui_poke_away_img);
        lv_image_set_antialias(trip_label_, false);
        lv_image_set_pivot(trip_label_, ui_poke_away_img.header.w / 2, ui_poke_away_img.header.h / 2);
        lv_image_set_scale(trip_label_, LV_SCALE_NONE * kAwayLabelScale);
        lv_obj_set_pos(trip_label_, (width_ - static_cast<int>(ui_poke_away_img.header.w)) / 2,
                       kAwayLabelCenterY - static_cast<int>(ui_poke_away_img.header.h) / 2);
        lv_obj_remove_flag(trip_label_, LV_OBJ_FLAG_CLICKABLE);
        SetVisible(trip_label_, false);
    }
    // 몸(figure_)은 캐릭터를 바꾸면 다시 만들어져 z 가 달라질 수 있어 매번 맞춘다. 글자·버튼 아래.
    lv_obj_move_to_index(trip_rozzi_img_, lv_obj_get_index(figure_->obj()) + 1);
    lv_obj_move_to_index(trip_img_, lv_obj_get_index(figure_->obj()) + 2);
    if (trip_timer_ == nullptr) {
        trip_timer_ = lv_timer_create(OnTripTick, kTickMs, this);
        trip_last_tick_ = lv_tick_get();
    }
}

void UsageScreen::StopTripSprite() {
    trip_src_ = nullptr;
    if (trip_img_ != nullptr) {
        lv_image_set_src(trip_img_, nullptr);
        SetVisible(trip_img_, false);
        SetVisible(trip_rozzi_img_, false);
    }
    if (trip_label_ != nullptr) {
        SetVisible(trip_label_, false);
    }
}

void UsageScreen::EndTrip() {
    trip_ = Trip::kNone;
    trip_away_mask_ = 0;
    trip_guest_mask_ = 0;
    trip_anim_ = nullptr;
    trip_scene_body_ = nullptr;
    trip_rozzi_ = nullptr;
    StopTripSprite();
    if (trip_timer_ != nullptr) {
        lv_timer_delete(trip_timer_);
        trip_timer_ = nullptr;
    }
    ApplyScenePlace();                                  // 로찌가 제자리로 돌아온다
    figure_->SetBodyPos(walk_x_center(), walk_y_);       // 비켜 있던 집주인도
    BboketerSetMirror(false);
    RefreshHostBody();
}

bool UsageScreen::StartTripLeave(uint8_t who) {
    if (!CanTrip()) {
        return false;
    }
    if (CastActive()) {
        const uint8_t going = CastGoing(who);   // 답을 기다리는 사이 꺼진 캐릭터는 못 간다
        if (going == 0) {
            return false;
        }
        StopWalk();
        trip_ = Trip::kLeaving;
        trip_away_mask_ = going;
        trip_x_ = 0;
        trip_home_x_ = 0;
        trip_t_ms_ = 0;
        cast_visible_ = true;
        ApplyScenePlace();
        EnsureTripObjects();
        ESP_LOGI(kTag, "leave %u", going);
        return true;
    }
    const uint8_t character = static_cast<uint8_t>(CurrentCharacter());
    const uint16_t scale = ActivePack().scale256;
    const int x = figure_->body_x() * scale / LV_SCALE_NONE;
    const int y = walk_y_ * scale / LV_SCALE_NONE;
    StopWalk();   // 몸을 숨기고 반응·걷기 타이머를 끊는다. trip_ 을 세우기 전에 불러야 한다
    trip_ = Trip::kLeaving;
    trip_anim_ = &PackSetOf(character).states[kUsageStateIdle];
    trip_scale_ = scale;
    trip_home_x_ = x;
    trip_x_ = x;
    trip_y_ = y;
    trip_t_ms_ = 0;
    trip_scene_body_ = nullptr;
    trip_rozzi_ = nullptr;
    if (IsBboketerScene() && !CharacterBorrowsScenes()) {   // 로찌도 같이 나간다
        const BboketerPlace place = BboketerScenePlace(BodyScene());
        trip_rozzi_ = BboketerRozziImage(BodyScene());
        trip_rozzi_dx_ = place.rozzi_x - x;
        trip_rozzi_dy_ = place.rozzi_dy;
    }
    ApplyScenePlace();
    EnsureTripObjects();
    DrawTrip();
    ESP_LOGI(kTag, "leave");
    return true;
}

bool UsageScreen::StartTripGuest(uint8_t character) {
    if (CastActive()) {   // character 는 들어올 캐릭터 비트 묶음이다
        if (!CanTrip() || character == 0 || character > 7) {
            return false;
        }
        trip_ = Trip::kGuestEnter;
        trip_guest_mask_ = character;
        trip_x_ = width_;
        trip_home_x_ = 0;
        trip_t_ms_ = 0;
        trip_anim_until_ms_ = 0;
        ApplyScenePlace();
        EnsureTripObjects();
        ESP_LOGI(kTag, "guest %u enter", character);
        return true;
    }
    if (!CanTrip() || character == 0 || character > kCharacterPackCount) {
        return false;
    }
    const PackSet& set = PackSetOf(character);
    trip_ = Trip::kGuestEnter;
    trip_guest_anim_ = &set.states[kUsageStateIdle];
    trip_greet_anim_ = set.tap.count > 0 ? &set.tap : trip_guest_anim_;
    trip_anim_ = trip_guest_anim_;
    trip_scale_ = kGuestScale256;
    const int foot_y = (walk_y_ground_ + kFrameH) * ActivePack().scale256 / LV_SCALE_NONE;
    const int frame_w = kFrameW * kGuestScale256 / LV_SCALE_NONE;
    trip_home_x_ = width_ - frame_w - kGuestRightMarginPx;
    trip_x_ = width_;
    trip_y_ = foot_y - kFrameH * kGuestScale256 / LV_SCALE_NONE;
    trip_t_ms_ = 0;
    trip_anim_until_ms_ = 0;
    trip_scene_body_ = nullptr;
    trip_rozzi_ = nullptr;
    if (IsBboketerScene()) {
        // 손님도 그 장면의 활동을 한다. 로나가 오면 로찌도 같이 온다.
        const int scene = BodyScene();
        const char* prefix = kCharacterPacks[character - 1].scene_prefix;
        const bool rona = prefix[0] == '\0';
        BboketerSetMirror(scene == kBabyRoomScene);     // 손님 그림을 고르기 전에 정한다
        trip_scene_body_ = BboketerGuestBody(scene, prefix);
        const BboketerGuestPlace guest = BboketerSceneGuestPlace(scene);
        trip_home_x_ = rona ? guest.rona : guest.manbok;
        trip_y_ = BboketerScenePlace(scene).foot - kFootInFrame * kGuestScale256 / LV_SCALE_NONE +
                  (rona ? guest.rona_dy : guest.manbok_dy);
        if (rona) {
            trip_rozzi_ = BboketerRozziImage(scene);
            trip_rozzi_dx_ = guest.rozzi - guest.rona;
            trip_rozzi_dy_ = guest.rozzi_dy - guest.rona_dy;
        }
    }
    ApplyScenePlace();                                  // 내 로찌가 자리를 비켜 준다
    figure_->SetBodyPos(walk_x_center(), walk_y_);       // 집주인도
    RefreshHostBody();                                  // 아기방의 만복이 집주인을 뒤집는다
    EnsureTripObjects();
    DrawTrip();
    ESP_LOGI(kTag, "guest %u enter", character);
    return true;
}

void UsageScreen::StartTripReturn() {
    if (TripHasGuest()) {
        // 손님 쪽 보드가 먼저 끝냈다(옷장에서 캐릭터를 바꿈). 같은 캐릭터가 두 보드에 보이지 않게 바로 내보낸다.
        trip_ = Trip::kGuestExit;
        trip_t_ms_ = 0;
        trip_anim_until_ms_ = 0;
        return;
    }
    if (trip_ != Trip::kLeaving && trip_ != Trip::kAway) {
        return;
    }
    if (state_ != kUsageStateIdle || idle_paused_ || lv_obj_has_flag(bg_image_, LV_OBJ_FLAG_HIDDEN)) {
        EndTrip();   // 상태 화면·옷장·꺼진 화면 중이면 그냥 끝낸다. 대기 화면으로 오면 몸이 다시 나온다
        return;
    }
    if (trip_label_ != nullptr) {
        SetVisible(trip_label_, false);
    }
    trip_ = Trip::kReturning;
    trip_x_ = width_;
    trip_t_ms_ = 0;
    if (CastActive()) ApplyScenePlace();   // 남아 있던 캐릭터가 원래 자리로 돌아간다
    EnsureTripObjects();
    DrawTrip();
    ESP_LOGI(kTag, "return");
}

bool UsageScreen::SendGuestHome() {
    if (trip_ != Trip::kGuestEnter && trip_ != Trip::kGuestStay) {
        return false;
    }
    ESP_LOGI(kTag, "guest sent home");
    trip_ = Trip::kGuestExit;   // 화면 밖으로 나가면 TripTick 이 상대에게 「떠났다」를 보낸다
    trip_anim_ = trip_guest_anim_;
    trip_t_ms_ = 0;
    trip_anim_until_ms_ = 0;
    return true;
}

bool UsageScreen::TripRecall() {
    if (trip_ != Trip::kLeaving && trip_ != Trip::kAway) {
        return false;
    }
    ESP_LOGI(kTag, "recall");   // 상대가 못 받으면 PokeLink 가 바로 돌려보낸다(ShowTripHome)
    return true;
}

// 상태 화면·화면 끄기·옷장이 대기 화면을 걷어 낼 때(StopWalk) 부른다.
void UsageScreen::TripOnStopWalk() {
    switch (trip_) {
        case Trip::kLeaving:
            trip_ = Trip::kAway;     // 이미 상대 보드로 출발했다. 돌아오는 신호는 그대로 기다린다
            trip_t_ms_ = 0;
            StopTripSprite();
            break;
        case Trip::kAway:
            StopTripSprite();        // 글자만 숨긴다. 대기 화면으로 오면 TripOnIdleResume 이 켠다
            break;
        case Trip::kReturning:
            EndTrip();
            break;
        case Trip::kGuestEnter:
        case Trip::kGuestStay:
        case Trip::kGuestExit:
            EndTrip();
            PokeLink::GetInstance().SendTripHome();
            break;
        case Trip::kNone:
            break;
    }
}

void UsageScreen::TripOnIdleResume() {
    // 사용량 푸시마다 여기로 온다. 이미 보이면 건드리지 않는다 — 패널·하트를 덮지 않게.
    if (trip_ == Trip::kAway && trip_label_ != nullptr && lv_obj_has_flag(trip_label_, LV_OBJ_FLAG_HIDDEN) &&
        (!CastActive() || CastDrawMask() == 0)) {   // 남은 캐릭터가 있으면 글자를 띄우지 않는다
        lv_obj_move_foreground(trip_label_);
        SetVisible(trip_label_, true);
    }
}

bool UsageScreen::HitTripGuest(int x, int y) const {
    if (trip_ != Trip::kGuestStay) {
        return false;
    }
    const int w = kFrameW * trip_scale_ / LV_SCALE_NONE;
    const int h = kFrameH * trip_scale_ / LV_SCALE_NONE;
    return x >= trip_x_ + kBodyHitPadPx && x < trip_x_ + w - kBodyHitPadPx && y >= trip_y_ && y < trip_y_ + h;
}

void UsageScreen::OnTripGuestTap() {
    trip_anim_ = trip_greet_anim_;
    trip_anim_until_ms_ = trip_t_ms_ + kTapMs;
    ShowPokePop(0);   // 큰 하트 + 집주인 반응. 콕을 받은 화면과 같다
}

void UsageScreen::DrawTrip() {
    if (CastActive()) {
        ApplyCast();
        return;
    }
    if (trip_img_ == nullptr || trip_anim_ == nullptr || trip_anim_->count == 0) {
        return;
    }
    const uint32_t frame_ms = trip_anim_->frame_ms > 0 ? trip_anim_->frame_ms : 250;
    const lv_image_dsc_t* src = trip_anim_->frames[(trip_t_ms_ / frame_ms) % trip_anim_->count];
    if (trip_ == Trip::kGuestStay && trip_anim_ == trip_guest_anim_ && trip_scene_body_ != nullptr) {
        src = trip_scene_body_;
    }
    // 라인아트(1배)는 옛 프레임(98×64 를 trip_scale_ 배)의 아래 가운데에 그 크기로 놓는다.
    const int frame_w = kFrameW * trip_scale_ / LV_SCALE_NONE, frame_h = kFrameH * trip_scale_ / LV_SCALE_NONE;
    const bool lineart = (src->header.flags & LV_IMAGE_FLAGS_USER1) != 0;
    const int img_dx = lineart ? frame_w / 2 - static_cast<int>(src->header.w) / 2 : 0;
    const int img_dy = lineart ? frame_h - static_cast<int>(src->header.h) : 0;
    if (src != trip_src_) {
        lv_image_set_src(trip_img_, src);
        lv_image_set_pivot(trip_img_, 0, 0);
        lv_image_set_scale(trip_img_, lineart ? LV_SCALE_NONE : trip_scale_);
        lv_image_set_antialias(trip_img_, lineart);
        trip_src_ = src;
    }
    const bool moving = trip_ == Trip::kLeaving || trip_ == Trip::kReturning || trip_ == Trip::kGuestEnter ||
                        trip_ == Trip::kGuestExit;
    int bob = moving && ((trip_t_ms_ / kBobMs) & 1) != 0 ? kBobPx : 0;
    if (trip_ == Trip::kGuestStay && !scene_bob_up_) {
        bob = kSeatedBobPx;                             // 앉은 손님은 집주인과 번갈아 두둥실거린다
    }
    lv_obj_set_pos(trip_img_, trip_x_ + img_dx, trip_y_ + img_dy - bob);
    SetVisible(trip_img_, true);
    if (trip_rozzi_ != nullptr) {
        lv_image_set_src(trip_rozzi_img_, trip_rozzi_);
        lv_image_set_pivot(trip_rozzi_img_, 0, 0);
        if ((trip_rozzi_->header.flags & LV_IMAGE_FLAGS_USER1) != 0) {
            // 로찌 자리 = 64×64 2배 프레임 왼쪽. 그 아래 가운데에 놓는다.
            lv_image_set_scale(trip_rozzi_img_, LV_SCALE_NONE);
            lv_image_set_antialias(trip_rozzi_img_, true);
            lv_obj_set_pos(trip_rozzi_img_, trip_x_ + trip_rozzi_dx_ + 64 - static_cast<int>(trip_rozzi_->header.w) / 2,
                           trip_y_ + frame_h + trip_rozzi_dy_ - static_cast<int>(trip_rozzi_->header.h) - bob);
        } else {
            lv_image_set_scale(trip_rozzi_img_, trip_scale_);
            lv_obj_set_pos(trip_rozzi_img_, trip_x_ + trip_rozzi_dx_, trip_y_ - bob);
        }
    }
    RaiseFrontProp();   // 앞 탁자를 손님 위로
    SetVisible(trip_rozzi_img_, trip_rozzi_ != nullptr);
}

void UsageScreen::OnTripTick(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->TripTick();
    }
}

void UsageScreen::TripTick() {
    // 틱 수가 아니라 실제 시간으로 잰다. 화면이 밀려 틱이 늦어도 두 보드의 시한이 어긋나지 않게 한다.
    const uint32_t elapsed_ms = lv_tick_elaps(trip_last_tick_);
    trip_t_ms_ += elapsed_ms;
    trip_last_tick_ = lv_tick_get();
    // 걷기도 실제 시간으로 움직인다. 2배 그림은 틱이 130ms 까지 늦어져 틱당 고정 이동이면 3배 느려진다.
    const int step_px = LV_MAX(kStepPx, kStepPx * static_cast<int>(elapsed_ms) / static_cast<int>(kTickMs));
    switch (trip_) {
        case Trip::kLeaving:
            trip_x_ += step_px;
            if (trip_x_ >= width_) {
                trip_ = Trip::kAway;
                trip_t_ms_ = 0;
                StopTripSprite();
                if (CastActive()) ApplyScenePlace();   // 남은 캐릭터가 혼자 자리로 옮긴다
                TripOnIdleResume();
                return;
            }
            break;
        case Trip::kAway:
            if (trip_t_ms_ >= kAwayMaxMs) {
                ESP_LOGW(kTag, "no word from peer, coming home");
                StartTripReturn();
            }
            return;
        case Trip::kReturning:
            trip_x_ -= step_px;
            if (trip_x_ <= trip_home_x_) {
                EndTrip();
                StartWalk();
                return;
            }
            break;
        case Trip::kGuestEnter:
            trip_x_ -= step_px;
            if (trip_x_ <= trip_home_x_) {
                trip_x_ = trip_home_x_;
                trip_ = Trip::kGuestStay;
                trip_t_ms_ = 0;
                trip_anim_ = trip_greet_anim_;
                trip_anim_until_ms_ = kGreetMs;
                BeginPackReaction(ReactionAnim(PackReaction::kHighFive), kGreetMs);
            }
            break;
        case Trip::kGuestStay:
            if (trip_anim_until_ms_ != 0 && trip_t_ms_ >= trip_anim_until_ms_) {
                trip_anim_until_ms_ = 0;
                trip_anim_ = trip_guest_anim_;
            }
            if (trip_t_ms_ >= kStayMs) {   // 만지고 있어도 떠난다. 상대의 안전 시한을 넘기면 두 곳에 나온다
                trip_ = Trip::kGuestExit;
                trip_anim_ = trip_guest_anim_;
                trip_anim_until_ms_ = 0;
                trip_t_ms_ = 0;
                ESP_LOGI(kTag, "guest exit");
            }
            break;
        case Trip::kGuestExit:
            trip_x_ += step_px;
            if (trip_x_ >= width_) {
                EndTrip();
                PokeLink::GetInstance().SendTripHome();
                return;
            }
            break;
        case Trip::kNone:
            EndTrip();
            return;
    }
    DrawTrip();
}
