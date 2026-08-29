// discord_krisp.node の IAT をフックし、Krisp が .kef をどのパスから開くかを観測する。
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <initializer_list>

static const uintptr_t RVA_SIG = 0x53110;
static const uintptr_t RVA_CreateFileW = 0xD91730;
static const uintptr_t RVA_CreateFile2 = 0xD91720;
static const uintptr_t RVA_GetModuleFileNameW = 0xD91908;
static const uintptr_t RVA_FindFirstFileW = 0xD917A0;
static const uintptr_t RVA_GetCurrentDirectoryW = 0xD91838;

static const wchar_t* kModDir =
    L"C:/Users/utaaa/AppData/Local/Discord/app-1.0.9255/modules/"
    L"discord_krisp-1/discord_krisp";

typedef int  (*Init_t)();
typedef void*(*Setup_t)(int,int);
typedef void (*Reset_t)(void*);

static HANDLE (WINAPI *realCFW)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static HANDLE (WINAPI *realCF2)(LPCWSTR,DWORD,DWORD,DWORD,LPCREATEFILE2_EXTENDED_PARAMETERS);
static DWORD  (WINAPI *realGMFN)(HMODULE,LPWSTR,DWORD);
static HANDLE (WINAPI *realFFF)(LPCWSTR,LPWIN32_FIND_DATAW);
static DWORD  (WINAPI *realGCD)(DWORD,LPWSTR);

static bool interesting(const wchar_t* n){
    if(!n) return false;
    for(const wchar_t* k : {L"kef",L"KMS",L"kms",L"krisp",L"Krisp",L"model",L"Model",L"thw"})
        if(wcsstr(n,k)) return true;
    return false;
}

static HANDLE WINAPI hookCFW(LPCWSTR n,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD c,DWORD f,HANDLE t){
    HANDLE r=realCFW(n,a,s,sa,c,f,t);
    if(interesting(n)) wprintf(L"[CreateFileW] %s => %s\n",n,r==INVALID_HANDLE_VALUE?L"FAIL":L"OK");
    return r;
}
static HANDLE WINAPI hookCF2(LPCWSTR n,DWORD a,DWORD s,DWORD c,LPCREATEFILE2_EXTENDED_PARAMETERS p){
    HANDLE r=realCF2(n,a,s,c,p);
    if(interesting(n)) wprintf(L"[CreateFile2] %s => %s\n",n,r==INVALID_HANDLE_VALUE?L"FAIL":L"OK");
    return r;
}
static HANDLE WINAPI hookFFF(LPCWSTR n,LPWIN32_FIND_DATAW d){
    HANDLE r=realFFF(n,d);
    if(interesting(n)) wprintf(L"[FindFirstFileW] %s => %s\n",n,r==INVALID_HANDLE_VALUE?L"FAIL":L"OK");
    return r;
}
static DWORD WINAPI hookGMFN(HMODULE m,LPWSTR buf,DWORD sz){
    DWORD r=realGMFN(m,buf,sz);
    wprintf(L"[GetModuleFileNameW] hmod=%p => %s\n",(void*)m,buf);
    return r;
}
static DWORD WINAPI hookGCD(DWORD sz,LPWSTR buf){
    DWORD r=realGCD(sz,buf);
    if(buf) wprintf(L"[GetCurrentDirectoryW] => %s\n",buf);
    return r;
}

static void patchIAT(void* base,uintptr_t rva,void* hook,void** realOut){
    void** slot=(void**)((uint8_t*)base+rva);
    *realOut=*slot;
    DWORD o; VirtualProtect(slot,8,PAGE_READWRITE,&o);
    *slot=hook;
    VirtualProtect(slot,8,o,&o);
}

int main(){
    SetConsoleOutputCP(CP_UTF8);
    SetDllDirectoryW(kModDir);
    wchar_t path[MAX_PATH]; swprintf(path,MAX_PATH,L"%s/discord_krisp.node",kModDir);
    HMODULE h=LoadLibraryW(path); if(!h){printf("load fail\n");return 1;}
    uint8_t* base=(uint8_t*)h;
    // 署名検証パッチ
    { uint8_t* t=base+RVA_SIG; DWORD o; VirtualProtect(t,3,PAGE_EXECUTE_READWRITE,&o);
      t[0]=0xB0;t[1]=0x01;t[2]=0xC3; VirtualProtect(t,3,o,&o); FlushInstructionCache(GetCurrentProcess(),t,3); }
    // IAT フック
    patchIAT(base,RVA_CreateFileW,(void*)hookCFW,(void**)&realCFW);
    patchIAT(base,RVA_CreateFile2,(void*)hookCF2,(void**)&realCF2);
    patchIAT(base,RVA_FindFirstFileW,(void*)hookFFF,(void**)&realFFF);
    patchIAT(base,RVA_GetModuleFileNameW,(void*)hookGMFN,(void**)&realGMFN);
    patchIAT(base,RVA_GetCurrentDirectoryW,(void*)hookGCD,(void**)&realGCD);

    auto Init=(Init_t)GetProcAddress(h,"KrispInitializeExternal");
    auto Setup=(Setup_t)GetProcAddress(h,"KrispNCSetup");
    auto Reset=(Reset_t)GetProcAddress(h,"KrispNCReset");

    printf("=== KrispInitializeExternal ===\n");
    int r=Init(); printf("  => %d\n",r);
    printf("=== KrispNCSetup(48000,10) ===\n");
    void* s=Setup(48000,10); printf("  session=%p\n",s);
    if(s) Reset(s);
    printf("done.\n");
    return 0;
}
