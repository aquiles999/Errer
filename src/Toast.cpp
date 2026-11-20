#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#ifdef WINVER
#undef WINVER
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wrl/client.h>
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#include "Toast.h"
#include "Flags.h"
#include "CrashLog.h"

static bool ToastsDisabled() { return NoToastsFlag(); }

#pragma comment(lib, "shell32.lib")

using namespace Microsoft::WRL;

namespace TNote = ABI::Windows::UI::Notifications;
namespace TXml = ABI::Windows::Data::Xml::Dom;
using TNote::IToastNotification;
using TNote::IToastNotificationFactory;
using TNote::IToastNotificationManagerStatics;
using TNote::IToastNotifier;
using TXml::IXmlDocument;
using TXml::IXmlDocumentIO;

#define TOAST_AUMID L"Windows Error Reporting"
#define TOAST_LEGACY_AUMID L"Windows.ErrorReporting"
#define TOAST_SHORTCUT_NAME L"Windows Error Reporting.lnk"
#define TOAST_LEGACY_SHORTCUT_NAME L"errer.lnk"
#define TOAST_ICON_RES_ID 114   // IDR_TOASTPNG (PNG, 64x64 app logo)

typedef HRESULT(WINAPI* FnRoInitialize)(RO_INIT_TYPE initType);
typedef HRESULT(WINAPI* FnRoGetActivationFactory)(HSTRING activatableClassId, REFIID iid, void** factory);
typedef HRESULT(WINAPI* FnRoActivateInstance)(HSTRING activatableClassId, IInspectable** instance);
typedef HRESULT(WINAPI* FnWindowsCreateStringReference)(PCWSTR sourceString, UINT32 length, HSTRING_HEADER* hstringHeader, HSTRING* string);

static FnWindowsCreateStringReference g_StrRef;
static FnRoInitialize g_RoInit;
static FnRoGetActivationFactory g_RoFactory;
static FnRoActivateInstance g_RoActivate;

static void* LoadFn(const wchar_t* dll, const char* fn) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);
    return m ? (void*)GetProcAddress(m, fn) : NULL;
}

static FnWindowsCreateStringReference GetStrRef() {
    if (g_StrRef) return g_StrRef;
    const wchar_t* dlls[] = { L"api-ms-win-core-winrt-string-l1-1-0.dll", L"combase.dll" };
    for (int i = 0; i < 2; i++) {
        g_StrRef = (FnWindowsCreateStringReference)LoadFn(dlls[i], "WindowsCreateStringReference");
        if (g_StrRef) break;
    }
    return g_StrRef;
}

static void EnsureRoApi() {
    if (g_RoFactory) return;
    const wchar_t* dlls[] = { L"api-ms-win-core-winrt-l1-1-0.dll", L"winrt.dll" };
    for (int i = 0; i < 2; i++) {
        if (!g_RoInit) g_RoInit = (FnRoInitialize)LoadFn(dlls[i], "RoInitialize");
        if (!g_RoFactory) g_RoFactory = (FnRoGetActivationFactory)LoadFn(dlls[i], "RoGetActivationFactory");
        if (!g_RoActivate) g_RoActivate = (FnRoActivateInstance)LoadFn(dlls[i], "RoActivateInstance");
        if (g_RoFactory) break;
    }
}

static HRESULT WinCreateStringRef(PCWSTR src, UINT32 len, HSTRING_HEADER* hdr, HSTRING* out) {
    FnWindowsCreateStringReference fn = GetStrRef();
    if (!fn) return E_FAIL;
    return fn(src, len, hdr, out);
}

static bool IsWin8OrLater() {
    typedef LONG(NTAPI* fnRtlGetVersion)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    fnRtlGetVersion rtl = ntdll ? (fnRtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (rtl) {
        RTL_OSVERSIONINFOW vi = {};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (rtl(&vi) == 0) {
            if (vi.dwMajorVersion > 6) return true;
            if (vi.dwMajorVersion == 6 && vi.dwMinorVersion >= 2) return true;
            return false;
        }
    }
    return false;
}
// The toast logo: a PNG copy of the app icon (WinRT toasts don't render .ico),
// dropped to %TEMP% so it can be referenced as an appLogoOverride image.
static wchar_t g_toastIconFile[MAX_PATH] = L"";

static bool EnsureToastIcon() {
    if (g_toastIconFile[0]) return true;

    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(TOAST_ICON_RES_ID), (LPCWSTR)RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hg = LoadResource(NULL, hRes);
    const BYTE* p = hg ? (const BYTE*)LockResource(hg) : NULL;
    DWORD size = p ? SizeofResource(NULL, hRes) : 0;
    if (!p || !size) return false;

    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return false;
    wcscat_s(tmp, MAX_PATH, L"errer_toast.png");

    FILE* f = nullptr;
    _wfopen_s(&f, tmp, L"wb");
    if (!f) return false;
    fwrite(p, 1, size, f);
    fclose(f);

    wcscpy_s(g_toastIconFile, MAX_PATH, tmp);
    return true;
}

static void ToFileUrl(const wchar_t* path, wchar_t* out, size_t cap) {
    size_t o = 0;
    if (cap > 8) { wcscpy_s(out, cap, L"file:///"); o = 8; }
    for (const wchar_t* p = path; *p && o + 8 < cap; p++) {
        if (*p == L'\\') {
            out[o++] = L'/';
        } else if (*p == L' ') {
            wcscpy_s(out + o, cap - o, L"%20");
            o += 3;
        } else {
            out[o++] = *p;
        }
    }
    if (o < cap) out[o] = 0; else out[cap - 1] = 0;
}

static void XmlEscape(const wchar_t* in, wchar_t* out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 6 < cap; in++) {
        switch (*in) {
        case L'&':
            wcscpy_s(out + o, cap - o, L"&amp;");
            o += 5;
            break;
        case L'<':
            wcscpy_s(out + o, cap - o, L"&lt;");
            o += 4;
            break;
        case L'>':
            wcscpy_s(out + o, cap - o, L"&gt;");
            o += 4;
            break;
        case L'"':
            wcscpy_s(out + o, cap - o, L"&quot;");
            o += 6;
            break;
        case L'\'':
            wcscpy_s(out + o, cap - o, L"&apos;");
            o += 6;
            break;
        case L'\r':
            wcscpy_s(out + o, cap - o, L"&#xD;");
            o += 5;
            break;
        case L'\n':
            wcscpy_s(out + o, cap - o, L"&#xA;");
            o += 5;
            break;
        default:
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

static bool EnsureAumidShortcut() {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) return false;

    wchar_t lnkDir[MAX_PATH];
    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, lnkDir);
    if (FAILED(hr)) return false;

    wchar_t lnkPath[MAX_PATH];
    swprintf_s(lnkPath, MAX_PATH, L"%s\\%s", lnkDir, TOAST_SHORTCUT_NAME);

    wchar_t oldLnk[MAX_PATH];
    swprintf_s(oldLnk, MAX_PATH, L"%s\\%s", lnkDir, TOAST_LEGACY_SHORTCUT_NAME);
    DeleteFileW(oldLnk);

    bool fresh = GetFileAttributesW(lnkPath) == INVALID_FILE_ATTRIBUTES;
    if (!fresh) return true;

    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))))
        return false;
    link->SetPath(exePath);
    link->SetIconLocation(exePath, 0);
    link->SetDescription(L"Windows Error Reporting");

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(link.As(&props))) {
        PROPVARIANT pv = {};
        InitPropVariantFromString(TOAST_AUMID, &pv);
        props->SetValue(PKEY_AppUserModel_ID, pv);
        PropVariantClear(&pv);

        InitPropVariantFromString(exePath, &pv);
        props->SetValue(PKEY_AppUserModel_RelaunchCommand, pv);
        PropVariantClear(&pv);

        props->Commit();
    }

    ComPtr<IPersistFile> pf;
    if (FAILED(link.As(&pf))) return false;
    return SUCCEEDED(pf->Save(lnkPath, TRUE));
}

static ComPtr<IToastNotifier> g_notifier;
static ComPtr<TNote::IToastNotificationManagerStatics> g_statics;
static ComPtr<IToastNotification> g_current;
static DWORD g_roThread = 0;
static bool g_roOk = false;

static bool WinRReady() {
    DWORD tid = GetCurrentThreadId();
    if (g_roThread == tid && g_roOk) return true;
    if (g_roThread != 0) return false;

    EnsureRoApi();
    if (!g_RoInit || !g_RoFactory || !g_RoActivate) return false;

    if (FAILED(g_RoInit(RO_INIT_SINGLETHREADED))) return false;
    g_roThread = tid;

    if (!EnsureAumidShortcut()) return false;

    HSTRING_HEADER clsHd, aumidHd;
    HSTRING clsName = NULL, aumid = NULL;
    ComPtr<IToastNotificationManagerStatics> statics;

    if (FAILED(WinCreateStringRef(
            RuntimeClass_Windows_UI_Notifications_ToastNotificationManager,
            (UINT32)wcslen(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager),
            &clsHd, &clsName)) ||
        FAILED(g_RoFactory(clsName, __uuidof(IToastNotificationManagerStatics),
                           reinterpret_cast<void**>(statics.GetAddressOf()))) ||
        FAILED(WinCreateStringRef(TOAST_AUMID, (UINT32)wcslen(TOAST_AUMID),
                                  &aumidHd, &aumid)) ||
        FAILED(statics->CreateToastNotifierWithId(aumid, g_notifier.GetAddressOf())))
        return false;

    g_statics = statics;
    g_roOk = true;

    // One-time cleanup of the old dotted-AUMID leftovers in the action center.
#if defined(____x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics2_INTERFACE_DEFINED__) && \
    defined(____x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistory_INTERFACE_DEFINED__)
    {
        ComPtr<TNote::IToastNotificationManagerStatics2> statics2;
        ComPtr<TNote::IToastNotificationHistory> hist;
        if (SUCCEEDED(g_statics.As(&statics2)) &&
            SUCCEEDED(statics2->get_History(&hist)) && hist) {
            HSTRING_HEADER oldHd;
            HSTRING oldAumid = NULL;
            if (SUCCEEDED(WinCreateStringRef(TOAST_LEGACY_AUMID,
                                             (UINT32)wcslen(TOAST_LEGACY_AUMID),
                                             &oldHd, &oldAumid)))
                hist->ClearWithId(oldAumid);
        }
    }
#endif
    return true;
}

// Hides the toast on screen and wipes the AUMID's Action Center queue so
// nothing piles up between payloads. Note: Remove() takes a notification
// tag, not an app id -- ClearWithId() is the one that empties our queue.
static void DismissWinRToast() {
    if (g_notifier && g_current) {
        HRESULT hr = g_notifier->Hide(g_current.Get());
        CrashLogf("toast winrt Hide hr=0x%08X", (unsigned)hr);
        g_current.Reset();
    }
#if defined(____x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics2_INTERFACE_DEFINED__) && \
    defined(____x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistory_INTERFACE_DEFINED__)
    if (g_statics) {
        ComPtr<TNote::IToastNotificationManagerStatics2> statics2;
        ComPtr<TNote::IToastNotificationHistory> hist;
        HRESULT h2 = g_statics.As(&statics2);
        HRESULT hh = h2 == S_OK ? statics2->get_History(&hist) : h2;
        if (hh == S_OK && hist) {
            HSTRING_HEADER aumidHd;
            HSTRING aumid = NULL;
            if (SUCCEEDED(WinCreateStringRef(TOAST_AUMID, (UINT32)wcslen(TOAST_AUMID),
                                             &aumidHd, &aumid))) {
                HRESULT hc = hist->ClearWithId(aumid);
                CrashLogf("toast winrt ClearWithId hr=0x%08X", (unsigned)hc);
            }
        } else {
            CrashLogf("toast winrt history QI hr=0x%08X", (unsigned)hh);
        }
    } else {
        CrashLog("toast winrt no statics (not an 8+ build?)");
    }
#endif
}

// Pin the toast's lifetime: Windows itself pulls it off the screen (and out
// of the action center) once the visible window elapses, so it dismisses even
// if our own Hide() call never lands (e.g. the payload process is already
// tearing down). Needs a boxed IReference&lt;DateTime&gt; for ExpirationTime.
static void SetToastExpiration(DWORD timeoutMs) {
    HSTRING_HEADER pvHd;
    HSTRING pvCls = NULL;
    static const wchar_t kPVClass[] = L"Windows.Foundation.PropertyValue";
    if (!g_RoFactory) return;
    if (FAILED(WinCreateStringRef(kPVClass, (UINT32)wcslen(kPVClass), &pvHd, &pvCls)))
        return;

    ComPtr<ABI::Windows::Foundation::IPropertyValueStatics> pvs;
    if (FAILED(g_RoFactory(pvCls, __uuidof(ABI::Windows::Foundation::IPropertyValueStatics),
                           reinterpret_cast<void**>(pvs.GetAddressOf()))) || !pvs)
        return;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ABI::Windows::Foundation::DateTime dt;
    dt.UniversalTime = ((INT64)ft.dwHighDateTime << 32) | (UINT32)ft.dwLowDateTime;
    dt.UniversalTime += (INT64)timeoutMs * 10000LL;

    ComPtr<::IInspectable> boxed;
    if (FAILED(pvs->CreateDateTime(dt, boxed.GetAddressOf())) || !boxed)
        return;

    ComPtr<__FIReference_1_Windows__CFoundation__CDateTime> ref;
    if (FAILED(boxed.As(&ref)) || !ref)
        return;

    if (g_current) {
        HRESULT hr = g_current->put_ExpirationTime(ref.Get());
        CrashLogf("toast winrt expiration hr=0x%08X ms=%lu", (unsigned)hr, (unsigned long)timeoutMs);
    }
}

static bool ShowWinRToast(const wchar_t* title, const wchar_t* body, DWORD timeoutMs) {
    if (timeoutMs < 700) timeoutMs = 700;
    if (!WinRReady()) return false;

    // Show() returns S_OK even when Windows has notifications turned off for
    // this app -- nothing renders. Ask the notifier so we can fall back to the
    // balloon channel instead of showing nothing at all.
    ABI::Windows::UI::Notifications::NotificationSetting setting =
        (ABI::Windows::UI::Notifications::NotificationSetting)0;
    HRESULT hrSet = g_notifier->get_Setting(&setting);
    if (FAILED(hrSet) || setting != (ABI::Windows::UI::Notifications::NotificationSetting)0) {
        CrashLogf("toast winrt notifications off (setting=%d hr=0x%08X), balloon fallback",
                  (int)setting, (unsigned)hrSet);
        return false;
    }

    DismissWinRToast();

    wchar_t t[512], b[512], xml[6144], iconUrl[512];
    iconUrl[0] = 0;
    if (EnsureToastIcon()) ToFileUrl(g_toastIconFile, iconUrl, ARRAYSIZE(iconUrl));

    XmlEscape(title, t, ARRAYSIZE(t));
    XmlEscape(body, b, ARRAYSIZE(b));

    if (iconUrl[0]) {
        swprintf_s(xml, ARRAYSIZE(xml),
                   L"<toast duration=\"short\"><visual><binding template=\"ToastImageAndText02\">"
                   L"<image id=\"1\" src=\"%s\" placement=\"appLogoOverride\"/>"
                   L"<text id=\"1\">%s</text><text id=\"2\">%s</text>"
                   L"</binding></visual><audio silent=\"true\"/></toast>", iconUrl, t, b);
    } else {
        swprintf_s(xml, ARRAYSIZE(xml),
                   L"<toast duration=\"short\"><visual><binding template=\"ToastText02\">"
                   L"<text id=\"1\">%s</text><text id=\"2\">%s</text>"
                   L"</binding></visual><audio silent=\"true\"/></toast>", t, b);
    }

    HSTRING_HEADER xmlHd;
    HSTRING xmlStr = NULL;
    HSTRING_HEADER xmlDocClsHd;
    HSTRING xmlDocCls = NULL;
    ComPtr<IInspectable> insp;
    ComPtr<IXmlDocument> doc;
    ComPtr<IXmlDocumentIO> io;

    if (FAILED(WinCreateStringRef(xml, (UINT32)wcslen(xml), &xmlHd, &xmlStr)))
        return false;

    // RoActivateInstance takes the runtime class NAME, not the XML text.
    if (FAILED(WinCreateStringRef(
            RuntimeClass_Windows_Data_Xml_Dom_XmlDocument,
            (UINT32)wcslen(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument),
            &xmlDocClsHd, &xmlDocCls)))
        return false;

    HRESULT hStep = g_RoActivate(xmlDocCls, insp.GetAddressOf());
    if (FAILED(hStep)) { CrashLogf("toast winrt step activate hr=0x%08X", (unsigned)hStep); return false; }
    hStep = insp.As(&doc);
    if (FAILED(hStep)) { CrashLogf("toast winrt step qi doc hr=0x%08X", (unsigned)hStep); return false; }
    hStep = doc.As(&io);
    if (FAILED(hStep)) { CrashLogf("toast winrt step qiio hr=0x%08X", (unsigned)hStep); return false; }
    hStep = io->LoadXml(xmlStr);
    if (FAILED(hStep)) { CrashLogf("toast winrt step loadxml hr=0x%08X", (unsigned)hStep); return false; }

    HSTRING_HEADER toastClsHd;
    HSTRING toastCls = NULL;
    ComPtr<IToastNotificationFactory> factory;
    ComPtr<IToastNotification> toast;

    if (FAILED(WinCreateStringRef(
            RuntimeClass_Windows_UI_Notifications_ToastNotification,
            (UINT32)wcslen(RuntimeClass_Windows_UI_Notifications_ToastNotification),
            &toastClsHd, &toastCls)))
        return false;
    if (FAILED(hStep = g_RoFactory(toastCls, __uuidof(IToastNotificationFactory),
                                   reinterpret_cast<void**>(factory.GetAddressOf())))) {
        CrashLogf("toast winrt step factory hr=0x%08X", (unsigned)hStep);
        return false;
    }
    if (FAILED(hStep = factory->CreateToastNotification(doc.Get(), toast.GetAddressOf()))) {
        CrashLogf("toast winrt step create hr=0x%08X", (unsigned)hStep);
        return false;
    }

    g_current = toast;
    SetToastExpiration(timeoutMs);
    HRESULT hrShow = g_notifier->Show(g_current.Get());
    CrashLogf("toast winrt Show hr=0x%08X", (unsigned)hrShow);
    return SUCCEEDED(hrShow);
}

void DismissToast();

static HWND s_balloonWnd = NULL;
static NOTIFYICONDATAW s_nid;
static bool s_balloonIcon = false;

static HWND g_warden = NULL;
static volatile LONG s_wardenStarted = 0;
static const wchar_t* kToastWardenClass = L"errerToastWarden";

static UINT ToastWardenMsg() {
    static UINT s_msg = RegisterWindowMessageW(L"errerDismissToasts");
    return s_msg;
}

LRESULT CALLBACK ToastWardenProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == ToastWardenMsg()) { DismissToast(); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The warden lives on its own thread with a real message pump. Payload main
// threads spend their lives in Sleep()/beat-wait loops and never drain a
// message queue, which is exactly how a stale toast used to survive a payload
// handoff: the broadcast arrived, but nobody processed it.
static DWORD WINAPI WardenThreadFn(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ToastWardenProc;
    wc.hInstance = inst;
    wc.lpszClassName = kToastWardenClass;
    RegisterClassW(&wc);

    g_warden = CreateWindowExW(0, kToastWardenClass, L"errerToastWarden", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, inst, NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

static void EnsureWarden() {
    if (InterlockedCompareExchange(&s_wardenStarted, 1, 0) != 0) return;
    HANDLE h = CreateThread(NULL, 0, WardenThreadFn, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

static BOOL CALLBACK WardenEnumProc(HWND hwnd, LPARAM lp) {
    (void)lp;
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) && _wcsicmp(cls, kToastWardenClass) == 0)
        SendMessageTimeoutW(hwnd, ToastWardenMsg(), 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_NORMAL, 25, NULL);
    return TRUE;
}

void DismissAllToasts() {
    EnsureWarden();
    EnumWindows(WardenEnumProc, 0);
}

static HWND BalloonWindow() {
    HINSTANCE inst = GetModuleHandleW(NULL);
    static const wchar_t* kClass = L"errerToastHost";
    static bool reg = false;
    if (!s_balloonWnd) {
        if (!reg) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = inst;
            wc.lpszClassName = kClass;
            RegisterClassW(&wc);
            reg = true;
        }
        s_balloonWnd = CreateWindowExW(0, kClass, L"Windows Error Reporting", WS_POPUP,
                                       CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
                                       NULL, NULL, inst, NULL);
    }
    return s_balloonWnd;
}

static void ShowBalloon(const wchar_t* title, const wchar_t* body, DWORD timeoutMs) {
    HWND hwnd = BalloonWindow();
    if (!hwnd) return;

    memset(&s_nid, 0, sizeof(s_nid));
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = hwnd;
    s_nid.uID = 0xFAC5;
    s_nid.uFlags = NIF_INFO | NIF_ICON | NIF_TIP;
    s_nid.uTimeout = timeoutMs < 700 ? 700 : timeoutMs;
    s_nid.dwInfoFlags = NIIF_ERROR | NIIF_NOSOUND;
    lstrcpynW(s_nid.szInfo, body, ARRAYSIZE(s_nid.szInfo));
    lstrcpynW(s_nid.szInfoTitle, title, ARRAYSIZE(s_nid.szInfoTitle));
    lstrcpynW(s_nid.szTip, title, ARRAYSIZE(s_nid.szTip));
    s_nid.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_INFORMATION));

    if (s_balloonIcon)
        Shell_NotifyIconW(NIM_DELETE, &s_nid);
    s_balloonIcon = true;
    Shell_NotifyIconW(NIM_ADD, &s_nid);
}

static void DismissBalloon() {
    if (s_balloonIcon) {
        NOTIFYICONDATAW d = s_nid;
        d.uFlags = NIF_INFO;
        d.szInfo[0] = 0;
        d.szInfoTitle[0] = 0;
        Shell_NotifyIconW(NIM_MODIFY, &d);
    }
}

void ShowToast(const wchar_t* title, const wchar_t* body, DWORD timeoutMs) {
    if (ToastsDisabled()) return;
    if (IsWin8OrLater()) {
        if (!ShowWinRToast(title, body, timeoutMs)) {
            CrashLogf("toast winrt show failed, balloon fallback ms=%lu", (unsigned long)timeoutMs);
            ShowBalloon(title, body, timeoutMs);
        }
    } else {
        ShowBalloon(title, body, timeoutMs);
    }
}

void DismissToast() {
    if (IsWin8OrLater())
        DismissWinRToast();
    else
        DismissBalloon();
}

static const struct {
    const wchar_t* title;
    const wchar_t* body;
} kErrorToastPool[] = {
    { L"Windows Memory Diagnostic", L"The Windows Memory Diagnostic detected a problem. Contact your computer manufacturer. Error code 0xC000009F." },
    { L"Clean Shutdown Failed", L"The system has rebooted without cleanly shutting down first. Bug check code 0x0000009F." },
    { L"S.M.A.R.T. Failure Predicted", L"A.R.T. Failure Predicted on hard disk 0. Warranty replacement recommended. Failing attribute 0x0005." },
    { L"Graphics Device Removed", L"The graphics device has been physically removed, was suspended, or a driver upgrade is in progress. Status 0x00000000." },
    { L"Device Timeout", L"The device did not respond within the timeout period. A reset to the device was issued." },
    { L"Volume C: Corrupt", L"The file structure on volume C: cannot be repaired. Please run the chkdsk utility." },
    { L"PCI Express Error", L"A PCI Express device reported an uncorrectable error. Bus #2, Device #2, Function #3." },
    { L"Thermal Threshold", L"An operating system temperature sensor is approaching the critical threshold at 82 degrees C." },
    { L"Checksum Error", L"Portable computer power error. Post code 0x00000000." },
    { L"Bus Error", L"The bus has reported an uncorrectable error. Check cables and peripheral connections." },
    { L"Out Of Range", L"A configuration data range was requested that is out of range." },
    { L"Boot Device Inaccessible", L"The boot device is inaccessible. The boot configuration data could not be read." },
    { L"Drive Health", L"A physical drive has reported a loss of predictive health. Status 0x00000002." },
    { L"POST Error", L"Post code 0x0000004E. The system could not initialize the operating system loader." },
    { L"Chkdsk Required", L"The volume bitmap on volume C: could not be reconciled. Please run the chkdsk utility." },
    { L"Uncorrectable Error", L"The processor has reported an uncorrectable error. Error code 0x00000133." },
    { L"Multi Bit ECC Error", L"A Multi Bit ECC error has been found in RAM please shutdown immediately" },
    { L"Critical Process Died", L"A critical system process, svchost.exe, has died" },
    { L"IRQL_NOT_LESS_OR_EQUAL", L"An exception was thrown at an unsupported interrupt request level" },
    { L"KERNEL_DATA_INPAGE_ERROR", L"Windows could not page this page of memory into RAM" },
    { L"REGISTRY FAILURE", L"The system hive has been corrupted. A persistent problem was detected" },
    { L"MEMORY MANAGEMENT", L"A serious error occurred in the memory manager. Your system has been stopped" },
    { L"DRIVER_IRQL_NOT_LESS_OR_EQUAL", L"A driver accessed a memory address it is not permitted to use" },
    { L"SYSTEM_THREAD_EXCEPTION_NOT_HANDLED", L"An unhandled exception was raised in a system thread" },
    { L"NO MORE IRP STACK LOCATIONS", L"The I/O request packet stack size is too small for this operation" },
    { L"VIDEO TDR FAILURE", L"The graphics driver stopped responding and has recovered" },
    { L"ATTEMPTED WRITE TO READONLY MEMORY", L"A process tried to write to memory that is marked read-only" },
    { L"KMODE_EXCEPTION_NOT_HANDLED", L"An unhandled kernel-mode exception occurred" },
    { L"FATAL FILE SYSTEM ERROR", L"The file system redirector ran into a fatal condition" },
    { L"STOP: 0x0000007F", L"An unexpected kernel mode trap was raised" },
    { L"KERNEL SECURITY CHECK FAILURE", L"A kernel component violated a security invariant" },
    { L"Could not divide by zero", L"Could not divide by zero. The attempted division exceeds the range of an integer value." },
    { L"HARDWARE FAILURE", L"A floating point exception was raised in the hardware abstraction layer." },
    { L"STACK OVERFLOW", L"The kernel mode stack overflowed in the process csrss.exe." },
    { L"WINDOWS UPDATE FAILED", L"Some updates could not be installed. Error code 0x80070002 was encountered." },
    { L"DISK IO FAILURE", L"The requested operation could not be performed due to a hardware device error." },
    { L"DRIVER POWER STATE FAILURE", L"A driver is in an invalid or inconsistent power state." },
    { L"PAGE FAULT IN NONPAGED AREA", L"The requested kernel page could not be read from the paging file." },
    { L"INTERRUPT EXCEPTION NOT HANDLED", L"A hardware interrupt was raised but the handler was not registered." },
    { L"Special pool double free", L"A tried to free a block of memory that was already released." },
    { L"CLOCK WATCHDOG TIMEOUT", L"The system clock has stopped responding and corrective action was taken." },
    { L"DRIVER VERIFIER DMA VIOLATION", L"A driver made an invalid DMA request and verifier detected it." },
    { L"MANUALLY INITIATED CRASH", L"An administrator forced the system to crash for diagnostics." },
    { L"UNEXPECTED STORE EXCEPTION", L"The store component could not flush the session data to disk." },
    { L"SECURE KERNEL ERROR", L"The secure kernel logged an exception and cannot continue." },
    { L"REFERENCE BY POINTER", L"The kernel dereferenced a pointer that was already released." },
    { L"MISALIGNED ACCESS", L"An aligned memory access was attempted at an unaligned address." },
    { L"INSTRUCTION FETCH ABORT", L"The processor could not fetch an instruction from the requested page." },
    { L"NMI HARDWARE FAILURE", L"A non-maskable interrupt was raised by the hardware." },
    { L"PFN_LIST_CORRUPT", L"The system attempted to get the next page from a freed list." },
    { L"SYSTEM_SERVICE_EXCEPTION", L"An exception happened while executing a system service routine." },
    { L"DRIVER_OVERRAN_STACK_BUFFER", L"A driver overran a stack-based buffer." },
    { L"DPC_WATCHDOG_VIOLATION", L"The deferred procedure call was not completed in time." },
    { L"THREAD_STUCK_IN_DEVICE_DRIVER", L"A device driver is stuck in a loop." },
    { L"CRITICAL_STRUCTURE_CORRUPTION", L"The code that modifies the critical kernel structures was altered." },
    { L"KERNEL MODE HEAP CORRUPTION", L"A kernel memory block was modified after it was freed." },
    { L"APC INDEX MISMATCH", L"An asynchronous procedure call was raised on the wrong object." },
    { L"INACCESSIBLE BOOT DEVICE", L"Windows could not read the boot configuration data." },
    { L"MISMATCHED HAL", L"The hardware abstraction layer does not match this platform." },
    { L"BAD SYSTEM CONFIG INFO", L"The registry is damaged or missing a required file." },
    { L"UNSUPPORTED INSTRUCTION MODE", L"The processor received an instruction mode it cannot run." },
    { L"MUP_FILE_SYSTEM", L"The multiple UNC provider failed to complete a write request." },
    { L"SPIN_LOCK NOT OWNED", L"A thread released a spin lock it never acquired." },
    { L"CONFIG LIST FAILURE", L"The configuration manager could not load one of its lists." },
    { L"BOOTMNGR", L"The boot selection failed because a required device is inaccessible." },
    { L"VOLUME SNAPSHOT SERVICE FAILED", L"The shadow copy provider could not complete a snapshot." },
    { L"UNMOUNTABLE BOOT VOLUME", L"The boot volume could not be mounted by the file system." },
    { L"STATUS_DEVICE_DATA_ERROR", L"The cyclic redundancy check detected an error on the device." },
    { L"WORKER INVALID", L"A worker thread was returned to a worker who never owned it." },
    { L"WerFault.exe - Application Error", L"The application failed to initialize properly (0xc0000142)." },
    { L"Windows Update", L"We couldn't complete the updates. Undoing changes. Don't turn off your computer." },
    { L"WHEA_UNCORRECTABLE_ERROR", L"A hardware error occurred that could not be corrected." },
    { L"BATTERY NOT DETECTED", L"The system could not find any battery on this computer." },
    { L"SECURE BOOT VERIFICATION FAILED", L"This device does not meet the firmware requirements to boot." },
};
static const int kErrorToastCount = (int)(sizeof(kErrorToastPool) / sizeof(kErrorToastPool[0]));

void ShowRandomErrorToast(DWORD timeoutMs) {
    int idx = (int)(rand() % kErrorToastCount);
    ShowToast(kErrorToastPool[idx].title, kErrorToastPool[idx].body, timeoutMs);
}

static HANDLE s_toastThread = NULL;
static HANDLE s_toastThreadEvent = NULL;
static volatile LONG s_toastStop = 0;

struct ToastThreadParam {
    const DWORD* delays;   // non-NULL => timeline mode
    int      count;
    DWORD    interval;     // ticker interval for timeline == NULL
};

static DWORD WINAPI ToastThreadFn(LPVOID p) {
    ToastThreadParam* par = (ToastThreadParam*)p;
    if (par->delays) {
        DWORD t = GetTickCount();
        for (int i = 0; i < par->count && !s_toastStop; i++) {
            t += par->delays[i];
            for (;;) {
                LONG remain = (LONG)(t - GetTickCount());
                if (remain <= 0) break;
                DWORD wait = remain > 16 ? 16 : (DWORD)remain;
                if (WaitForSingleObject(s_toastThreadEvent, wait) == WAIT_OBJECT_0) {
                    delete par;
                    return 0;
                }
            }
            // A toast lives at least 700 ms, and otherwise exactly until the
            // next one is due (WinRT expiration / balloon timeout both use
            // this value).
            DWORD shown = par->delays[i];
            if (shown < 700) shown = 700;
            ShowRandomErrorToast(shown);
            DWORD until = GetTickCount() + shown;
            while (!s_toastStop) {
                LONG remain = (LONG)(until - GetTickCount());
                if (remain <= 0) break;
                if (WaitForSingleObject(s_toastThreadEvent, 8) == WAIT_OBJECT_0) {
                    delete par;
                    return 0;
                }
            }
            DismissToast();
            // Hide only covers toasts we own; this also purges anything a
            // sibling payload process left behind (process teardown, etc).
            DismissAllToasts();
        }
    } else {
        DWORD interval = par->interval;
        if (interval < 50) interval = 50;
        DWORD showMs = interval * 7 / 10;
        if (showMs > interval - 50) showMs = interval - 50;
        while (!s_toastStop) {
            if (WaitForSingleObject(s_toastThreadEvent, interval) == WAIT_OBJECT_0) break;
            ShowRandomErrorToast();
            if (WaitForSingleObject(s_toastThreadEvent, showMs) == WAIT_OBJECT_0) break;
            DismissToast();
        }
    }
    DismissAllToasts();   // nothing outlives the timeline
    delete par;
    return 0;
}

static void StartToastThread(ToastThreadParam* par) {
    if (!par) return;
    if (ToastsDisabled()) { delete par; return; }
    if (s_toastThread) { delete par; return; }
    // The previous payload may still have a toast on screen: close it.
    DismissAllToasts();
    InterlockedExchange(&s_toastStop, 0);
    s_toastThreadEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    EnsureWarden();
    s_toastThread = CreateThread(NULL, 0, ToastThreadFn, par, 0, NULL);
}

void StartToastTimeline(const DWORD* delays, int count) {
    if (!delays || count <= 0) return;
    StartToastThread(new ToastThreadParam{ delays, count, 0 });
}

void StartRandomErrorToasts(DWORD intervalMs) {
    if (intervalMs < 50) intervalMs = 50;
    StartToastThread(new ToastThreadParam{ NULL, 0, intervalMs });
}

// Called by a payload right before it terminates: stops any pending toast,
// hides the one on screen and clears the AUMID's queue everywhere, so nothing
// from this payload survives into the next one.
void ToastShutdown() {
    InterlockedExchange(&s_toastStop, 1);
    if (s_toastThreadEvent) SetEvent(s_toastThreadEvent);
    DismissToast();
    DismissAllToasts();
}