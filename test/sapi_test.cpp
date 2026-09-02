// SAPI 5 end-to-end test for the CyberTalk voices (no ATL required).
//
//   sapi_test <output dir> [text]
//
// Enumerates the installed SAPI voices and, for every CyberTalk voice, renders
// WAV files through the normal SAPI pipeline (ISpVoice -> CyberTalkSAPI.dll ->
// CyberTalkHost.exe -> STLTTS.EXE), printing word / sentence / bookmark events,
// then exercises rate, volume, XML tags and cancellation.  Build it for x86
// and x64 to test both engine DLLs.
#include <windows.h>
#include <initguid.h>
#include <sapi.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() {}
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
    void reset() { if (p) p->Release(); p = nullptr; }
};

static std::wstring key_string(ISpDataKey* key, const wchar_t* name) {
    if (!key) return L"";
    LPWSTR s = nullptr;
    if (FAILED(key->GetStringValue(name, &s)) || !s) return L"";
    std::wstring r = s;
    CoTaskMemFree(s);
    return r;
}

static void free_event(SPEVENT& ev) {
    if (ev.elParamType == SPET_LPARAM_IS_POINTER || ev.elParamType == SPET_LPARAM_IS_STRING) {
        if (ev.lParam) CoTaskMemFree(reinterpret_cast<void*>(ev.lParam));
    } else if (ev.elParamType == SPET_LPARAM_IS_TOKEN || ev.elParamType == SPET_LPARAM_IS_OBJECT) {
        if (ev.lParam) reinterpret_cast<IUnknown*>(ev.lParam)->Release();
    }
}

static void drain_events(ISpVoice* voice, const char* label) {
    SPEVENT ev;
    ULONG fetched = 0;
    for (;;) {
        memset(&ev, 0, sizeof(ev));
        if (FAILED(voice->GetEvents(1, &ev, &fetched)) || fetched != 1) break;
        switch (ev.eEventId) {
        case SPEI_WORD_BOUNDARY:
            printf("  [%s] word     audio=%llu chars %lu..+%lu\n", label, ev.ullAudioStreamOffset, (unsigned long)ev.lParam, (unsigned long)ev.wParam);
            break;
        case SPEI_SENTENCE_BOUNDARY:
            printf("  [%s] sentence audio=%llu chars %lu..+%lu\n", label, ev.ullAudioStreamOffset, (unsigned long)ev.lParam, (unsigned long)ev.wParam);
            break;
        case SPEI_TTS_BOOKMARK:
            printf("  [%s] bookmark audio=%llu \"%S\" (%lu)\n", label, ev.ullAudioStreamOffset,
                   ev.elParamType == SPET_LPARAM_IS_STRING && ev.lParam ? reinterpret_cast<const wchar_t*>(ev.lParam) : L"", (unsigned long)ev.wParam);
            break;
        case SPEI_START_INPUT_STREAM: printf("  [%s] start of stream\n", label); break;
        case SPEI_END_INPUT_STREAM: printf("  [%s] end of stream\n", label); break;
        default: break;
        }
        free_event(ev);
    }
}

static bool render(ISpObjectToken* token, const std::wstring& name, const std::wstring& outdir, const std::wstring& text,
                   const wchar_t* suffix, long rate, USHORT volume, bool cancel_test) {
    ComPtr<ISpVoice> voice;
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice));
    if (FAILED(hr)) { printf("CoCreateInstance(SpVoice) failed 0x%08lX\n", hr); return false; }
    hr = voice->SetVoice(token);
    if (FAILED(hr)) { printf("SetVoice failed 0x%08lX\n", hr); return false; }
    voice->SetRate(rate);
    voice->SetVolume(volume);

    ComPtr<ISpStream> stream;
    hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, reinterpret_cast<void**>(&stream));
    if (FAILED(hr)) { printf("CoCreateInstance(SpStream) failed 0x%08lX\n", hr); return false; }
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 11025;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = 22050;
    std::wstring file = outdir + L"\\sapi_" + name + L"_" + suffix + L".wav";
    hr = stream->BindToFile(file.c_str(), SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &wfx, 0);
    if (FAILED(hr)) { printf("BindToFile failed 0x%08lX\n", hr); return false; }
    hr = voice->SetOutput(stream, TRUE);
    if (FAILED(hr)) { printf("SetOutput failed 0x%08lX\n", hr); return false; }

    const ULONGLONG interest = SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_SENTENCE_BOUNDARY) | SPFEI(SPEI_TTS_BOOKMARK) |
                               SPFEI(SPEI_END_INPUT_STREAM) | SPFEI(SPEI_START_INPUT_STREAM);
    voice->SetInterest(interest, interest);
    voice->SetNotifyWin32Event();
    HANDLE notify = voice->GetNotifyEventHandle();

    char label[128];
    snprintf(label, sizeof(label), "%S/%S", name.c_str(), suffix);
    DWORD t0 = GetTickCount();
    hr = voice->Speak(text.c_str(), SPF_ASYNC | SPF_IS_XML, nullptr);
    if (FAILED(hr)) { printf("Speak failed 0x%08lX\n", hr); return false; }
    if (cancel_test) {
        Sleep(300);
        DWORD tc = GetTickCount();
        hr = voice->Speak(L"Cancelled and replaced.", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
        printf("  [%s] purge+speak after 300 ms -> 0x%08lX (returned in %lu ms)\n", label, hr, GetTickCount() - tc);
    }
    (void)notify;
    for (;;) {
        HRESULT w = voice->WaitUntilDone(200);
        drain_events(voice, label);
        if (w == S_OK) break;
        if (GetTickCount() - t0 > 120000) { printf("  [%s] TIMEOUT\n", label); break; }
    }
    drain_events(voice, label);
    stream->Close();
    printf("  [%s] wrote %S in %lu ms\n", label, file.c_str(), GetTickCount() - t0);
    return true;
}

// Speaks through the default audio device, printing every event with a
// millisecond timestamp, then purges after `purge_after_ms` (0 = no purge)
// and reports how long the purge took: the screen-reader scenario.
static bool render_audio(ISpObjectToken* token, const std::wstring& text, DWORD purge_after_ms, const char* label) {
    ComPtr<ISpVoice> voice;
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice));
    if (FAILED(hr)) { printf("CoCreateInstance(SpVoice) failed 0x%08lX\n", hr); return false; }
    hr = voice->SetVoice(token);
    if (FAILED(hr)) { printf("SetVoice failed 0x%08lX\n", hr); return false; }
    hr = voice->SetOutput(nullptr, TRUE);
    if (FAILED(hr)) { printf("SetOutput(default audio) failed 0x%08lX\n", hr); return false; }
    const ULONGLONG interest = SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_SENTENCE_BOUNDARY) | SPFEI(SPEI_TTS_BOOKMARK) |
                               SPFEI(SPEI_END_INPUT_STREAM) | SPFEI(SPEI_START_INPUT_STREAM);
    voice->SetInterest(interest, interest);
    DWORD t0 = GetTickCount();
    hr = voice->Speak(text.c_str(), SPF_ASYNC | SPF_IS_XML, nullptr);
    printf("  [%s] Speak(async) returned 0x%08lX after %lu ms\n", label, hr, GetTickCount() - t0);
    bool purged = false;
    for (;;) {
        HRESULT w = voice->WaitUntilDone(20);
        SPEVENT ev;
        ULONG fetched = 0;
        for (;;) {
            memset(&ev, 0, sizeof(ev));
            if (FAILED(voice->GetEvents(1, &ev, &fetched)) || fetched != 1) break;
            const char* kind = ev.eEventId == SPEI_WORD_BOUNDARY ? "word" : ev.eEventId == SPEI_SENTENCE_BOUNDARY ? "sentence" :
                               ev.eEventId == SPEI_TTS_BOOKMARK ? "bookmark" : ev.eEventId == SPEI_START_INPUT_STREAM ? "start" :
                               ev.eEventId == SPEI_END_INPUT_STREAM ? "end" : "other";
            printf("  [%s] +%5lu ms %-8s audio=%llu wParam=%lu lParam=%S\n", label, GetTickCount() - t0, kind, ev.ullAudioStreamOffset,
                   (unsigned long)ev.wParam, ev.elParamType == SPET_LPARAM_IS_STRING && ev.lParam ? reinterpret_cast<const wchar_t*>(ev.lParam) : L"");
            free_event(ev);
        }
        if (w == S_OK) break;
        if (!purged && purge_after_ms && GetTickCount() - t0 >= purge_after_ms) {
            purged = true;
            DWORD tc = GetTickCount();
            hr = voice->Speak(L"Cancelled and replaced.", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
            printf("  [%s] +%5lu ms PURGE -> 0x%08lX, call took %lu ms\n", label, GetTickCount() - t0, hr, GetTickCount() - tc);
        }
        if (GetTickCount() - t0 > 120000) { printf("  [%s] TIMEOUT\n", label); break; }
    }
    printf("  [%s] finished after %lu ms\n", label, GetTickCount() - t0);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: sapi_test <output dir> [text]\n"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring outdir = argv[1];
    CreateDirectoryW(outdir.c_str(), nullptr);
    // extra arguments: a token id (starts with HKEY_) selects a voice directly
    // without enumerating; anything else replaces the test text
    std::wstring text =
        L"<bookmark mark=\"1\"/>Hello, this is CyberTalk speaking through SAPI 5. <bookmark mark=\"2\"/>The total is $19.99 for 42 items. "
        L"<pitch absmiddle=\"10\">Higher pitch here.</pitch> <bookmark mark=\"3\"/>Done.";
    std::vector<std::wstring> token_ids;
    bool audio_mode = false;
    for (int i = 2; i < argc; i++) {
        std::wstring a = argv[i];
        if (a.rfind(L"HKEY_", 0) == 0) token_ids.push_back(a);
        else if (a == L"--audio") audio_mode = true;
        else text = a;
    }
    CoInitialize(nullptr);
    printf("SAPI test (%d-bit)\n", (int)(sizeof(void*) * 8));

    std::vector<ISpObjectToken*> tokens;
    HRESULT hr;
    if (!token_ids.empty()) {
        for (const std::wstring& id : token_ids) {
            ISpObjectToken* t = nullptr;
            hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_ISpObjectToken, reinterpret_cast<void**>(&t));
            if (SUCCEEDED(hr)) hr = t->SetId(nullptr, id.c_str(), FALSE);
            if (FAILED(hr)) {
                printf("token %S: 0x%08lX\n", id.c_str(), hr);
                if (t) t->Release();
                continue;
            }
            tokens.push_back(t);
        }
    } else {
        ComPtr<ISpObjectTokenCategory> cat;
        hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, reinterpret_cast<void**>(&cat));
        if (FAILED(hr)) { printf("CoCreateInstance(SpObjectTokenCategory) failed 0x%08lX\n", hr); return 1; }
        hr = cat->SetId(SPCAT_VOICES, FALSE);
        if (FAILED(hr)) { printf("SetId failed 0x%08lX\n", hr); return 1; }
        ComPtr<IEnumSpObjectTokens> en;
        // only our own tokens: walking every installed voice would also exercise
        // other vendors' enumerators, which is not what this test is for
        hr = cat->EnumTokens(L"Vendor=Panasonic", nullptr, &en);
        if (FAILED(hr)) { printf("EnumTokens failed 0x%08lX\n", hr); return 1; }
        ULONG count = 0;
        en->GetCount(&count);
        printf("%lu Panasonic voices installed\n", count);
        for (;;) {
            ISpObjectToken* t = nullptr;
            if (en->Next(1, &t, nullptr) != S_OK) break;
            tokens.push_back(t);
        }
    }

    int tested = 0;
    for (ISpObjectToken* token : tokens) {
        std::wstring name = key_string(token, nullptr);
        ComPtr<ISpDataKey> attr;
        token->OpenKey(L"Attributes", &attr);
        std::wstring vendor = key_string(attr, L"Vendor"), gender = key_string(attr, L"Gender"), lang = key_string(attr, L"Language");
        LPWSTR id = nullptr;
        token->GetId(&id);
        printf("voice: %S (vendor=%S gender=%S lang=%S)\n        id=%S\n", name.c_str(), vendor.c_str(), gender.c_str(), lang.c_str(), id ? id : L"");
        if (id) CoTaskMemFree(id);
        if (name.find(L"CyberTalk") == std::wstring::npos) continue;
        std::wstring safe = name;
        for (auto& c : safe) if (c == L' ') c = L'_';
        if (audio_mode) {
            // live audio: short utterance (latency), then a long one purged after 1.5 s (cancel latency)
            render_audio(token, L"<bookmark mark=\"a\"/>Quick test.<bookmark mark=\"b\"/>", 0, "audio-short");
            render_audio(token, L"Second utterance right after the first one.", 0, "audio-second");
            std::wstring long_text;
            for (int n = 1; n <= 20; n++)
                long_text += L"<bookmark mark=\"" + std::to_wstring(n) + L"\"/>Sentence number " + std::to_wstring(n) + L" of a long text that will be cut off. ";
            render_audio(token, long_text, 1500, "audio-cancel");
            tested++;
            continue;
        }
        render(token, safe, outdir, text, L"default", 0, 100, false);
        if (name.find(L"Male") != std::wstring::npos) {
            render(token, safe, outdir, text, L"rate_plus5", 5, 100, false);
            render(token, safe, outdir, text, L"rate_plus10", 10, 100, false);
            render(token, safe, outdir, text, L"rate_minus5", -5, 100, false);
            render(token, safe, outdir, text, L"volume_50", 0, 50, false);
            // the engine renders about 200 times faster than real time, so the
            // text has to be very long for a cancel after 300 ms to land mid-utterance
            std::wstring long_text;
            for (int n = 1; n <= 60; n++)
                long_text += L"Sentence number " + std::to_wstring(n) + L" of this very long text is going to be cancelled after a short while. ";
            render(token, safe, outdir, long_text, L"cancel", 0, 100, true);
            render(token, safe, outdir, L"café naïve “smart quotes” 25° and a back\\slash", L"unicode", 0, 100, false);
            render(token, safe, outdir,
                   L"<spell>NVDA</spell> spelled, then <silence msec=\"800\"/> after a silence, <volume level=\"30\">quiet part</volume> normal again. "
                   L"<rate absspeed=\"8\">Fast part.</rate> <pitch absmiddle=\"-8\">Low part.</pitch>",
                   L"xml", 0, 100, false);
        }
        tested++;
    }
    for (ISpObjectToken* t : tokens) t->Release();
    printf("tested %d CyberTalk voice(s)\n", tested);
    CoUninitialize();
    printf("exiting\n");
    return tested > 0 ? 0 : 1;
}
