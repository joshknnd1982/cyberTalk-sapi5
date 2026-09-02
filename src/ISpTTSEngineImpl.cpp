#include <new>
#include <string>
#include <cmath>
#include <algorithm>
#include <vector>
#include <mutex>
#include <cwctype>

#include "utils.hpp"
#include "ISpTTSEngineImpl.hpp"
#include "ct_log.h"
#include "ct_settings.h"
#include "pipe_client.h"
#include "stl_text.h"

namespace CyberTalk {
namespace sapi {

namespace {

constexpr WORD AUDIO_CHANNELS = 1;
constexpr DWORD AUDIO_SAMPLE_RATE = 11025;
constexpr WORD AUDIO_BITS_PER_SAMPLE = 16;

constexpr int ENGINE_SPEED_MIN = 100, ENGINE_SPEED_MAX = 300;
constexpr int ENGINE_VOLUME_MAX = 255;

// One pipe connection per process, shared by every voice object.
PipeClient& pipe() {
    static PipeClient client;
    return client;
}
std::mutex g_speak_mutex;

struct SettingsCache {
    ctsettings::Settings settings;
    FILETIME mtime = {};
    bool loaded = false;
    DWORD last_check = 0;
    std::mutex mutex;

    const ctsettings::Settings& get() {
        std::lock_guard<std::mutex> lock(mutex);
        DWORD now = GetTickCount();
        if (!loaded || now - last_check > 500) {
            last_check = now;
            std::wstring path = ctsettings::settings_path();
            WIN32_FILE_ATTRIBUTE_DATA fad;
            bool exists = !path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad);
            if (!loaded || (exists && CompareFileTime(&fad.ftLastWriteTime, &mtime) != 0)) {
                ctsettings::Settings s;
                ctsettings::load(s, &mtime);
                settings = s;
                loaded = true;
                ctlog::Logger::instance().set_enabled(settings.logging != 0);
                CT_LOG("settings loaded: custom gender=%d pitch=%d speed=%d volume=%d bright=%d dollar=%d zero=%d numbers=%d spreadsheet=%d list=%d rate_boost=%d word_events=%d",
                       s.custom.gender, s.custom.pitch, s.custom.speed, s.custom.volume, s.custom.bright, s.custom.dollar, s.custom.zero,
                       s.custom.number_samples, s.custom.spreadsheet, s.custom.list, s.rate_boost, s.word_events);
            }
        }
        return settings;
    }
} g_settings;

voice_params params_for_voice(int index, const ctsettings::Settings& s) {
    voice_params p;
    if (index == VOICE_FEMALE) {
        p.gender = 1; p.pitch = 195; p.speed = 200; p.volume = 255; p.bright = 6; p.modes = MODE_FLAG_DOLLAR;
    } else if (index == VOICE_CUSTOM) {
        p.gender = s.custom.gender;
        p.pitch = s.custom.pitch;
        p.speed = s.custom.speed;
        p.volume = s.custom.volume;
        p.bright = s.custom.bright;
        p.modes = (s.custom.dollar ? MODE_FLAG_DOLLAR : 0) | (s.custom.zero ? MODE_FLAG_ZERO : 0) |
                  (s.custom.number_samples ? MODE_FLAG_NUMBER_SAMPLES : 0) | (s.custom.spreadsheet ? MODE_FLAG_SPREADSHEET : 0) |
                  (s.custom.list ? MODE_FLAG_LIST : 0);
    } else {
        p.gender = 2; p.pitch = 90; p.speed = 200; p.volume = 255; p.bright = 8; p.modes = MODE_FLAG_DOLLAR;
    }
    return p;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// SAPI rate -10..10 -> speed multiplier (3x at +10, 1/3 at -10)
double rate_multiplier(long rate) { return std::pow(3.0, clampi(static_cast<int>(rate), -10, 10) / 10.0); }

// SAPI pitch adjustment (-24..24) -> multiplier, one octave at +/-24
double pitch_multiplier(int adj) { return std::pow(2.0, clampi(adj, -24, 24) / 24.0); }

int pitch_for(int gender, int base_pitch, int adj) {
    int lo = ctsettings::pitch_min(gender), hi = ctsettings::pitch_max(gender);
    return clampi(static_cast<int>(std::lround(base_pitch * pitch_multiplier(adj))), lo, hi);
}

struct MarkInfo {
    enum Kind { BOOKMARK, SENTENCE } kind;
    std::wstring text;      // bookmark string
    ULONG src_offset;       // sentence: character offset
    ULONG src_len;
};

struct TextRef {
    int frag;        // index into the fragment array, -1 for tag bytes
    ULONG offset;    // character offset within the fragment
};

struct SpeakContext {
    ISpTTSEngineSite* site = nullptr;
    ULONGLONG bytes_written = 0;
    bool aborted = false;
    bool want_words = false;
    bool want_bookmarks = false;
    bool want_sentences = false;
    std::vector<MarkInfo> marks;            // every SAPI event carried by a mark
    std::vector<std::vector<uint32_t>> mark_groups;   // engine mark id - 1 -> indices into marks
    std::vector<TextRef> refs;              // per engine text byte
    std::vector<const SPVTEXTFRAG*> frags;
    std::vector<std::wstring> frag_text;    // copy of fragment texts (for word lengths)
    unsigned events_added = 0;
};

bool is_word_char(wchar_t c) { return std::iswalnum(c) || c == L'\'' || c == L'-' || c == L'_'; }

void add_event(SpeakContext& ctx, SPEVENT& ev) {
    HRESULT hr = ctx.site->AddEvents(&ev, 1);
    if (SUCCEEDED(hr)) ctx.events_added++;
    else CT_LOG("AddEvents failed 0x%08lX", hr);
}

}  // namespace

void shutdown_host() {
    pipe().shutdown_host();
}

ISpTTSEngineImpl::ISpTTSEngineImpl() : voice_index_(0) {}
ISpTTSEngineImpl::~ISpTTSEngineImpl() = default;

STDMETHODIMP ISpTTSEngineImpl::SetObjectToken(ISpObjectToken* pToken) {
    if (!pToken) return E_INVALIDARG;
    try {
        voice_index_ = 0;
        utils::out_ptr<wchar_t> idx(CoTaskMemFree);
        if (SUCCEEDED(pToken->GetStringValue(L"VoiceIndex", idx.address())) && idx.get()) {
            voice_index_ = clampi(_wtoi(idx.get()), 0, VOICE_COUNT - 1);
        } else {
            ISpDataKeyPtr attr;
            if (SUCCEEDED(pToken->OpenKey(L"Attributes", &attr))) {
                utils::out_ptr<wchar_t> name(CoTaskMemFree);
                if (SUCCEEDED(attr->GetStringValue(L"Name", name.address())) && name.get()) {
                    for (int i = 0; i < VOICE_COUNT; ++i)
                        if (_wcsicmp(voices[i].name, name.get()) == 0) voice_index_ = i;
                }
            }
        }
        token_ = pToken;
        CT_LOG("SetObjectToken: voice index %d (%S)", voice_index_, voices[voice_index_].name);
        return S_OK;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP ISpTTSEngineImpl::GetObjectToken(ISpObjectToken** ppToken) {
    if (!ppToken) return E_POINTER;
    *ppToken = nullptr;
    if (!token_) return E_UNEXPECTED;
    token_.AddRef();
    *ppToken = token_.GetInterfacePtr();
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::GetOutputFormat(const GUID*, const WAVEFORMATEX*, GUID* pOutputFormatId,
                                               WAVEFORMATEX** ppCoMemOutputWaveFormatEx) {
    if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) return E_POINTER;
    *pOutputFormatId = SPDFID_WaveFormatEx;
    auto* pwfex = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!pwfex) return E_OUTOFMEMORY;
    pwfex->wFormatTag = WAVE_FORMAT_PCM;
    pwfex->nChannels = AUDIO_CHANNELS;
    pwfex->nSamplesPerSec = AUDIO_SAMPLE_RATE;
    pwfex->wBitsPerSample = AUDIO_BITS_PER_SAMPLE;
    pwfex->nBlockAlign = pwfex->nChannels * pwfex->wBitsPerSample / 8;
    pwfex->nAvgBytesPerSec = pwfex->nSamplesPerSec * pwfex->nBlockAlign;
    pwfex->cbSize = 0;
    *ppCoMemOutputWaveFormatEx = pwfex;
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::Speak(DWORD dwSpeakFlags, REFGUID, const WAVEFORMATEX*,
                                     const SPVTEXTFRAG* pTextFragList, ISpTTSEngineSite* pOutputSite) {
    if (!pTextFragList || !pOutputSite) return E_INVALIDARG;
    try {
        std::lock_guard<std::mutex> lock(g_speak_mutex);
        const ctsettings::Settings& settings = g_settings.get();
        DWORD t0 = GetTickCount();

        long rate = 0;
        pOutputSite->GetRate(&rate);
        USHORT sapi_volume = 100;
        pOutputSite->GetVolume(&sapi_volume);
        ULONGLONG interest = 0;
        pOutputSite->GetEventInterest(&interest);

        SpeakContext ctx;
        ctx.site = pOutputSite;
        ctx.want_words = settings.word_events && (interest & SPFEI(SPEI_WORD_BOUNDARY));
        ctx.want_sentences = (interest & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;
        ctx.want_bookmarks = (interest & SPFEI(SPEI_TTS_BOOKMARK)) != 0;

        voice_params base = params_for_voice(voice_index_, settings);
        int base_gender = base.gender;

        // --- utterance-level parameters ------------------------------------
        const SPVTEXTFRAG* first_speak = nullptr;
        for (const SPVTEXTFRAG* f = pTextFragList; f; f = f->pNext)
            if (f->State.eAction == SPVA_Speak || f->State.eAction == SPVA_SpellOut || f->State.eAction == SPVA_Pronounce) { first_speak = f; break; }
        int first_rate_adj = first_speak ? first_speak->State.RateAdj : 0;
        int first_pitch_adj = first_speak ? first_speak->State.PitchAdj.MiddleAdj : 0;
        int first_vol = first_speak ? static_cast<int>(first_speak->State.Volume) : 100;

        double target_speed = base.speed * rate_multiplier(rate + first_rate_adj);
        int native_speed = clampi(static_cast<int>(std::lround(target_speed)), ENGINE_SPEED_MIN, ENGINE_SPEED_MAX);
        float sonic = 1.0f;
        if (settings.rate_boost && std::fabs(target_speed - native_speed) > 0.5)
            sonic = static_cast<float>(target_speed / native_speed);
        int utt_pitch = pitch_for(base_gender, base.pitch, first_pitch_adj);
        int utt_volume = clampi(static_cast<int>(std::lround(base.volume * (sapi_volume / 100.0) * (clampi(first_vol, 0, 100) / 100.0))), 0, ENGINE_VOLUME_MAX);

        // --- build the engine text -------------------------------------------
        std::string text;
        text.reserve(1024);
        auto append_tag = [&](const std::string& tag) {
            for (size_t k = 0; k < tag.size(); k++) ctx.refs.push_back({ -1, 0 });
            text += tag;
        };
        size_t text_len_after_mark = static_cast<size_t>(-1);
        auto new_mark = [&](MarkInfo::Kind kind, const std::wstring& s, ULONG off, ULONG len) {
            ctx.marks.push_back({ kind, s, off, len });
            const uint32_t idx = static_cast<uint32_t>(ctx.marks.size() - 1);
            if (!ctx.mark_groups.empty() && text.size() == text_len_after_mark) {
                // nothing speakable since the previous mark: the engine only
                // reports one of two back-to-back marks, so let that one mark
                // carry both events (e.g. a sentence start and a bookmark)
                ctx.mark_groups.back().push_back(idx);
                return;
            }
            ctx.mark_groups.push_back({ idx });
            append_tag("\\Mrk=" + std::to_string(ctx.mark_groups.size()) + "\\");
            text_len_after_mark = text.size();
        };
        int cur_speed = native_speed, cur_pitch = utt_pitch, cur_vol = utt_volume;
        int frag_index = 0;
        int frag_count = 0;
        for (const SPVTEXTFRAG* f = pTextFragList; f; f = f->pNext) frag_count++;
        ctx.frags.reserve(frag_count);
        ctx.frag_text.reserve(frag_count);

        for (const SPVTEXTFRAG* frag = pTextFragList; frag; frag = frag->pNext, frag_index++) {
            ctx.frags.push_back(frag);
            ctx.frag_text.emplace_back(frag->pTextStart ? frag->pTextStart : L"", frag->pTextStart ? frag->ulTextLen : 0);
            const SPVACTIONS action = frag->State.eAction;
            if (action == SPVA_Bookmark) {
                std::wstring name(frag->pTextStart ? frag->pTextStart : L"", frag->pTextStart ? frag->ulTextLen : 0);
                new_mark(MarkInfo::BOOKMARK, name, frag->ulTextSrcOffset, frag->ulTextLen);
                continue;
            }
            if (action == SPVA_Silence) {
                int ms = clampi(static_cast<int>(frag->State.SilenceMSecs), 0, 30000);
                if (ms > 0) append_tag("\\Pau=" + std::to_string(ms) + "\\ ");
                continue;
            }
            if (action != SPVA_Speak && action != SPVA_SpellOut && action != SPVA_Pronounce) continue;
            if (!frag->pTextStart || frag->ulTextLen == 0) continue;

            // per-fragment prosody through inline engine tags
            int f_speed = clampi(static_cast<int>(std::lround(base.speed * rate_multiplier(rate + frag->State.RateAdj))), ENGINE_SPEED_MIN, ENGINE_SPEED_MAX);
            int f_pitch = pitch_for(base_gender, base.pitch, frag->State.PitchAdj.MiddleAdj);
            int f_vol = clampi(static_cast<int>(std::lround(base.volume * (sapi_volume / 100.0) * (clampi(static_cast<int>(frag->State.Volume), 0, 100) / 100.0))), 0, ENGINE_VOLUME_MAX);
            if (f_speed != cur_speed) { append_tag("\\Spd=" + std::to_string(f_speed) + "\\"); cur_speed = f_speed; }
            if (f_pitch != cur_pitch) { append_tag("\\Pit=" + std::to_string(f_pitch) + "\\"); cur_pitch = f_pitch; }
            if (f_vol != cur_vol) { append_tag("\\Vol=" + std::to_string(f_vol * 257) + "\\"); cur_vol = f_vol; }

            if (ctx.want_sentences) new_mark(MarkInfo::SENTENCE, L"", frag->ulTextSrcOffset, frag->ulTextLen);

            std::vector<unsigned> offsets;
            std::string converted;
            if (action == SPVA_SpellOut) {
                // speak each character separately
                std::wstring spaced;
                std::vector<unsigned> src;
                for (ULONG i = 0; i < frag->ulTextLen; i++) {
                    wchar_t c = frag->pTextStart[i];
                    if (std::iswspace(c)) continue;
                    spaced += c;
                    spaced += L' ';
                }
                std::vector<unsigned> tmp;
                converted = stl::wide_to_engine_text(spaced.c_str(), spaced.size(), false, &tmp);
                // map spaced positions back to original character indexes
                std::vector<unsigned> spaced_to_src;
                for (ULONG i = 0; i < frag->ulTextLen; i++) {
                    if (std::iswspace(frag->pTextStart[i])) continue;
                    spaced_to_src.push_back(i);
                    spaced_to_src.push_back(i);
                }
                offsets.reserve(tmp.size());
                for (unsigned t : tmp) offsets.push_back(t < spaced_to_src.size() ? spaced_to_src[t] : 0);
            } else {
                converted = stl::wide_to_engine_text(frag->pTextStart, frag->ulTextLen, false, &offsets);
            }
            if (!text.empty() && text.back() != ' ' && text.back() != '\n') append_tag(" ");
            for (size_t k = 0; k < converted.size(); k++) ctx.refs.push_back({ frag_index, k < offsets.size() ? offsets[k] : 0 });
            text += converted;
        }
        if (!text.empty() && text.back() != ' ') append_tag(" ");

        // nothing speakable
        bool has_text = false;
        for (char c : text) if (c != ' ' && c != '\n' && c != '\t') { has_text = true; break; }
        if (!has_text) {
            CT_LOG("Speak: nothing to speak (%d fragments)", frag_count);
            return S_OK;
        }

        SpeakCommand cmd = {};
        cmd.gender = static_cast<uint32_t>(base_gender);
        cmd.pitch = static_cast<uint32_t>(utt_pitch);
        cmd.speed = static_cast<uint32_t>(native_speed);
        cmd.volume = static_cast<uint32_t>(utt_volume);
        cmd.bright = static_cast<uint32_t>(base.bright);
        cmd.modes = base.modes;
        cmd.sonic_speed = sonic;
        cmd.flags = 0;

        CT_LOG("Speak: voice=%d flags=0x%lX rate=%ld vol=%u frags=%d words=%d sentences=%d bookmarks=%d -> gender=%d pitch=%d speed=%d sonic=%.2f volume=%d bright=%d modes=0x%X text=\"%.200s%s\"",
               voice_index_, dwSpeakFlags, rate, sapi_volume, frag_count, ctx.want_words, ctx.want_sentences, ctx.want_bookmarks,
               base_gender, utt_pitch, native_speed, sonic, utt_volume, base.bright, base.modes, text.c_str(), text.size() > 200 ? "..." : "");

        // --- stream ------------------------------------------------------------
        PipeClient::SpeakEvents ev;
        // The engine hands over 11000-byte (half-second) chunks.  SAPI's Write
        // blocks while its audio buffer is full, and a purge is only noticed
        // once Write returns, so hand the audio to SAPI in small slices and
        // look at GetActions between them: that keeps cancel latency low.
        const ULONG WRITE_SLICE = 440;    // 20 ms of audio
        ev.audio = [&](const void* data, uint32_t size) -> bool {
            const DWORD actions = ctx.site->GetActions();
            if (actions & SPVES_ABORT) { ctx.aborted = true; return false; }
            if (actions & SPVES_SKIP) { ctx.site->CompleteSkip(0); ctx.aborted = true; return false; }
            const BYTE* p = static_cast<const BYTE*>(data);
            ULONG remaining = size;
            while (remaining > 0) {
                ULONG written = 0;
                const ULONG slice = remaining < WRITE_SLICE ? remaining : WRITE_SLICE;
                const DWORD tw = GetTickCount();
                HRESULT hr = ctx.site->Write(p, slice, &written);
                if (FAILED(hr)) { CT_LOG("site Write failed 0x%08lX", hr); ctx.aborted = true; return false; }
                if (written == 0 || written > slice) written = slice;   // SAPI does not reliably report pcbWritten
                ctx.bytes_written += written;
                remaining -= written;
                p += written;
                if (ctx.site->GetActions() & SPVES_ABORT) {
                    CT_LOG("Speak: abort seen after %llu bytes (last Write of %lu bytes took %lu ms)", ctx.bytes_written, slice, GetTickCount() - tw);
                    ctx.aborted = true;
                    return false;
                }
            }
            return true;
        };
        ev.mark = [&](uint32_t id, uint64_t offset) {
            if (id == 0 || id > ctx.mark_groups.size()) return;
            for (uint32_t idx : ctx.mark_groups[id - 1]) {
                const MarkInfo& m = ctx.marks[idx];
                SPEVENT e = {};
                e.ullAudioStreamOffset = offset;
                e.ulStreamNum = 0;
                if (m.kind == MarkInfo::BOOKMARK) {
                    if (!ctx.want_bookmarks) continue;
                    e.eEventId = SPEI_TTS_BOOKMARK;
                    e.elParamType = SPET_LPARAM_IS_STRING;
                    e.lParam = reinterpret_cast<LPARAM>(m.text.c_str());
                    e.wParam = static_cast<WPARAM>(_wtol(m.text.c_str()));
                } else {
                    e.eEventId = SPEI_SENTENCE_BOUNDARY;
                    e.elParamType = SPET_LPARAM_IS_UNDEFINED;
                    e.lParam = static_cast<LPARAM>(m.src_offset);
                    e.wParam = static_cast<WPARAM>(m.src_len);
                }
                add_event(ctx, e);
            }
        };
        ev.word = [&](uint32_t pos, uint64_t offset) {
            if (!ctx.want_words || pos >= ctx.refs.size()) return;
            const TextRef& r = ctx.refs[pos];
            if (r.frag < 0 || r.frag >= static_cast<int>(ctx.frags.size())) return;
            const std::wstring& ft = ctx.frag_text[r.frag];
            if (r.offset >= ft.size()) return;
            if (!is_word_char(ft[r.offset])) return;   // punctuation token
            ULONG start = r.offset;
            while (start > 0 && is_word_char(ft[start - 1])) start--;
            if (start != r.offset) return;              // not the first character of the word (e.g. spelled letter)
            ULONG end = r.offset;
            while (end < ft.size() && is_word_char(ft[end])) end++;
            SPEVENT e = {};
            e.eEventId = SPEI_WORD_BOUNDARY;
            e.elParamType = SPET_LPARAM_IS_UNDEFINED;
            e.ullAudioStreamOffset = offset;
            e.ulStreamNum = 0;
            e.lParam = static_cast<LPARAM>(ctx.frags[r.frag]->ulTextSrcOffset + start);
            e.wParam = static_cast<WPARAM>(end - start);
            add_event(ctx, e);
        };

        SpeakEndResponse end = {};
        bool ok = false;
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            if (!pipe().connect()) {
                CT_LOG("Speak: cannot connect to host: %s", pipe().last_error().c_str());
                break;
            }
            ok = pipe().speak(cmd, text, ev, end);
            if (!ok) {
                CT_LOG("Speak: pipe exchange failed (%s), %s", pipe().last_error().c_str(),
                       ctx.bytes_written == 0 && attempt == 0 ? "retrying" : "giving up");
                pipe().disconnect();
                if (ctx.bytes_written > 0 || ctx.aborted) break;
            }
        }
        CT_LOG("Speak: %s in %lu ms, %llu bytes (%.2fs), %u events, host status %lu hr 0x%08lX",
               ctx.aborted ? "aborted" : ok ? "done" : "FAILED", GetTickCount() - t0, ctx.bytes_written,
               ctx.bytes_written / 22050.0, ctx.events_added, (unsigned long)end.status, (unsigned long)end.hresult);
        if (!ok && ctx.bytes_written == 0 && !ctx.aborted) return E_FAIL;
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        CT_LOG("Speak: unexpected exception");
        return E_UNEXPECTED;
    }
}

}  // namespace sapi
}  // namespace CyberTalk
