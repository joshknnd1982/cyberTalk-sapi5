#pragma once

#include <string>
#include "ct_settings.h"

namespace CyberTalk {
namespace sapi {

// The three SAPI voices this engine exposes.  Male and Female use the engine's
// defaults; the Custom Voice follows settings.ini (see ct_settings.h).
enum VoiceIndex { VOICE_MALE = 0, VOICE_FEMALE = 1, VOICE_CUSTOM = 2, VOICE_COUNT = 3 };

struct voice_info {
    const wchar_t* name;
    int index;
};

inline constexpr voice_info voices[VOICE_COUNT] = {
    { L"CyberTalk Male", VOICE_MALE },
    { L"CyberTalk Female", VOICE_FEMALE },
    { L"CyberTalk Custom Voice", VOICE_CUSTOM },
};

// Engine parameters selected by a voice before SAPI rate/volume/pitch are applied.
struct voice_params {
    int gender = 2;
    int pitch = 90;
    int speed = 200;
    int volume = 255;
    int bright = 8;
    unsigned modes = 0;   // SpeakModeFlags from pipe_protocol.h
};

class voice_attributes {
public:
    explicit voice_attributes(int voice_index = 0) noexcept : index_(voice_index) {
        if (index_ < 0 || index_ >= VOICE_COUNT) index_ = 0;
    }

    [[nodiscard]] std::wstring get_name() const { return voices[index_].name; }
    [[nodiscard]] int get_index() const noexcept { return index_; }
    [[nodiscard]] std::wstring get_age() const { return L"Adult"; }
    [[nodiscard]] std::wstring get_language() const { return L"409"; }
    [[nodiscard]] std::wstring get_vendor() const { return L"Panasonic"; }

    [[nodiscard]] std::wstring get_gender() const {
        if (index_ == VOICE_FEMALE) return L"Female";
        if (index_ == VOICE_MALE) return L"Male";
        ctsettings::Settings s;
        ctsettings::load(s);
        return s.custom.gender == 1 ? L"Female" : L"Male";
    }

private:
    int index_;
};

}  // namespace sapi
}  // namespace CyberTalk
