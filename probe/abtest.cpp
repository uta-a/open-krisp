// abtest : 指定マイクを数秒録音し、Krisp NC 適用前後を WAV 出力して聴き比べる検証ツール。
//   usage: abtest.exe [秒数=8] [マイク名の一部]
//   例:    abtest.exe 8 fifine
// 出力: captured.wav（原音） / denoised.wav（Krisp適用後）
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

static const uintptr_t RVA_SIG_CHECK = 0x53110;
static const wchar_t* kModDir =
    L"C:/Users/utaaa/AppData/Local/Discord/app-1.0.9255/modules/"
    L"discord_krisp-1/discord_krisp";

typedef int  (*Init_t)();
typedef void*(*Setup_t)(int, int);
typedef int  (*Proc_t)(void*, const float*, size_t, float*, size_t);
typedef void (*Reset_t)(void*);

static HMODULE loadAndPatch() {
    SetDllDirectoryW(kModDir);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s/discord_krisp.node", kModDir);
    HMODULE h = LoadLibraryW(path);
    if (!h) { printf("LoadLibrary failed: %lu\n", GetLastError()); return nullptr; }
    uint8_t* t = (uint8_t*)h + RVA_SIG_CHECK;
    DWORD old; VirtualProtect(t, 3, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xB0; t[1] = 0x01; t[2] = 0xC3;
    VirtualProtect(t, 3, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 3);
    return h;
}

static std::wstring devName(IMMDevice* d) {
    IPropertyStore* ps = nullptr; std::wstring nm;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) nm = v.pwszVal;
        PropVariantClear(&v); ps->Release();
    }
    return nm;
}

static void writeWav(const char* path, const std::vector<float>& s, int sr) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    uint32_t db = (uint32_t)(s.size() * 4), fc = 16, br = sr * 4, riff = 36 + db;
    uint16_t tag = 3, ch = 1, bits = 32, ba = 4;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); fwrite(&fc,4,1,f); fwrite(&tag,2,1,f); fwrite(&ch,2,1,f);
    fwrite(&sr,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&db,4,1,f); fwrite(s.data(),1,db,f); fclose(f);
    printf("wrote %s (%.1fs)\n", path, s.size() / (double)sr);
}

int main(int argc, char** argv) {
    int seconds = (argc > 1) ? atoi(argv[1]) : 8;
    std::wstring want;
    if (argc > 2) { std::string a = argv[2]; want.assign(a.begin(), a.end());
        std::transform(want.begin(), want.end(), want.begin(), ::towlower); }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE h = loadAndPatch(); if (!h) return 1;
    auto Init=(Init_t)GetProcAddress(h,"KrispInitializeExternal");
    auto Setup=(Setup_t)GetProcAddress(h,"KrispNCSetup");
    auto Proc=(Proc_t)GetProcAddress(h,"KrispNCProcessFloat");
    auto Reset=(Reset_t)GetProcAddress(h,"KrispNCReset");
    if (Init()!=0){printf("init failed\n");return 2;}
    void* sess=Setup(48000,10); if(!sess){printf("setup failed\n");return 3;}

    // デバイス選択
    IMMDeviceEnumerator* en=nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&en);
    IMMDevice* dev=nullptr;
    if (want.empty()) {
        en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
    } else {
        IMMDeviceCollection* col=nullptr; en->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&col);
        UINT n=0; col->GetCount(&n);
        for(UINT i=0;i<n&&!dev;i++){IMMDevice* d=nullptr;col->Item(i,&d);
            std::wstring nm=devName(d),low=nm; std::transform(low.begin(),low.end(),low.begin(),::towlower);
            if(low.find(want)!=std::wstring::npos) dev=d; else d->Release();}
        col->Release();
    }
    if(!dev){printf("device not found\n");return 4;}
    wprintf(L"capture device: %s\n", devName(dev).c_str());

    IAudioClient* ac=nullptr; dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&ac);
    WAVEFORMATEX* mix=nullptr; ac->GetMixFormat(&mix);
    printf("mix: %uHz %uch %ubit\n", mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample);
    HANDLE ev=CreateEvent(nullptr,FALSE,FALSE,nullptr);
    ac->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,10000000,0,mix,nullptr);
    ac->SetEventHandle(ev);
    IAudioCaptureClient* cap=nullptr; ac->GetService(__uuidof(IAudioCaptureClient),(void**)&cap);
    int srcSr=mix->nSamplesPerSec, ch=mix->nChannels;
    bool isFloat = mix->wFormatTag==WAVE_FORMAT_IEEE_FLOAT ||
        (mix->wFormatTag==WAVE_FORMAT_EXTENSIBLE &&
         ((WAVEFORMATEXTENSIBLE*)mix)->SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    int bps=mix->wBitsPerSample/8;

    printf("\n=== %d秒間、話しながら周囲でノイズを出してください ===\n", seconds);
    std::vector<float> mono;
    ac->Start();
    DWORD idx=0; HANDLE mm=AvSetMmThreadCharacteristicsW(L"Pro Audio",&idx);
    ULONGLONG end=GetTickCount64()+(ULONGLONG)seconds*1000;
    while(GetTickCount64()<end){
        WaitForSingleObject(ev,200);
        UINT32 pk=0;
        while(SUCCEEDED(cap->GetNextPacketSize(&pk))&&pk>0){
            BYTE* data;UINT32 fr;DWORD fl;
            if(FAILED(cap->GetBuffer(&data,&fr,&fl,nullptr,nullptr)))break;
            bool sil=fl&AUDCLNT_BUFFERFLAGS_SILENT;
            for(UINT32 i=0;i<fr;i++){float a=0;if(!sil){const BYTE* p=data+(size_t)i*ch*bps;
                if(isFloat){const float* fp=(const float*)p;for(int c=0;c<ch;c++)a+=fp[c];}
                else if(bps==2){const int16_t* sp=(const int16_t*)p;for(int c=0;c<ch;c++)a+=sp[c]/32768.f;}
                a/=ch;} mono.push_back(a);}
            cap->ReleaseBuffer(fr);
        }
    }
    ac->Stop(); if(mm)AvRevertMmThreadCharacteristics(mm);

    // 48kへ線形リサンプル
    std::vector<float> in48;
    if(srcSr==48000) in48=mono;
    else{double r=48000.0/srcSr;size_t nn=(size_t)(mono.size()*r);in48.resize(nn);
        for(size_t i=0;i<nn;i++){double sp=i/r;size_t i0=(size_t)sp;double fr=sp-i0;
            float x=i0<mono.size()?mono[i0]:0,y=i0+1<mono.size()?mono[i0+1]:x;in48[i]=x+(y-x)*(float)fr;}}

    // Krisp 適用
    const size_t FR=480; std::vector<float> out48(in48.size(),0); std::vector<float> fi(FR),fo(FR);
    size_t frames=in48.size()/FR;
    double inSil=0,outSil=0;int silN=0; double inV=0,outV=0;int vN=0;
    for(size_t f=0;f<frames;f++){for(size_t i=0;i<FR;i++)fi[i]=in48[f*FR+i];
        Proc(sess,fi.data(),FR,fo.data(),FR);
        for(size_t i=0;i<FR;i++)out48[f*FR+i]=fo[i];
        double ri=0,ro=0;for(size_t i=0;i<FR;i++){ri+=fi[i]*fi[i];ro+=fo[i]*fo[i];}
        ri=sqrt(ri/FR);ro=sqrt(ro/FR);
        if(ri<0.01){inSil+=ri;outSil+=ro;silN++;}else{inV+=ri;outV+=ro;vN++;}}
    Reset(sess);

    printf("\n=== 結果 ===\n総フレーム %zu (低=%d, 高=%d)\n",frames,silN,vN);
    if(silN)printf("非発話 RMS: in=%.5f out=%.5f (out/in=%.3f)\n",inSil/silN,outSil/silN,inSil>0?outSil/inSil:0);
    if(vN)printf("発話   RMS: in=%.5f out=%.5f (out/in=%.3f)\n",inV/vN,outV/vN,inV>0?outV/inV:0);
    writeWav("captured.wav",in48,48000);
    writeWav("denoised.wav",out48,48000);
    printf("\ncaptured.wav（原音）と denoised.wav（処理後）を聴き比べてください。\n");
    return 0;
}
