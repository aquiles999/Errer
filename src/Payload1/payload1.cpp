#include <windows.h>
#include <string>
#include <exdisp.h>
#include <shlobj.h>
#include <shlguid.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <tlhelp32.h>
#include "../FakeExplorer.h"
#include "../Flags.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

#define IDR_MP3 112
#define NUM_NAV 45

static IMFMediaSession* g_session = NULL;
DWORD g_audioStartTick = 0;

static const int NAV_DELAYS[45] = {
    411,   424,   121,    87,   112,
    111,   401,   448,   424,   224,
    223,   441,   439,   432,   112,
    103,    81,   119,   458,   446,
    425,   223,   270,   362,   457,
    416,   120,   120,    81,   144,
    448,   409,   440,   216,   330,
    327,   423,   432,   112,   112,
     89,   127,   449,   409,   415
};

static const wchar_t* NAV_PATHS[5] = {
    L"C:\\",
    L"C:\\Users",
    L"C:\\Windows\\System32",
    L"C:\\Windows\\SysWOW64",
    NULL
};

static std::wstring GetExpandedUserProfile() {
    wchar_t buf[MAX_PATH];
    ExpandEnvironmentStringsW(L"%USERPROFILE%", buf, MAX_PATH);
    return buf;
}

static std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, &type, buf, &size) == ERROR_SUCCESS)
        return buf;
    return L"";
}

static void WriteUndoReg(const std::wstring& originalFormat) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    std::wstring regPath = dir + L"undo.reg";

    FILE* f = nullptr;
    _wfopen_s(&f, regPath.c_str(), L"w");
    if (!f) return;
    fprintf(f, "Windows Registry Editor Version 5.00\r\n\r\n");
    fprintf(f, "[HKEY_CURRENT_USER\\Control Panel\\International]\r\n");
    fprintf(f, "\"sShortDate\"=\"%ls\"\r\n", originalFormat.c_str());
    fclose(f);
}

static void SetShortDateFormat(const wchar_t* format) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\International", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"sShortDate", 0, REG_SZ, (const BYTE*)format, (DWORD)((wcslen(format) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

static void DbgLog(const wchar_t* step, HRESULT hr) {
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"errer_audio.log");
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"a");
    if (f) {
        fwprintf(f, L"[%lu] %ls hr=0x%08X\n", GetTickCount(), step, (unsigned)hr);
        fclose(f);
    }
}

static void PlayWithMci(void* data, DWORD size) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wcscat_s(tmp, L"errer_badapple.mp3");

    FILE* f = nullptr;
    _wfopen_s(&f, tmp, L"wb");
    if (!f) { DbgLog(L"mci temp file open failed", HRESULT_FROM_WIN32(GetLastError())); return; }
    fwrite(data, 1, size, f);
    fclose(f);

    mciSendStringW(L"close errer_mp3", NULL, 0, NULL);
    std::wstring cmd = L"open \"" + std::wstring(tmp) + L"\" type mpegvideo alias errer_mp3";
    DWORD r = mciSendStringW(cmd.c_str(), NULL, 0, NULL);
    if (r != 0) { DbgLog(L"mci open failed", (HRESULT)r); return; }
    g_audioStartTick = GetTickCount();
    mciSendStringW(L"play errer_mp3", NULL, 0, NULL);
}

void PlayBadApple() {
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_MP3), (LPCWSTR)RT_RCDATA);
    if (!hRes) { DbgLog(L"findres failed", HRESULT_FROM_WIN32(GetLastError())); return; }
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) { DbgLog(L"loadres failed", HRESULT_FROM_WIN32(GetLastError())); return; }
    void* p = LockResource(hData);
    DWORD size = SizeofResource(NULL, hRes);
    if (!p || size == 0) { DbgLog(L"lockres failed", E_FAIL); return; }

    static bool mfReady = false;
    if (!mfReady) {
        HRESULT hr = MFStartup(MF_VERSION, 0);
        if (FAILED(hr)) { DbgLog(L"MFStartup failed", hr); PlayWithMci(p, size); return; }
        mfReady = true;
    }

    IStream* pStream = NULL;
    HRESULT hr = CreateStreamOnHGlobal(hData, FALSE, &pStream);
    if (FAILED(hr)) { DbgLog(L"CreateStreamOnHGlobal failed", hr); PlayWithMci(p, size); return; }

    IMFByteStream* pMem = NULL;
    hr = MFCreateMFByteStreamOnStream(pStream, &pMem);
    pStream->Release();
    if (FAILED(hr)) { DbgLog(L"MFCreateMFByteStreamOnStream failed", hr); PlayWithMci(p, size); return; }

    IMFSourceResolver* pResolver = NULL;
    hr = MFCreateSourceResolver(&pResolver);
    if (FAILED(hr)) { DbgLog(L"MFCreateSourceResolver failed", hr); pMem->Release(); PlayWithMci(p, size); return; }

    IUnknown* pSourceObj = NULL;
    MF_OBJECT_TYPE objType = MF_OBJECT_INVALID;
    hr = pResolver->CreateObjectFromByteStream(pMem, NULL,
        MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE,
        NULL, &objType, &pSourceObj);
    pResolver->Release();
    pMem->Release();
    if (FAILED(hr) || objType != MF_OBJECT_MEDIASOURCE || !pSourceObj) {
        DbgLog(L"CreateObjectFromByteStream failed", hr);
        PlayWithMci(p, size);
        return;
    }

    IMFMediaSource* pSource = NULL;
    hr = pSourceObj->QueryInterface(IID_PPV_ARGS(&pSource));
    pSourceObj->Release();
    if (FAILED(hr) || !pSource) { DbgLog(L"QI media source failed", hr); PlayWithMci(p, size); return; }

    if (g_session) {
        g_session->Shutdown();
        g_session->Release();
        g_session = NULL;
    }
    hr = MFCreateMediaSession(NULL, &g_session);
    if (FAILED(hr) || !g_session) {
        DbgLog(L"MFCreateMediaSession failed", hr);
        if (g_session) { g_session->Release(); g_session = NULL; }
        pSource->Release();
        PlayWithMci(p, size);
        return;
    }

    IMFPresentationDescriptor* pPD = NULL;
    hr = pSource->CreatePresentationDescriptor(&pPD);
    if (FAILED(hr)) { DbgLog(L"CreatePresentationDescriptor failed", hr); pSource->Release(); PlayWithMci(p, size); return; }

    BOOL fSelected = FALSE;
    IMFStreamDescriptor* pSD = NULL;
    hr = pPD->GetStreamDescriptorByIndex(0, &fSelected, &pSD);
    if (FAILED(hr)) { DbgLog(L"GetStreamDescriptorByIndex failed", hr); pPD->Release(); pSource->Release(); PlayWithMci(p, size); return; }

    IMFTopology* pTopo = NULL;
    if (SUCCEEDED(MFCreateTopology(&pTopo))) {
        IMFTopologyNode* pSrcNode = NULL;
        IMFTopologyNode* pOutNode = NULL;
        if (SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &pSrcNode)) &&
            SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pOutNode))) {
            pSrcNode->SetObject(pSource);
            pSrcNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pSD);
            pTopo->AddNode(pSrcNode);

            IMFActivate* pActivate = NULL;
            if (SUCCEEDED(MFCreateAudioRendererActivate(&pActivate))) {
                pOutNode->SetObject(pActivate);
                pActivate->Release();
            }
            pTopo->AddNode(pOutNode);

            if (SUCCEEDED(pSrcNode->ConnectOutput(0, pOutNode, 0)) &&
                SUCCEEDED(g_session->SetTopology(0, pTopo))) {
                PROPVARIANT pv;
                pv.vt = VT_EMPTY;
                g_audioStartTick = GetTickCount();
                g_session->Start(NULL, &pv);
            }
            pOutNode->Release();
        }
        if (pSrcNode) pSrcNode->Release();
        pTopo->Release();
    }

    pSD->Release();
    pPD->Release();
    pSource->Release();
}

void StopBadApple() {
    if (g_session) {
        g_session->Stop();
        g_session->Shutdown();
        g_session->Release();
        g_session = NULL;
    }
    mciSendStringW(L"stop errer_mp3", NULL, 0, NULL);
    mciSendStringW(L"close errer_mp3", NULL, 0, NULL);
}

static DWORD WINAPI RestartExplorerThread(LPVOID p) {
    (void)p;
    typedef LONG(NTAPI* pNtTerminateProcess)(HANDLE, UINT);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    pNtTerminateProcess NtTerminate = ntdll ?
        (pNtTerminateProcess)GetProcAddress(ntdll, "NtTerminateProcess") : nullptr;

    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &tp.Privileges[0].Luid);
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
        CloseHandle(hToken);
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pe.th32ProcessID);
                    if (!h) continue;
                    if (NtTerminate)
                        NtTerminate(h, 0);
                    else
                        TerminateProcess(h, 0);
                    CloseHandle(h);
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    Sleep(150);

    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring exe = std::wstring(sysDir) + L"\\explorer.exe";
    STARTUPINFOW si2 = {};
    si2.cb = sizeof(si2);
    PROCESS_INFORMATION pi2 = {};
    if (CreateProcessW(exe.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si2, &pi2)) {
        CloseHandle(pi2.hProcess);
        CloseHandle(pi2.hThread);
    }

    Sleep(150);
    if (FindWindowW(L"Shell_TrayWnd", nullptr) == nullptr) {
        ShellExecuteW(nullptr, L"open", exe.c_str(), L"C:\\", nullptr, SW_SHOW);
    }
    return 0;
}

static HWND KillRestartExplorer() {
    HWND hwnd = CreateFakeExplorer(L"C:\\", 80, 80, 540, 320);

    HANDLE hRestart = CreateThread(NULL, 0, RestartExplorerThread, NULL, 0, NULL);
    if (hRestart) CloseHandle(hRestart);

    return hwnd;
}

void runPayload1() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t* fakeFormat = L"ER/R/25ER";
    std::wstring originalFormat = ReadRegString(HKEY_CURRENT_USER, L"Control Panel\\International", L"sShortDate");
    HWND hwnd = NULL;
    if (originalFormat != fakeFormat && !NoDateChangeFlag()) {
        WriteUndoReg(originalFormat);
        SetShortDateFormat(fakeFormat);
        hwnd = KillRestartExplorer();
    } else {
        hwnd = CreateFakeExplorer(L"C:\\", 80, 80, 540, 320);
    }

    std::wstring userProfile = GetExpandedUserProfile();
    const wchar_t* dirs[5] = { NAV_PATHS[0], NAV_PATHS[1], NAV_PATHS[2], NAV_PATHS[3], userProfile.c_str() };

    DWORD songAnchor = g_audioStartTick;
    {
        wchar_t buf[64];
        DWORD n = GetEnvironmentVariableW(L"ERRER_SONG_START", buf, 64);
        if (n > 0 && n < 64) songAnchor = (DWORD)_wtoi(buf);
    }

    if (hwnd) {
        if (songAnchor) {
            long long beats[NUM_NAV];
            long long acc = 1700;
            for (int i = 0; i < NUM_NAV; i++) { beats[i] = acc; acc += NAV_DELAYS[i]; }

            for (int i = 0; i < NUM_NAV; i++) {
                if (!IsWindow(hwnd)) break;
                DWORD now = GetTickCount();
                DWORD target = songAnchor + (DWORD)beats[i];
                if ((LONG)(now - target) >= 0) continue;
                PumpMessagesFor(target - now);
                NavigateFakeExplorer(hwnd, dirs[i % 5]);
            }
        } else {
            PumpMessagesFor(1700);
            for (int i = 0; i < NUM_NAV; i++) {
                if (!IsWindow(hwnd)) break;
                NavigateFakeExplorer(hwnd, dirs[i % 5]);
                PumpMessagesFor(NAV_DELAYS[i]);
            }
        }
        DestroyFakeExplorer(hwnd);
    }

    TerminateProcess(GetCurrentProcess(), 0);
}
