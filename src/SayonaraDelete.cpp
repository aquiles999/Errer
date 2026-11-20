#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>
#include "SayonaraDelete.h"
#include "CrashLog.h"
#include "Flags.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")

// rand() is no good here: MSVC gives every new thread the same initial state,
// and each delete box is spawned on its own fresh thread, so every box would
// pick the same file. RtlGenRandom is a real thread-safe CSPRNG.
extern "C" BOOLEAN WINAPI SystemFunction036(PVOID pbBuffer, ULONG cbBuffer);

static DWORD Rnd(void) {
    DWORD v = 0;
    if (!SystemFunction036(&v, sizeof(v))) {
        // Fallback: mix tick count with a process-wide counter so threads that
        // race still disagree.
        static volatile LONG seq = 0;
        v = GetTickCount() ^ (DWORD)InterlockedIncrement(&seq) ^ (GetCurrentThreadId() * 2654435761u);
    }
    return v;
}

// Uniform-enough value in [0, n).
static int RndBelow(int n) {
    if (n <= 0) return 0;
    return (int)(Rnd() % (DWORD)n);
}

// ---------------------------------------------------------------------------
// Faithful port of Sayonara's DeleteMgr/SayonaraDeleteDialog. The struct
// offsets below match the original heap block exactly (everything in the
// decompile is short* based, hence the 2-byte stride).
// ---------------------------------------------------------------------------

static int      g_dpi = 96;
static HFONT    g_dlgFont = NULL;
static HICON    g_shellIcon = NULL;
static COLORREF g_accent = 0xd77800;
static bool     g_darkTheme = false;
static bool     g_classReg = false;
static int      g_placement = SAYONARA_PLACEMENT_CASCADE;
static volatile LONG g_cascadeSlot  = 0;
static volatile LONG g_cascadeReady = 0;
static int      g_cascadeX = 0;
static int      g_cascadeY = 0;
static int      g_cascadeDX = 1;
static int      g_cascadeDY = 1;

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Localised strings.
//
// Sayonara shipped localised ("Excluir Arquivo" / "Sim" / "Nao"), so the dialog
// follows the system language instead of always showing English. Selected once,
// automatically - there is no override, the dialog just reads the locale.
//
// A LANGID packs the primary language into the LOW 10 bits and the sublanguage
// into the top 6: pt-BR is 0x0416, so primary 0x16 + sublanguage 1.
//
// Beware: the SDK's PRIMARYLANGID macro is (lid >> 10) & 0x3f, which extracts
// the SUBLANGUAGE, not the language. Using it makes every lookup miss.
// ---------------------------------------------------------------------------

#define LID_PRIMARY(lid)  ((WORD)((lid) & 0x3FF))
#define LID_SUB(lid)      ((WORD)(((lid) >> 10) & 0x3F))

struct SayonaraLang {
    WORD primary;                       // LANGID primary language, 0 = catch-all
    const wchar_t* title;
    const wchar_t* question;
    const wchar_t* created;
    const wchar_t* size;
    const wchar_t* desc;
    const wchar_t* company;
    const wchar_t* version;
    const wchar_t* yes;
    const wchar_t* no;
};

#define L_TITLE     L"Delete File"
#define L_QUESTION  L"Are you sure you want to permanently delete this file?"
#define L_CREATED   L"Created:"
#define L_SIZE      L"Size:"
#define L_DESC      L"File description:"
#define L_COMPANY   L"Company:"
#define L_VERSION   L"File version:"
#define L_YES       L"Yes"
#define L_NO        L"No"

static const SayonaraLang kLang[] = {
    // 0x01 Arabic
    { 0x01, L"\u062d\u0630\u0641 \u0645\u0644\u0641",
      L"\u0647\u0644 \u0623\u0646\u062a \u0645\u062a\u0623\u0643\u062f \u0623\u0646\u0643 \u062a\u0631\u06cc\u0628 \u062d\u0630\u0641 \u0647\u0630\u0627 \u0627\u0644\u0645\u0644\u0641 \u0646\u0647\u0627\u0626\u06cc\u061f",
      L"\u062a\u0627\u0631\u06cc\u062e \u0627\u0644\u0625\u0646\u0634\u0627\u0621:", L"\u0627\u0644\u062d\u062c\u0645:", L"\u0648\u0635\u0641 \u0627\u0644\u0645\u0644\u0641:", L"\u0627\u0644\u0634\u0631\u0643\u0629:",
      L"\u0625\u0635\u062f\u0627\u0631 \u0627\u0644\u0645\u0644\u0641:", L"\u0646\u0639\u0645", L"\u0644\u0627" },

    // 0x02 Bulgarian
    { 0x02, L"\u0418\u0437\u0442\u0440\u0438\u0432\u0430\u043d\u0435 \u043d\u0430 \u0444\u0430\u0439\u043b",
      L"\u0421\u0438\u0433\u0443\u0440\u043d\u0438 \u043b\u0438 \u0441\u0442\u0435 \u0443\u0431\u0435\u0434\u0435\u043d\u0438, \u0447\u0435 \u0438\u0441\u043a\u0430\u0447\u0435 \u0442\u043e\u0437\u0432\u0430 \u0444\u0430\u0439\u043b\u0430 \u0437\u0430\u0432\u0435\u043a\u0438 \u0437\u0430\u043f\u0440\u0435\u0445\u043e\u0434\u043d\u043e?",
      L"\u0421\u044a\u0437\u0434\u0430\u043d\u043e \u043d\u0430:", L"\u0420\u0430\u0437\u043c\u0435\u0440:", L"\u041e\u043f\u0438\u0441\u0430\u043d\u0438\u0435 \u043d\u0430 \u0444\u0430\u0439\u043b\u0430:",
      L"\u0424\u0438\u0440\u043c\u0430:", L"\u0412\u0435\u0440\u0441\u0438\u044f \u043d\u0430 \u0444\u0430\u0439\u043b\u0430:", L"\u0414\u0430", L"\u041d\u0435" },

    // 0x04 Chinese - script chosen by sublanguage (see LangForLid)
    { 0x04, L"\u522a\u9664\u6a94\u6848",
      L"\u78ba\u5b9a\u8981\u6c38\u4e45\u522a\u9664\u6b64\u6a94\u6848\u55ce\uff1f",
      L"\u5efa\u7acb\u6642\u9593:", L"\u5927\u5c0f:", L"\u6a94\u6848\u63cf\u8ff0:", L"\u516c\u53f8:",
      L"\u6a94\u6848\u7248\u672c:", L"\u662f", L"\u5426" },
    { 0x04, L"\u522a\u9664\u6587\u4ef6",
      L"\u60a8\u786e\u5b9a\u8981\u6c38\u4e45\u5220\u9664\u6b64\u6587\u4ef6\u5417\uff1f",
      L"\u521b\u5efa\u65f6\u95f4:", L"\u5927\u5c0f:", L"\u6587\u4ef6\u8bf4\u660e:", L"\u516c\u53f8:",
      L"\u6587\u4ef6\u7248\u672c:", L"\u662f", L"\u5426" },

    // 0x05 Czech
    { 0x05, L"Odstranit soubor",
      L"Opravdu chcete tento soubor trvale odstranit?",
      L"Vytvo\u0159eno:", L"Velikost:", L"Popis souboru:", L"Spole\u010dnost:",
      L"Verze souboru:", L"Ano", L"Ne" },

    // 0x06 Danish
    { 0x06, L"Slet fil",
      L"Er du sikker p\u00e5, at du vil slette denne fil permanent?",
      L"Oprettet:", L"St\u00f8rrelse:", L"Filbeskrivelse:", L"Virksomhed:",
      L"Filversion:", L"Ja", L"Nej" },

    // 0x07 German
    { 0x07, L"Datei l\u00f6schen",
      L"M\u00f6chten Sie diese Datei wirklich dauerhaft l\u00f6schen?",
      L"Erstellt:", L"Gr\u00f6\u00dfe:", L"Dateibeschreibung:", L"Unternehmen:",
      L"Dateiversion:", L"Ja", L"Nein" },

    // 0x08 Greek
    { 0x08, L"\u0394\u03b9\u03b1\u03b3\u03c1\u03b1\u03c6\u03ae \u03b1\u03c1\u03c7\u03b5\u03af\u03bf\u03c5",
      L"\u0395\u03af\u03bd\u03b1\u03c2\u03c2 \u03b2\u03ad\u03b2\u03b5\u03bf\u03b9 \u03cc\u03c4\u03b9 \u03b8\u03ad\u03bb\u03b5\u03c4\u03b5 \u03bd\u03b1 \u03b4\u03b9\u03b1\u03b3\u03c1\u03b1\u03c6\u03af\u03c3\u03b5\u03c2\u03b5 \u03bf\u03c1\u03b9\u03c3\u03c4\u03b9\u03b3\u03bd\u03cc\u03c2 \u03b1\u03c5\u03c4\u03ce \u03c4\u03bf \u03b1\u03c5\u03c4\u03cc \u03c4\u03bf \u03b1\u03c1\u03c7\u03b5\u03af\u03bf\u03c5;",
      L"\u0394\u03b7\u03bc\u03b9\u03bf\u03c5\u03c1\u03b3\u03af\u03b8\u03b7\u03ba\u03b5:", L"\u039c\u03ad\u03b3\u03b5\u03b8\u03bf\u03c2:", L"\u03a0\u03b5\u03c1\u03b9\u03b3\u03c1\u03b1\u03c6\u03ae \u03b1\u03c1\u03c7\u03b5\u03af\u03bf\u03c5:",
      L"\u0395\u03c4\u03b1\u03b9\u03c1\u03b5\u03af\u03b1:", L"\u0388\u03ba\u03b4\u03bf\u03c3\u03b7 \u03b1\u03c1\u03c7\u03b5\u03af\u03bf\u03c5:", L"\u039d\u03b1\u03b9", L"\u038c\u03c7\u03b9" },

    // 0x09 English
    { 0x09, L_TITLE, L_QUESTION, L_CREATED, L_SIZE, L_DESC, L_COMPANY, L_VERSION, L_YES, L_NO },

    // 0x0a Spanish
    { 0x0a, L"Eliminar archivo",
      L"\u00bfEst\u00e1 seguro de que desea eliminar este archivo de forma permanente?",
      L"Fecha de creaci\u00f3n:", L"Tama\u00f1o:", L"Descripci\u00f3n del archivo:", L"Empresa:",
      L"Versi\u00f3n del archivo:", L"S\u00ed", L"No" },

    // 0x0b Finnish
    { 0x0b, L"Poista tiedosto",
      L"Haluatko varmasti poistaa t\u00e4m\u00e4n tiedoston pysyv\u00e4sti?",
      L"Luotu:", L"Koko:", L"Tiedoston kuvaus:", L"Yritys:",
      L"Tiedostoversio:", L"Kyll\u00e4", L"Ei" },

    // 0x0c French
    { 0x0c, L"Supprimer le fichier",
      L"Voulez-vous vraiment supprimer ce fichier d\u00e9finitivement ?",
      L"Cr\u00e9\u00e9 le :", L"Taille :", L"Description du fichier :", L"Soci\u00e9t\u00e9 :",
      L"Version du fichier :", L"Oui", L"Non" },

    // 0x0d Hebrew
    { 0x0d, L"\u05de\u05d7\u05d9\u05e7\u05ea \u05e7\u05d5\u05d1",
      L"\u05d4\u05d0\u05dd \u05d0\u05ea\u05d4 \u05d1\u05d8\u05d5\u05d7 \u05e9\u05d1\u05e8\u05e6\u05d5\u05e0\u05da \u05dc\u05de\u05d7\u05d9\u05e7 \u05e7\u05d5\u05d1 \u05d6\u05d4 \u05dc\u05e6\u05de\u05d9\u05ea?",
      L"\u05e0\u05d5\u05e6\u05e8:", L"\u05d2\u05d5\u05d3\u05dc:", L"\u05ea\u05d0\u05d5\u05e8 \u05d4\u05e7\u05d5\u05d1:", L"\u05d7\u05d1\u05e8\u05d4:",
      L"\u05d2\u05e8\u05e1\u05d4 \u05d4\u05e7\u05d5\u05d1:", L"\u05db\u05df", L"\u05dc\u05d0" },

    // 0x0e Hungarian
    { 0x0e, L"F\u00e1jl t\u00f6rl\u00e9se",
      L"Biztosan v\u00e9glegesen t\u00f6rli ezt a f\u00e1jlt?",
      L"L\u00e9trehozva:", L"M\u00e9ret:", L"F\u00e1jlle\u00edr\u00e1s:", L"V\u00e1llalat:",
      L"F\u00e1jlverzi\u00f3:", L"Igen", L"Nem" },

    // 0x10 Italian
    { 0x10, L"Elimina file",
      L"Sei sicuro di voler eliminare definitivamente questo file?",
      L"Creato il:", L"Dimensione:", L"Descrizione file:", L"Azienda:",
      L"Versione file:", L"S\u00ec", L"No" },

    // 0x11 Japanese
    { 0x11, L"\u30d5\u30a1\u30a4\u30eb\u3092\u524a\u9664",
      L"\u3053\u306e\u30d5\u30a1\u30a4\u30eb\u3092\u5b8c\u5168\u306b\u524a\u9664\u3057\u307e\u3059\u3088\u3046\u304b\uff1f",
      L"\u4f5c\u6210\u65e5\u6642:", L"\u30b5\u30a4\u30ba:", L"\u30d5\u30a1\u30a4\u30eb\u306e\u8aac\u660e:", L"\u4f1a\u793e:",
      L"\u30d5\u30a1\u30a4\u30eb\u30d0\u30fc\u30b8\u30e7\u30f3:", L"\u306f\u3044", L"\u3044\u3044\u3048" },

    // 0x12 Korean
    { 0x12, L"\ud30c\uc77c \uc0ad\uc81c",
      L"\uc774 \ud30c\uc77c\uc744 \uc601\uad6c\uc801\uc73c\ub85c \uc0ad\uc81c\ud558\uc2dc\uaca8\uc2b5\ub2c8\ub2e4?",
      L"\uc0dd\uc131\uc77c:", L"\ud06c\uae30:", L"\ud30c\uc77c \uc124\uba85:", L"\ud68c\uc0ac:",
      L"\ud30c\uc77c \ubc84\uc804:", L"\uc608", L"\uc544\ub2c8\uc624" },

    // 0x13 Dutch
    { 0x13, L"Bestand verwijderen",
      L"Weet u zeker dat u dit bestand definitief wilt verwijderen?",
      L"Aangemaakt:", L"Grootte:", L"Bestandsomschrijving:", L"Bedrijf:",
      L"Bestandsversie:", L"Ja", L"Nee" },

    // 0x14 Norwegian
    { 0x14, L"Slett fil",
      L"Er du sikker p\u00e5 at du vil slette denne filen permanent?",
      L"Opprettet:", L"St\u00f8rrelse:", L"Filbeskrivelse:", L"Selskap:",
      L"Filversjon:", L"Ja", L"Nei" },

    // 0x15 Polish
    { 0x15, L"Usu\u0144 plik",
      L"Czy na pewno chcesz trwale usun\u0105\u0107 ten plik?",
      L"Utworzono:", L"Rozmiar:", L"Opis pliku:", L"Firma:",
      L"Wersja pliku:", L"Tak", L"Nie" },

    // 0x16 Portuguese - Brazil unless sublanguage says Europe
    { 0x16, L"Excluir Arquivo",
      L"Tem certeza de que deseja excluir este arquivo permanentemente?",
      L"Data da cria\u00e7\u00e3o:", L"Tamanho:", L"Descri\u00e7\u00e3o do arquivo:", L"Empresa:",
      L"Vers\u00e3o do arquivo:", L"Sim", L"N\u00e3o" },
    { 0x16, L"ExcluirFicheiro",
      L"Tem a certeza de que deseja excluir este ficheiro permanentemente?",
      L"Data de cria\u00e7\u00e3o:", L"Tamanho:", L"Descri\u00e7\u00e3o do ficheiro:", L"Empresa:",
      L"Vers\u00e3o do ficheiro:", L"Sim", L"N\u00e3o" },

    // 0x18 Romanian
    { 0x18, L"\u0218terge fi\u0219ierul",
      L"Sigur dori\u021bi s\u0103 \u0219terge\u021bi definitiv acest fi\u0219ier?",
      L"Creat:", L"Dimensiune:", L"Descrierea fi\u0219ierului:", L"Companie:",
      L"Versiunea fi\u0219ierului:", L"Da", L"Nu" },

    // 0x19 Russian
    { 0x19, L"\u0423\u0434\u0430\u043b\u0438\u0442\u044c \u0444\u0430\u0439\u043b",
      L"\u0412\u044b \u0443\u0432\u0435\u0440\u0435\u043d\u044b, \u0447\u0442\u043e \u0432\u044b \u0445\u043e\u0442\u0438\u0442\u0435 \u043e\u043a\u043e\u043d\u0447\u0430\u0442\u0435\u043b\u044c\u043d\u043e \u0443\u0434\u0430\u043b\u0438\u0442\u044c \u044d\u0442\u043e\u0442 \u0444\u0430\u0439\u043b?",
      L"\u0421\u043e\u0437\u0434\u0430\u043d:", L"\u0420\u0430\u0437\u043c\u0435\u0440:", L"\u041e\u043f\u0438\u0441\u0430\u043d\u0438\u0435 \u0444\u0430\u0439\u043b\u0430:", L"\u041a\u043e\u043c\u043f\u0430\u043d\u0438\u044f:",
      L"\u0412\u0435\u0440\u0441\u0438\u044f \u0444\u0430\u0439\u043b\u0430:", L"\u0414\u0430", L"\u041d\u0435\u0442" },

    // 0x1a Serbian / Croatian
    { 0x1a, L"\u0411\u0440\u0438\u0448\u0430\u043d\u0458\u0435 \u0434\u0430\u0442\u043e\u0442\u0435\u043a\u0435",
      L"\u0414\u0430 \u043b\u0438 \u0441\u0442\u0435 \u0441\u0438\u0433\u0443\u0440\u043d\u043e \u0434\u0430 \u0442\u0440\u0430\u0439\u043d\u043e \u0438\u0437\u0431\u0440\u0438\u0448\u0435\u0442\u0435 \u043e\u0432\u0430\u0439 \u0434\u0430\u0442\u043e\u0442\u0435\u043a\u0435?",
      L"\u041d\u0430\u043f\u0440\u0430\u0432\u0459\u0435\u043d\u043e:", L"\u0412\u0435\u043b\u0438\u0447\u0438\u043d\u0430:", L"\u041e\u043f\u0438\u0441 \u0434\u0430\u0442\u043e\u0442\u0435\u043a\u0435:", L"\u041a\u043e\u043c\u043f\u0430\u043d\u0438\u0458\u0430:",
      L"\u0412\u0435\u0440\u0437\u0438\u0458\u0430 \u0434\u0430\u0442\u043e\u0442\u0435\u043a\u0435:", L"\u0414\u0430", L"\u041d\u0435" },

    // 0x1b Slovak
    { 0x1b, L"Odstr\u00e1ni\u0165 s\u00fabor",
      L"Naozaj chcete tento s\u00fabor natrvalo odstr\u00e1ni\u0165?",
      L"Vytvoren\u00e9:", L"Ve\u013ekos\u0165:", L"Popis s\u00faboru:", L"Spolo\u010dnos\u0165:",
      L"Verzia s\u00faboru:", L"\u00c1no", L"Nie" },

    // 0x1d Swedish
    { 0x1d, L"Ta bort fil",
      L"\u00c4r du s\u00e4ker p\u00e5 att du vill ta bort den h\u00e4r filen permanent?",
      L"Skapad:", L"Storlek:", L"Filbeskrivning:", L"F\u00f6retag:",
      L"Filversion:", L"Ja", L"Nej" },

    // 0x1e Thai
    { 0x1e, L"\u0e25\u0e1a\u0e44\u0e1f\u0e25\u0e4c",
      L"\u0e04\u0e38\u0e13\u0e40\u0e19\u0e49\u0e13\u0e2b\u0e23\u0e37\u0e2d\u0e44\u0e21\u0e48\u0e17\u0e35\u0e48\u0e15\u0e49\u0e2d\u0e01\u0e32\u0e23\u0e25\u0e1b\u0e44\u0e1f\u0e25\u0e4c\u0e19\u0e35\u0e49\u0e2d\u0e32\u0e07\u0e16\u0e32\u0e27\u0e22",
      L"\u0e2a\u0e23\u0e49\u0e32\u0e07\u0e40\u0e21\u0e37\u0e48\u0e2d:", L"\u0e02\u0e19\u0e32\u0e14:", L"\u0e04\u0e33\u0e2d\u0e18\u0e34\u0e1a\u0e32\u0e22\u0e44\u0e1f\u0e25\u0e4c:", L"\u0e1a\u0e23\u0e34\u0e29\u0e31\u0e15:",
      L"\u0e40\u0e27\u0e2d\u0e23\u0e4c\u0e0a\u0e31\u0e19\u0e44\u0e1f\u0e25\u0e4c:", L"\u0e43\u0e0a\u0e48", L"\u0e44\u0e21\u0e48" },

    // 0x1f Turkish
    { 0x1f, L"Dosyay\u0131 Sil",
      L"Bu dosyay\u0131 kal\u0131c\u0131 olarak silmek istedi\u011finizden emin misiniz?",
      L"Olu\u015fturulma:", L"Boyut:", L"Dosya a\u00e7\u0131klamas\u0131:", L"\u015eirket:",
      L"Dosya s\u00fcr\u00fcm\u00fc:", L"Evet", L"Hay\u0131r" },

    // 0x21 Indonesian
    { 0x21, L"Hapus File",
      L"Apakah Anda yakin ingin menghapus file ini secara permanen?",
      L"Dibuat:", L"Ukuran:", L"Deskripsi file:", L"Perusahaan:",
      L"Versi file:", L"Ya", L"Tidak" },

    // 0x22 Ukrainian
    { 0x22, L"\u0412\u0438\u0434\u0430\u043b\u0438\u0442\u0438 \u0444\u0430\u0439\u043b",
      L"\u0412\u0438 \u0441\u043f\u0440\u0430\u0432\u0434\u0456 \u0445\u043e\u0447\u0435\u0442\u0435 \u043e\u0441\u0442\u0430\u0442\u043a\u043e\u0432\u043e \u0432\u0438\u0434\u0430\u043b\u0438\u0442\u0438 \u0446\u0435\u0439 \u0444\u0430\u0439\u043b?",
      L"\u0421\u0442\u0432\u043e\u0440\u0435\u043d\u043e:", L"\u0420\u043e\u0437\u043c\u0456\u0440:", L"\u041e\u043f\u0438\u0441 \u0444\u0430\u0439\u043b\u0443:", L"\u041a\u043e\u043c\u043f\u0430\u043d\u0456\u044f:",
      L"\u0412\u0435\u0440\u0441\u0456\u044f \u0444\u0430\u0439\u043b\u0443:", L"\u0422\u0430\u043a", L"\u041d\u0456" },

    // 0x24 Slovenian
    { 0x24, L"Izbri\u0161i datoteko",
      L"Ali ste prepri\u010dani, da \u017eelite trajno izbrisati to datoteko?",
      L"Ustvarjeno:", L"Velikost:", L"Opis datoteke:", L"Podjetje:",
      L"Razli\u010dica datoteke:", L"Da", L"Ne" },

    // 0x25 Estonian
    { 0x25, L"Kustuta fail",
      L"Kas olete kindel, et soovite selle faili j\u00e4\u00e4davalt kustutada?",
      L"Loodud:", L"Suurus:", L"Faili kirjeldus:", L"Ettev\u00f5te:",
      L"Faili versioon:", L"Jah", L"Ei" },

    // 0x26 Latvian
    { 0x26, L"Dz\u0113st failu",
      L"Vai esat p\u0101rliecin\u0101ts, ka v\u0113laties neatgriezeniski dz\u0113st \u0161o failu?",
      L"Izveidots:", L"Lielums:", L"Faila apraksts:", L"Uz\u0146\u0113mums:",
      L"Faila versija:", L"J\u0101", L"N\u0113" },

    // 0x27 Lithuanian
    { 0x27, L"I\u0161trinti fail\u0105",
      L"Ar tikrai norite negr\u012f\u017etamai i\u0161trinti \u0161\u012f fail\u0105?",
      L"Suk\u016brimo data:", L"Dydis:", L"Failo apra\u0161ymas:", L"\u012emon\u0117:",
      L"Failo versija:", L"Taip", L"Ne" },

    // 0x2a Vietnamese
    { 0x2a, L"X\u00f3a t\u1ec7p",
      L"B\u1ea1n c\u00f3 ch\u1eafc ch\u0103n mu\u1ed1n x\u00f3a v\u0129nh vi\u1ec5n t\u1ec7p n\u00e0y kh\u00f4ng?",
      L"\u0110\u00e3 t\u1ea1o:", L"K\u00edch th\u01b0\u1edbc:", L"M\u00f4 t\u1ea3 t\u1ec7p:", L"C\u00f4ng ty:",
      L"Phi\u00ean b\u1ea3n t\u1ec7p:", L"C\u00f3", L"Kh\u00f4ng" },

    // 0x36 Catalan
    { 0x36, L"Elimina el fitxer",
      L"Segur que vols suprimir permanentment aquest fitxer?",
      L"Creat:", L"Mida:", L"Descripci\u00f3 del fitxer:", L"Empresa:",
      L"Versi\u00f3 del fitxer:", L"S\u00ed", L"No" },
};

#define LANG_TABLE_COUNT ((int)(sizeof(kLang) / sizeof(kLang[0])))

static const SayonaraLang kLangEn = {
    0x09, L_TITLE, L_QUESTION, L_CREATED, L_SIZE, L_DESC, L_COMPANY, L_VERSION, L_YES, L_NO
};

static const SayonaraLang* g_lang = &kLangEn;

// Chinese: two rows share primary 0x04, so pick by sublanguage.
static const SayonaraLang* LangForLid(LANGID lid) {
    WORD primary = LID_PRIMARY(lid);
    WORD sub = LID_SUB(lid);

    const SayonaraLang* first = NULL;
    for (int i = 0; i < LANG_TABLE_COUNT; i++) {
        if (kLang[i].primary != primary) continue;
        if (!first) { first = &kLang[i]; continue; }

        // Second row for this primary: disambiguate by sublanguage.
        if (primary == 0x04) {                       // Chinese
            int simplified = (sub == 0x02 || sub == 0x04);
            return &kLang[simplified ? i : i - 1];
        }
        if (primary == 0x16) {                       // Portuguese
            return &kLang[sub == 0x02 ? i : i - 1];  // Europe vs Brazil
        }
        return first;
    }
    return first;   // NULL when the language is not one we translate
}

static void SelectLanguage() {
    // Three settings can each be the "real" language and they often disagree on
    // a VM: GetUserDefaultUILanguage is the display/MUI language (often left
    // English even on a localised install), GetUserDefaultLangID is the user's
    // locale, GetSystemDefaultLangID is the machine's.
    //
    // Prefer the display language, but if it lands on English while the user's
    // or system's locale is something we translate, follow that - a Portuguese
    // install should not show an English dialog.
    const SayonaraLang* ui = LangForLid(GetUserDefaultUILanguage());
    if (ui) { g_lang = ui; return; }

    const SayonaraLang* user = LangForLid(GetUserDefaultLangID());
    if (user) { g_lang = user; return; }

    const SayonaraLang* sys = LangForLid(GetSystemDefaultLangID());
    g_lang = sys ? sys : &kLangEn;
}
// Boxes spawn on separate threads and update the cascade head together, so the
// head is guarded by one process-wide lock.
static CRITICAL_SECTION g_cascadeCs;
static volatile LONG g_cascadeCsReady = 0;

static void CascadeLock(void) {
    if (InterlockedCompareExchange(&g_cascadeCsReady, 1, 0) == 0)
        InitializeCriticalSection(&g_cascadeCs);
}

void SayonaraDeleteSetPlacement(int mode) {
    g_placement = mode;
}

static int MD(int v) { return MulDiv(v, g_dpi, 96); }

struct SayonaraDlg {
    wchar_t name[96];             // 0x000
    wchar_t type[96];             // 0x0C0
    wchar_t size[64];             // 0x180
    wchar_t date[96];             // 0x200
    HICON   fileIcon;             // 0x2C0
    int     qLeft, qTop, qRight, qBottom;   // 0x2C8 question rect
    int     shellX, shellY;       // 0x2D8 document icon
    int     iconX, iconY;         // 0x2E0 file icon
    int     textLeft;             // 0x2E8
    int     textTop;              // 0x2EC
    int     shellIconSize;        // 0x2F0
    int     fileIconSize;         // 0x2F4
    int     lineH;                // 0x2F8
    RECT    yesRect;              // 0x2FC
    RECT    noRect;               // 0x30C
    int     yesHot;               // 0x31C
    int     tracked;              // 0x320
    int     slot;                 // 0x324

    // VERSIONINFO lines, shown only when the file actually carries them
    // (errer.exe has none, which is why its dialog shows only name/date/size).
    wchar_t desc[128];
    wchar_t company[128];
    wchar_t version[64];
    int     lineCount;            // how many detail rows are non-empty

    // "No" hover fades over ~0.3 s rather than snapping.
    int     noHot;
    double  noFade;               // 0 = idle, 1 = fully hovered
    double  noFadeFrom, noFadeTo;
    ULONGLONG noFadeStart;
    int     yesFocused;
};

#define DLG_WANT_DELETE 0x8001   // spawn one box
#define DLG_CLEAR_ALL   0x8002   // destroy all boxes

static void ReloadTheme() {
    HDC hdc = GetDC(NULL);
    if (!hdc) { g_dpi = 96; }
    else {
        g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        if (g_dpi == 0) g_dpi = 96;
        ReleaseDC(NULL, hdc);
    }

    typedef HRESULT(WINAPI* fnGetDpiForSystem)(VOID);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    fnGetDpiForSystem pDpi = user32 ? (fnGetDpiForSystem)GetProcAddress(user32, "GetDpiForSystem") : NULL;
    if (pDpi) {
        UINT d = pDpi();
        if (d) g_dpi = (int)d;
    }

    DWORD appsLight = 1;
    DWORD sz = sizeof(appsLight);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &appsLight, &sz);
    g_darkTheme = (appsLight == 0);

    DWORD accent = 0;
    sz = sizeof(accent);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, NULL, &accent, &sz) == ERROR_SUCCESS) {
        g_accent = (COLORREF)(((accent >> 8) & 0xff) << 8 |
                              ((accent >> 16) & 0xff) << 16 |
                              (accent & 0xff));
    } else {
        g_accent = (COLORREF)0xd77800;
    }
}

static void ReloadIcons() {
    // Top-left icon is the real Windows "unknown document" icon, not a folder.
    // Asking for an extension nothing owns makes the shell hand back its
    // default document image, which is what the original showed.
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(L"C:\\nonexistent.zzz", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                      SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
        g_shellIcon = sfi.hIcon;   // the shell's default document image
    }

    wchar_t sysDir[MAX_PATH];
    if (!GetSystemDirectoryW(sysDir, MAX_PATH)) return;

    // Segoe UI, 9 POINTS -> pixels (MulDiv with 72, the original's 0x48).
    if (g_dlgFont) DeleteObject(g_dlgFont);
    g_dlgFont = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_DONTCARE, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

// Win10 1809+ dark title bar.
static void ApplyDarkTitleBar(HWND hwnd, bool dark) {
    if (!dark) return;
    typedef HRESULT(WINAPI* fnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (!dwm) dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    fnDwmSetWindowAttribute fn = (fnDwmSetWindowAttribute)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (!fn) return;
    BOOL v = TRUE;
    fn(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &v, sizeof(v));
}

typedef HRESULT(WINAPI* fnStrFormatByteSizeEx)(ULONGLONG, DWORD, PWSTR, UINT);
static void FormatSize(ULONGLONG bytes, wchar_t* out, size_t cap) {
    HMODULE shl = GetModuleHandleW(L"shlwapi.dll");
    fnStrFormatByteSizeEx fn = shl ? (fnStrFormatByteSizeEx)GetProcAddress(shl, "StrFormatByteSizeEx") : NULL;
    if (fn && SUCCEEDED(fn(bytes, 1, out, (UINT)cap))) return;
    swprintf_s(out, cap, L"%llu bytes", (unsigned long long)bytes);
}

static LRESULT CALLBACK SayonaraDeleteProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SayonaraDlg* d = (SayonaraDlg*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_CREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_TIMER && wp == 1) {
        KillTimer(hwnd, 1);
        DestroyWindow(hwnd);
        return 0;
    }
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        COLORREF bg = g_darkTheme ? RGB(0x2b, 0x2b, 0x2b) : RGB(0xff, 0xff, 0xff);
        COLORREF fg = g_darkTheme ? RGB(0xff, 0xff, 0xff) : RGB(0x1a, 0x1a, 0x1a);
        COLORREF sub = g_darkTheme ? RGB(0xe8, 0xe8, 0xe8) : RGB(0x1a, 0x1a, 0x1a);

        HBRUSH br = CreateSolidBrush(bg);
        FillRect(mem, &rc, br);
        DeleteObject(br);

        SetBkMode(mem, TRANSPARENT);

        if (g_shellIcon)
            DrawIconEx(mem, d->shellX, d->shellY, g_shellIcon,
                       d->shellIconSize, d->shellIconSize, 0, NULL, DI_NORMAL);

        // Red X badge on the lower-right of the document icon, matching the
        // original's "this file is being deleted" mark.
        {
            int ix = d->shellX, iy = d->shellY, isz = d->shellIconSize;
            int bx = ix + isz - MD(20);
            int by = iy + isz - MD(20);
            int bw = MD(18), bh = MD(18);
            HPEN xp = CreatePen(PS_SOLID, MD(3), RGB(0xd9, 0x1a, 0x1a));
            HGDIOBJ oxp = SelectObject(mem, xp);
            MoveToEx(mem, bx + MD(3), by + MD(3), NULL);
            LineTo(mem, bx + bw - MD(3), by + bh - MD(3));
            MoveToEx(mem, bx + bw - MD(3), by + MD(3), NULL);
            LineTo(mem, bx + MD(3), by + bh - MD(3));
            SelectObject(mem, oxp);
            DeleteObject(xp);
        }

        SetTextColor(mem, fg);
        SelectObject(mem, g_dlgFont);
        RECT q = { d->qLeft, d->qTop, d->qRight, d->qBottom };
        DrawTextW(mem, g_lang->question, -1, &q,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);

        if (d->fileIcon)
            DrawIconEx(mem, d->iconX, d->iconY, d->fileIcon,
                       d->fileIconSize, d->fileIconSize, 0, NULL, DI_NORMAL);

        SelectObject(mem, g_dlgFont);
        SetTextColor(mem, sub);
        RECT line = { d->textLeft, d->textTop, rc.right - MD(12), d->textTop + d->lineH };
        DrawTextW(mem, d->name, -1, &line, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        line.top = d->textTop + d->lineH;
        line.bottom = line.top + d->lineH;
        // Only the rows the file actually has, so the block grows/shrinks
        // exactly like the original's.
        if (d->desc[0]) {
            DrawTextW(mem, d->desc, -1, &line, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            line.top += d->lineH; line.bottom += d->lineH;
        }
        if (d->company[0]) {
            DrawTextW(mem, d->company, -1, &line, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            line.top += d->lineH; line.bottom += d->lineH;
        }
        if (d->version[0]) {
            DrawTextW(mem, d->version, -1, &line, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            line.top += d->lineH; line.bottom += d->lineH;
        }
        DrawTextW(mem, d->date, -1, &line, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        line.top += d->lineH;
        line.bottom += d->lineH;
        DrawTextW(mem, d->size, -1, &line, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mem, g_dlgFont);

        // Yes (accent border when hovered, matching the original)
        COLORREF yesFill = g_darkTheme ? RGB(0x33, 0x33, 0x33) : RGB(0xf0, 0xf0, 0xf0);
        if (d->yesHot == 1) yesFill = g_darkTheme ? RGB(0x3a, 0x3a, 0x3a) : RGB(0xe2, 0xe2, 0xe2);
        HBRUSH yb = CreateSolidBrush(yesFill);
        HPEN yp = CreatePen(PS_SOLID, MD(2), g_accent);
        SelectObject(mem, yb);
        SelectObject(mem, yp);
        RoundRect(mem, d->yesRect.left, d->yesRect.top, d->yesRect.right, d->yesRect.bottom,
                  MD(8), MD(8));
        SelectObject(mem, GetStockObject(NULL_PEN));
        SelectObject(mem, yb);
        SetTextColor(mem, g_darkTheme ? RGB(0xff, 0xff, 0xff) : RGB(0x1a, 0x1a, 0x1a));
        SelectObject(mem, g_dlgFont);
        RECT y = d->yesRect;
        DrawTextW(mem, g_lang->yes, -1, &y, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, g_dlgFont);
        DeleteObject(yb);
        DeleteObject(yp);

        // "Sim" is the default button, so it carries the dotted focus ring
        // the original shows straight out of the box.
        if (GetFocus() == hwnd || d->yesFocused) {
            RECT fr = d->yesRect;
            InflateRect(&fr, -MD(3), -MD(3));
            DrawFocusRect(mem, &fr);
        }

        // No - fill and border fade toward the hover colour over ~0.3 s.
        double f = d->noFade;
        if (f < 0.0) f = 0.0;
        if (f > 1.0) f = 1.0;

        COLORREF noFillBase = g_darkTheme ? RGB(0x2f, 0x2f, 0x2f) : RGB(0xf5, 0xf5, 0xf5);
        COLORREF noFillHot  = g_darkTheme ? RGB(0x24, 0x3c, 0x5c) : RGB(0xe8, 0xf1, 0xfc);
        COLORREF noEdgeBase = g_darkTheme ? RGB(0x3a, 0x3a, 0x3a) : RGB(0xe2, 0xe2, 0xe2);
        COLORREF noEdgeHot  = g_darkTheme ? RGB(0x5c, 0x9c, 0xe6) : RGB(0x3a, 0x7b, 0xd8);
        COLORREF noFgBase   = g_darkTheme ? RGB(0x6d, 0x6d, 0x6d) : RGB(0xa6, 0xa6, 0xa6);
        COLORREF noFgHot    = g_darkTheme ? RGB(0xc8, 0xdc, 0xf4) : RGB(0x1f, 0x3c, 0x63);

        int noFill = (int)(GetRValue(noFillBase) + (GetRValue(noFillHot) - GetRValue(noFillBase)) * f);
        int noFillG = (int)(GetGValue(noFillBase) + (GetGValue(noFillHot) - GetGValue(noFillBase)) * f);
        int noFillB = (int)(GetBValue(noFillBase) + (GetBValue(noFillHot) - GetBValue(noFillBase)) * f);
        int noEdge = (int)(GetRValue(noEdgeBase) + (GetRValue(noEdgeHot) - GetRValue(noEdgeBase)) * f);
        int noEdgeG = (int)(GetGValue(noEdgeBase) + (GetGValue(noEdgeHot) - GetGValue(noEdgeBase)) * f);
        int noEdgeB = (int)(GetBValue(noEdgeBase) + (GetBValue(noEdgeHot) - GetBValue(noEdgeBase)) * f);
        int noFg = (int)(GetRValue(noFgBase) + (GetRValue(noFgHot) - GetRValue(noFgBase)) * f);
        int noFgG = (int)(GetGValue(noFgBase) + (GetGValue(noFgHot) - GetGValue(noFgBase)) * f);
        int noFgB = (int)(GetBValue(noFgBase) + (GetBValue(noFgHot) - GetBValue(noFgBase)) * f);

        HBRUSH nb = CreateSolidBrush(RGB(noFill, noFillG, noFillB));
        HPEN np = CreatePen(PS_SOLID, MD(1), RGB(noEdge, noEdgeG, noEdgeB));
        SelectObject(mem, nb);
        SelectObject(mem, np);
        RoundRect(mem, d->noRect.left, d->noRect.top, d->noRect.right, d->noRect.bottom,
                  MD(8), MD(8));
        SetTextColor(mem, RGB(noFg, noFgG, noFgB));
        SelectObject(mem, g_dlgFont);
        RECT n = d->noRect;
        DrawTextW(mem, g_lang->no, -1, &n, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, g_dlgFont);
        DeleteObject(nb);
        DeleteObject(np);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST:
        return HTCAPTION;

    case WM_MOUSEMOVE: {
        POINT p = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        int hot = PtInRect(&d->yesRect, p) ? 1 : -1;
        if (hot != d->yesHot) {
            d->yesHot = hot;
            InvalidateRect(hwnd, &d->yesRect, FALSE);
        }

        int noHot = PtInRect(&d->noRect, p) ? 1 : 0;
        if (noHot != d->noHot) {
            d->noHot = noHot;
            // Start (or reverse) the ~0.3 s fade instead of snapping.
            d->noFadeFrom = d->noFade;
            d->noFadeTo = noHot ? 1.0 : 0.0;
            d->noFadeStart = GetTickCount64();
            SetTimer(hwnd, 2, 16, NULL);
        }

        if (!d->tracked) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            d->tracked = 1;
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == 2) {
            double dur = 300.0;   // ms, matching the original's fade
            double t = (double)(GetTickCount64() - d->noFadeStart) / dur;
            if (t >= 1.0) {
                d->noFade = d->noFadeTo;
                KillTimer(hwnd, 2);
            } else {
                // ease-out, so it settles rather than stopping dead
                double e = 1.0 - (1.0 - t) * (1.0 - t);
                d->noFade = d->noFadeFrom + (d->noFadeTo - d->noFadeFrom) * e;
            }
            InvalidateRect(hwnd, &d->noRect, FALSE);
            return 0;
        }
        break;

    case WM_LBUTTONUP: {
        POINT p = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        if (PtInRect(&d->yesRect, p)) DestroyWindow(hwnd);
        return 0;
    }

    case WM_MOUSELEAVE:
        d->tracked = 0;
        if (d->yesHot != -1) {
            d->yesHot = -1;
            InvalidateRect(hwnd, &d->yesRect, FALSE);
        }
        if (d->noHot) {
            d->noHot = 0;
            d->noFadeFrom = d->noFade;
            d->noFadeTo = 0.0;
            d->noFadeStart = GetTickCount64();
            SetTimer(hwnd, 2, 16, NULL);
        }
        return 0;

    case WM_NCDESTROY:
        // The icon is shared with the icon cache and may still be in use by
        // another box, so it is deliberately not destroyed here.
        delete d;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Victim list and icon cache.
//
// Two things used to run on EVERY box spawn, and both are slow on a VM:
//
//   1. A full enumeration of System32. The directory holds thousands of
//      entries and scanning it is disk bound, so a single spawn could stall
//      for many seconds - which is exactly what the "delete-box spawn #2
//      took=14161ms" log line was. The list is now enumerated ONCE and
//      cached.
//   2. Icon extraction for the chosen binary. SHDefExtractIconW opens the
//      file and rasterises its resources, so it is not cheap either. The
//      results are now cached by path, and a background thread warms the
//      cache before the boxes start appearing.
//
// The 133 KB stack array that held the file names is gone too - it was
// re-zeroed on every spawn.
// ---------------------------------------------------------------------------

#define VICTIM_MAX 256

static wchar_t* g_victimNames = NULL;   // VICTIM_MAX entries of MAX_PATH
static int     g_victimCount = 0;
static volatile LONG g_victimState = 0;  // 0 = none, 1 = building, 2 = ready
static CRITICAL_SECTION g_victimCs;
static volatile LONG g_victimCsReady = 0;

static void VictimCsInit(void) {
    if (InterlockedCompareExchange(&g_victimCsReady, 1, 0) == 0)
        InitializeCriticalSection(&g_victimCs);
}

static void BuildVictimList(void) {
    wchar_t sysDir[MAX_PATH];
    if (!GetSystemDirectoryW(sysDir, MAX_PATH)) return;

    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*.exe", sysDir);

    wchar_t* names = (wchar_t*)malloc((size_t)VICTIM_MAX * MAX_PATH * sizeof(wchar_t));
    if (!names) return;

    int count = 0;
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        // Extremely unlikely, but fall back to the unfiltered listing rather
        // than refusing to show a dialog at all.
        _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", sysDir);
        hFind = FindFirstFileW(pattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE) { free(names); return; }
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (count < VICTIM_MAX) {
            wchar_t* slot = names + (size_t)count * MAX_PATH;
            wcsncpy_s(slot, MAX_PATH, fd.cFileName, _TRUNCATE);
            count++;
        }
    } while (FindNextFileW(hFind, &fd) && count < VICTIM_MAX);
    FindClose(hFind);

    if (count <= 0) { free(names); return; }

    VictimCsInit();
    EnterCriticalSection(&g_victimCs);
    if (g_victimNames) free(g_victimNames);
    g_victimNames = names;
    g_victimCount = count;
    LeaveCriticalSection(&g_victimCs);
}

// Makes sure the list exists. The first caller builds it; anyone else waits
// for that build to finish.
static bool EnsureVictimList(void) {
    LONG st = InterlockedCompareExchange(&g_victimState, 1, 0);
    if (st == 0) {
        BuildVictimList();
        InterlockedExchange(&g_victimState, 2);
        return true;
    }
    if (st == 1) {
        for (int i = 0; i < 6000; i++) {
            if (InterlockedCompareExchange(&g_victimState, 0, 0) == 2) return true;
            Sleep(10);
        }
        return false;
    }
    return true;
}

// Picks a random System32 executable name.
static bool PickVictim(wchar_t* out, size_t cap) {
    if (!EnsureVictimList()) return false;
    VictimCsInit();
    EnterCriticalSection(&g_victimCs);
    int n = g_victimCount;
    if (n > 0 && g_victimNames)
        wcsncpy_s(out, cap, g_victimNames + (size_t)RndBelow(n) * MAX_PATH, _TRUNCATE);
    LeaveCriticalSection(&g_victimCs);
    return out[0] != 0;
}

// --- icon cache -------------------------------------------------------------

#define ICON_CACHE_MAX 192

struct CachedIcon {
    wchar_t path[MAX_PATH];
    HICON   icon;
};

static CachedIcon g_iconCache[ICON_CACHE_MAX];
static int        g_iconCacheCount = 0;
static CRITICAL_SECTION g_iconCs;
static volatile LONG g_iconCsReady = 0;

static void IconCsInit(void) {
    if (InterlockedCompareExchange(&g_iconCsReady, 1, 0) == 0)
        InitializeCriticalSection(&g_iconCs);
}

// Returns the icon for a file, extracting it only the first time that file
// is used. Cached icons are deliberately never destroyed: a dialog that is
// still on screen may be painting with one, and the cache is bounded anyway.
static HICON GetFileIcon(const wchar_t* full) {
    IconCsInit();
    EnterCriticalSection(&g_iconCs);
    for (int i = 0; i < g_iconCacheCount; i++) {
        if (_wcsicmp(g_iconCache[i].path, full) == 0) {
            HICON hit = g_iconCache[i].icon;
            LeaveCriticalSection(&g_iconCs);
            return hit;
        }
    }
    LeaveCriticalSection(&g_iconCs);

    HICON h = NULL;
    if (FAILED(SHDefExtractIconW(full, 0, 0, &h, NULL, MD(0x40))) || !h) {
        SHFILEINFOW fileInfo = {};
        SHGetFileInfoW(full, 0, &fileInfo, sizeof(fileInfo), SHGFI_ICON);
        h = fileInfo.hIcon;
    }
    if (!h) return NULL;

    IconCsInit();
    EnterCriticalSection(&g_iconCs);
    if (g_iconCacheCount < ICON_CACHE_MAX) {
        wcsncpy_s(g_iconCache[g_iconCacheCount].path, MAX_PATH, full, _TRUNCATE);
        g_iconCache[g_iconCacheCount].icon = h;
        g_iconCacheCount++;
    }
    LeaveCriticalSection(&g_iconCs);
    return h;
}

// Builds the System32 file list off the spawn path, so the first box of the
// show does not pay for a directory scan. Metadata is warmed separately,
// per series, by SeriesMetaWarmThread.
static DWORD WINAPI VictimWarmThread(LPVOID) {
    EnsureVictimList();
    return 0;
}

// --- pre-chosen series victims -----------------------------------------------
// The whole series' victim files are chosen ONCE, when the series is
// scheduled, instead of being picked per spawn. A spawn then does nothing
// but read the next name: no System32 lookup, no RNG, no lock.
#define DELETE_SERIES_MAX 64

static wchar_t g_seriesVictims[DELETE_SERIES_MAX][MAX_PATH];
static int     g_seriesVictimCount = 0;

// The victim the next spawn should use. Set by the pump right before it
// spawns; empty means "pick one yourself".
static wchar_t g_nextVictim[MAX_PATH] = L"";

// Per-series pre-fetched metadata. Fetched in the background at schedule time
// so spawning a box does zero disk I/O: no version-info parsing, no file
// attribute stat, no icon extraction. Boxes just copy from here.
struct SeriesMeta {
    wchar_t desc[128];
    wchar_t company[128];
    wchar_t version[64];
    wchar_t size[64];
    wchar_t date[96];
    HICON   icon;
    bool    haveMeta;
};

static SeriesMeta g_seriesMeta[DELETE_SERIES_MAX];
static int        g_seriesMetaCount = 0;
static volatile LONG g_seriesMetaReady = 0;

// Current box index within the active series (declared later in the file;
// forward-declared so SayonaraSpawnDeleteBox can use it).
extern int g_seriesIdx;

// Warms version info, file size/date and icons for the files a series is
// about to use, in the background. Boxes spawned later just copy from
// g_seriesMeta[] - no disk I/O at all.
static DWORD WINAPI SeriesMetaWarmThread(LPVOID p) {
    wchar_t sysDir[MAX_PATH];
    if (!GetSystemDirectoryW(sysDir, MAX_PATH)) {
        InterlockedExchange(&g_seriesMetaReady, 2);
        return 0;
    }

    for (int i = 0; i < g_seriesMetaCount; i++) {
        SeriesMeta* m = &g_seriesMeta[i];
        m->haveMeta = false;
        m->desc[0] = m->company[0] = m->version[0] = 0;
        m->size[0] = m->date[0] = 0;
        m->icon = NULL;

        wchar_t full[MAX_PATH];
        _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", sysDir,
                     g_seriesVictims[i]);

        // Version info (description, company, version).
        DWORD vh = 0;
        DWORD vsize = GetFileVersionInfoSizeW(full, &vh);
        if (vsize) {
            LPVOID vbuf = malloc(vsize);
            if (vbuf && GetFileVersionInfoW(full, vh, vsize, vbuf)) {
                VS_FIXEDFILEINFO* ffi = NULL;
                UINT flen = 0;
                if (VerQueryValueW(vbuf, L"\\", (LPVOID*)&ffi, &flen) && ffi && flen) {
                    swprintf_s(m->version, 64, L"%s %u.%u.%u.%u", g_lang->version,
                               HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                               HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
                }

                UINT uiLang = 0, uiCP = 0;
                UINT tlen = 0;
                VerQueryValueW(vbuf, L"\\VarFileInfo\\Translation", (LPVOID*)&uiLang, &tlen);
                if (!tlen) { uiLang = 0x0409; uiCP = 0x04b0; }

                wchar_t key[96];
                wchar_t* val = NULL;
                UINT vlen = 0;

                swprintf_s(key, 96, L"\\StringFileInfo\\%04x%04x\\FileDescription", uiLang, uiCP);
                if (VerQueryValueW(vbuf, key, (LPVOID*)&val, &vlen) && val && val[0])
                    _snwprintf_s(m->desc, 128, _TRUNCATE, L"%s %s", g_lang->desc, val);

                swprintf_s(key, 96, L"\\StringFileInfo\\%04x%04x\\CompanyName", uiLang, uiCP);
                if (VerQueryValueW(vbuf, key, (LPVOID*)&val, &vlen) && val && val[0])
                    _snwprintf_s(m->company, 128, _TRUNCATE, L"%s %s", g_lang->company, val);
            }
            if (vbuf) free(vbuf);
        }

        // File size + creation date.
        ULARGE_INTEGER bytes = {};
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(full, GetFileExInfoStandard, &fad)) {
            bytes.LowPart = fad.nFileSizeLow;
            bytes.HighPart = fad.nFileSizeHigh;
            wchar_t szBuf[64];
            FormatSize(bytes.QuadPart, szBuf, 64);
            _snwprintf_s(m->size, 64, _TRUNCATE, L"%s %s", g_lang->size, szBuf);

            FILETIME local;
            FileTimeToLocalFileTime(&fad.ftCreationTime, &local);
            SYSTEMTIME st;
            FileTimeToSystemTime(&local, &st);
            wchar_t dateBuf[64], timeBuf[64];
            GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, dateBuf, 64);
            GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, timeBuf, 64);
            _snwprintf_s(m->date, 96, _TRUNCATE, L"%s %s %s", g_lang->created, dateBuf, timeBuf);
        }

        // Icon (cached by GetFileIcon).
        m->icon = GetFileIcon(full);
        m->haveMeta = true;
    }

    InterlockedExchange(&g_seriesMetaReady, 2);
    return 0;
}

// Phase timings for the most recent spawn. A single "took=14161ms" number
// says nothing about WHERE the time went; these do.
static volatile LONG64 g_phPickUs = 0, g_phVersionUs = 0, g_phAttrUs = 0;
static volatile LONG64 g_phIconUs = 0, g_phCreateUs = 0;

static inline LONG64 NowUsFast(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (LONG64)(((double)c.QuadPart * 1000000.0) / (double)f.QuadPart);
}

HWND SayonaraSpawnDeleteBox(int lifetimeMs) {
    if (!g_classReg) {
        ReloadTheme();
        ReloadIcons();
        SelectLanguage();
        {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "delete-box lang primary=0x%03x ui=0x%04x user=0x%04x sys=0x%04x",
                     LID_PRIMARY(GetUserDefaultUILanguage()),
                     GetUserDefaultUILanguage(), GetUserDefaultLangID(),
                     GetSystemDefaultLangID());
            CrashLog(buf);
        }
        {
            char buf2[64];
            snprintf(buf2, sizeof(buf2), "delete-box lid primary=0x%03x sub=0x%02x",
                     LID_PRIMARY(GetUserDefaultUILanguage()),
                     LID_SUB(GetUserDefaultUILanguage()));
            CrashLog(buf2);
        }

        WNDCLASSW wc = {};
        wc.lpfnWndProc = SayonaraDeleteProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"SayonaraDeleteDialog";
        RegisterClassW(&wc);
        g_classReg = true;
    }

    // Random "victim" executable from System32. Only .exe files: the dialog is
    // about deleting a program, and System32 always has plenty. The name for
    // this box was chosen when the series was scheduled, so there is nothing
    // to look up here - the list is only consulted as a fallback.
    wchar_t sysDir[MAX_PATH];
    if (!GetSystemDirectoryW(sysDir, MAX_PATH)) return NULL;

    LONG64 phT = NowUsFast();
    wchar_t victim[MAX_PATH] = L"";
    if (g_nextVictim[0]) {
        wcsncpy_s(victim, MAX_PATH, g_nextVictim, _TRUNCATE);
    } else if (!PickVictim(victim, MAX_PATH)) {
        CrashLog("delete-box: no System32 executable available");
        return NULL;
    }

    wchar_t full[MAX_PATH];
    _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", sysDir, victim);
    g_phPickUs = NowUsFast() - phT;

    SayonaraDlg* d = new SayonaraDlg();
    ZeroMemory(d, sizeof(*d));
    d->yesHot = -1;
    d->noHot = 0;
    d->noFade = 0.0;
    d->noFadeFrom = 0.0;
    d->noFadeTo = 0.0;
    d->noFadeStart = 0;
    d->yesFocused = 1;   // "Sim" is the default button, as in the original

    // Text lines: name / Type / Size / Date modified
    wcsncpy_s(d->name, victim, _TRUNCATE);

    // Use pre-fetched metadata when available (zero disk I/O), otherwise read
    // from disk. Metadata is pre-fetched for every box in the series at
    // schedule time by SeriesMetaWarmThread.
    const SeriesMeta* smeta = NULL;
    if (g_seriesMetaReady == 2 && g_seriesIdx < g_seriesMetaCount)
        smeta = &g_seriesMeta[g_seriesIdx];

    d->desc[0] = 0;
    d->company[0] = 0;
    d->version[0] = 0;
    phT = NowUsFast();
    if (smeta && smeta->haveMeta) {
        _snwprintf_s(d->desc, 128, _TRUNCATE, L"%s", smeta->desc);
        _snwprintf_s(d->company, 128, _TRUNCATE, L"%s", smeta->company);
        _snwprintf_s(d->version, 64, _TRUNCATE, L"%s", smeta->version);
    } else {
        DWORD vh = 0;
        DWORD vsize = GetFileVersionInfoSizeW(full, &vh);
        if (vsize) {
            LPVOID vbuf = malloc(vsize);
            if (vbuf && GetFileVersionInfoW(full, vh, vsize, vbuf)) {
                VS_FIXEDFILEINFO* ffi = NULL;
                UINT flen = 0;
                if (VerQueryValueW(vbuf, L"\\", (LPVOID*)&ffi, &flen) && ffi && flen) {
                    // Labelled like the other rows: "File version: 0.1.0.0"
                    swprintf_s(d->version, 64, L"%s %u.%u.%u.%u", g_lang->version,
                               HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                               HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
                }

                UINT uiLang = 0, uiCP = 0;
                UINT tlen = 0;
                VerQueryValueW(vbuf, L"\\VarFileInfo\\Translation", (LPVOID*)&uiLang, &tlen);
                if (!tlen) { uiLang = 0x0409; uiCP = 0x04b0; }

                wchar_t key[96];
                wchar_t* val = NULL;
                UINT vlen = 0;

                swprintf_s(key, 96, L"\\StringFileInfo\\%04x%04x\\FileDescription", uiLang, uiCP);
                if (VerQueryValueW(vbuf, key, (LPVOID*)&val, &vlen) && val && val[0])
                    _snwprintf_s(d->desc, 128, _TRUNCATE, L"%s %s", g_lang->desc, val);

                swprintf_s(key, 96, L"\\StringFileInfo\\%04x%04x\\CompanyName", uiLang, uiCP);
                if (VerQueryValueW(vbuf, key, (LPVOID*)&val, &vlen) && val && val[0])
                    _snwprintf_s(d->company, 128, _TRUNCATE, L"%s %s", g_lang->company, val);
            }
            if (vbuf) free(vbuf);
        }
    }
    g_phVersionUs = NowUsFast() - phT;

    d->type[0] = 0;

    phT = NowUsFast();
    ULARGE_INTEGER bytes = {};
    if (smeta && smeta->haveMeta) {
        _snwprintf_s(d->size, 64, _TRUNCATE, L"%s", smeta->size);
        _snwprintf_s(d->date, 96, _TRUNCATE, L"%s", smeta->date);
    } else {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(full, GetFileExInfoStandard, &fad)) {
            bytes.LowPart = fad.nFileSizeLow;
            bytes.HighPart = fad.nFileSizeHigh;
            wchar_t szBuf[64];
            FormatSize(bytes.QuadPart, szBuf, 64);
            _snwprintf_s(d->size, 64, _TRUNCATE, L"%s %s", g_lang->size, szBuf);

            // Creation time, formatted for the user's locale - that is what gives
            // "25/9/2026 18:46" on a pt-BR machine and "9/25/2026 6:46 PM" on en-US.
            FILETIME local;
            FileTimeToLocalFileTime(&fad.ftCreationTime, &local);
            SYSTEMTIME st;
            FileTimeToSystemTime(&local, &st);
            wchar_t dateBuf[64], timeBuf[64];
            GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, dateBuf, 64);
            GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, timeBuf, 64);
            _snwprintf_s(d->date, 96, _TRUNCATE, L"%s %s %s", g_lang->created, dateBuf, timeBuf);
        } else {
            _snwprintf_s(d->size, 64, _TRUNCATE, L"%s 0 bytes", g_lang->size);
            _snwprintf_s(d->date, 96, _TRUNCATE, L"%s -", g_lang->created);
        }
    }
    g_phAttrUs = NowUsFast() - phT;

    // File icon (full path so the shell resolves the real extension icon).
    // Cached by path - extraction opens the binary and is far too slow to do
    // on every spawn. Pre-fetched metadata covers it when available.
    phT = NowUsFast();
    d->fileIcon = smeta && smeta->haveMeta ? smeta->icon : GetFileIcon(full);
    g_phIconUs = NowUsFast() - phT;

    // Layout (identical math to the original)
    int marginX = MD(12);
    int marginY = MD(16);
    int width = MD(0x1d2);
    int iconSize = MD(0x28);
    int gap = MD(16);
    int fileIconSize = MD(0x40);
    int lineH = MD(16);

    HDC hdc = GetDC(NULL);
    HGDIOBJ oldFont = SelectObject(hdc, g_dlgFont);
    RECT meas = { 0, 0, width - (iconSize + gap + marginX) - marginX, MD(400) };
    DrawTextW(hdc, g_lang->question, -1, &meas,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
    int qh = meas.bottom - meas.top;
    SelectObject(hdc, oldFont);
    ReleaseDC(NULL, hdc);

    int right = width - marginX;
    d->qLeft = iconSize + gap + marginX;
    d->qTop = marginY;
    d->qRight = right;
    d->qBottom = qh + marginY;
    d->shellX = marginX;
    d->shellY = marginY;
    d->shellIconSize = iconSize;
    d->iconX = d->qLeft;
    d->iconY = qh + MD(8) + iconSize;
    d->fileIconSize = fileIconSize;
    d->textLeft = marginX + d->qLeft + fileIconSize;
    d->textTop = d->iconY;
    d->lineH = lineH;

    // Detail rows actually rendered: name + whichever of description/company/
    // version exist + created + size.
    d->lineCount = 1 + (d->desc[0] ? 1 : 0) + (d->company[0] ? 1 : 0) +
                   (d->version[0] ? 1 : 0) + 2;

    int btnY = lineH * d->lineCount + d->textTop + MD(9);
    int btnH = MD(24);
    int btnW = MD(88);
    SetRect(&d->noRect, (width - btnW) - marginX, btnY, right, btnY + btnH);
    SetRect(&d->yesRect, d->noRect.left - MD(8) - btnW, btnY, d->noRect.left - MD(8), btnY + btnH);

    RECT want = { 0, 0, width, MD(11) + btnY + btnH };
    AdjustWindowRectEx(&want, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, WS_EX_DLGMODALFRAME);
    int dw = want.right - want.left;
    int dh = want.bottom - want.top;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    int x, y;
    if (g_placement == SAYONARA_PLACEMENT_CASCADE) {
        // Walking cascade: each box lands where the previous one was, then the
        // head steps on by one dialog width/height in a chosen direction. The
        // direction is re-rolled often so the stack wanders across the screen
        // instead of running in a straight line off one corner, and it flips
        // immediately when a step would leave the work area so the walk never
        // gets stuck against an edge.
        RECT wr;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wr, 0);

        int stepX = dw / 3; if (stepX < MD(24)) stepX = MD(24);
        int stepY = MD(26);

        // One box per thread spawns concurrently, so the whole head update is
        // under a lock to stop two boxes claiming the same slot.
        CascadeLock();

        EnterCriticalSection(&g_cascadeCs);
        if (!g_cascadeReady) {
            g_cascadeX = wr.left + MD(24) + RndBelow(sw / 3);
            g_cascadeY = wr.top  + MD(24) + RndBelow(sh / 4);
            g_cascadeDX = 1;
            g_cascadeDY = 1;
            g_cascadeSlot = 0;
            g_cascadeReady = 1;
        }

        LONG slot = g_cascadeSlot++;
        if (slot > 24) { g_cascadeSlot = 0; g_cascadeX = wr.left + MD(24) + RndBelow(sw / 3);
                         g_cascadeY = wr.top + MD(24) + RndBelow(sh / 4); }

        // Re-roll direction frequently (roughly every other box, sometimes
        // every box) so consecutive boxes visibly change heading.
        if (slot % 2 == 0 || (RndBelow(4) == 0)) {
            static const int kDirs[4][2] = { {1,1}, {1,-1}, {-1,1}, {-1,-1} };
            const int* dir = kDirs[RndBelow(4)];
            g_cascadeDX = dir[0];
            g_cascadeDY = dir[1];
        }

        x = g_cascadeX;
        y = g_cascadeY;

        int nx = x + g_cascadeDX * stepX;
        int ny = y + g_cascadeDY * stepY;

        // Bounce off the work-area edges.
        if (nx + dw > wr.right - MD(8) || nx < wr.left + MD(8)) g_cascadeDX = -g_cascadeDX;
        if (ny + dh > wr.bottom - MD(8) || ny < wr.top + MD(8)) g_cascadeDY = -g_cascadeDY;
        g_cascadeX = x + g_cascadeDX * stepX;
        g_cascadeY = y + g_cascadeDY * stepY;

        // Never let the head itself end up outside the work area.
        if (g_cascadeX + dw > wr.right - MD(8)) g_cascadeX = wr.right - dw - MD(8);
        if (g_cascadeY + dh > wr.bottom - MD(8)) g_cascadeY = wr.bottom - dh - MD(8);
        if (g_cascadeX < wr.left + MD(8)) g_cascadeX = wr.left + MD(8);
        if (g_cascadeY < wr.top + MD(8)) g_cascadeY = wr.top + MD(8);

        LeaveCriticalSection(&g_cascadeCs);
    } else {
        // Anywhere on screen, but never off the edges and never so low that
        // the title bar falls below the work area.
        int maxX = sw - dw; if (maxX < 0) maxX = 0;
        int maxY = sh - dh; if (maxY < 0) maxY = 0;
        x = maxX > 0 ? RndBelow(maxX + 1) : 0;
        y = maxY > 0 ? RndBelow(maxY + 1) : 0;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    phT = NowUsFast();
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SayonaraDeleteDialog", g_lang->title,
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                                x, y, dw, dh, NULL, NULL, GetModuleHandleW(NULL), d);
    g_phCreateUs = NowUsFast() - phT;
    if (!hwnd) {
        delete d;
        return NULL;
    }

    ApplyDarkTitleBar(hwnd, g_darkTheme);
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
    if (lifetimeMs > 0) SetTimer(hwnd, 1, (UINT)lifetimeMs, NULL);
    return hwnd;
}

// ---------------------------------------------------------------------------
// Delete box queue.
//
// ONE pump thread owns every delete box. Boxes used to each get their own
// thread with its own PeekMessage loop, which meant payload 3's 52 boxes kept
// 52 threads spinning a Sleep(8) wakeup loop - around 6500 wakeups a second -
// and starved payload 3's bouncing explorer into a visible stutter. Now a
// single thread runs a real GetMessage loop for all of them, so the cost is
// flat no matter how many boxes are on screen.
// ---------------------------------------------------------------------------

#define WM_SAYONARA_SCHEDULE (WM_APP + 0x51)
#define WM_SAYONARA_CLOSEALL (WM_APP + 0x52)
#define WM_SAYONARA_PUMPDONE (WM_APP + 0x53)
#define SAYONARA_TIMER_ID    1

// Starts the shared pump and returns once its window exists, so the caller
// can post straight to it. Any helper thread spawned earlier is finished by
// the time this returns.
HWND SayonaraEnsurePump(void);

// Must cover the largest scheduled series (payload 3 uses 53), otherwise the
// overflow boxes never get closed at the payload handover.
static const int DELETE_BOX_MAX = 64;
static HWND g_deleteBoxes[DELETE_BOX_MAX];
static LONG g_deleteBoxCount = 0;   // only touched on the pump thread

static HWND     g_pumpWnd = NULL;
static HANDLE   g_pumpThread = NULL;
static HANDLE   g_pumpReady = NULL;
static volatile LONG g_pumpStarting = 0;

// The series currently being played out by the pump's timer.
static DWORD* g_seriesDelays = NULL;
static int    g_seriesCount = 0;
static int    g_seriesIdx = 0;

struct SeriesMsg { DWORD* delays; int count; DWORD startTick; };

static void PumpSpawnNext(void) {
    if (g_seriesIdx >= g_seriesCount) {
        KillTimer(g_pumpWnd, SAYONARA_TIMER_ID);
        free(g_seriesDelays);
        g_seriesDelays = NULL;
        g_seriesCount = 0;
        g_seriesIdx = 0;
        return;
    }

    // QPC, not GetTickCount64: the latter has ~15.6 ms granularity and cannot
    // resolve a sub-tick operation like this one.
    LARGE_INTEGER fc, cc;
    QueryPerformanceFrequency(&fc);
    QueryPerformanceCounter(&cc);
    ULONGLONG t0 = (ULONGLONG)cc.QuadPart;

    // Hand the box its pre-chosen victim, so the spawn does no lookups at all.
    g_nextVictim[0] = 0;
    {
        VictimCsInit();
        EnterCriticalSection(&g_victimCs);
        if (g_seriesIdx < g_seriesVictimCount && g_seriesVictimCount > 0)
            wcsncpy_s(g_nextVictim, MAX_PATH, g_seriesVictims[g_seriesIdx], _TRUNCATE);
        LeaveCriticalSection(&g_victimCs);
    }

    HWND box = SayonaraSpawnDeleteBox(0);
    g_nextVictim[0] = 0;
    QueryPerformanceCounter(&cc);
    ULONGLONG t1 = (ULONGLONG)cc.QuadPart;
    {
        static volatile LONG n = 0, maxUs = 0, totUs = 0;
        LONG us = (LONG)(((double)(t1 - t0) * 1000000.0) / (double)fc.QuadPart);
        totUs += us;
        InterlockedIncrement(&n);
        if (us > maxUs) maxUs = us;
        // Log EVERY spawn, not every 10th: an aborted run can end before the
        // 10th box exists, which silently produced zero output before.
        {
            char b[200];
            snprintf(b, sizeof(b), "delete-box spawn #%ld took=%.2fms (avg=%.2fms max=%.2fms)",
                     n, us / 1000.0, (double)totUs / n / 1000.0, maxUs / 1000.0);
            CrashLog(b);
            // Where the time actually went. Without this, a slow box is just
            // one opaque number.
            snprintf(b, sizeof(b),
                     "delete-box #%ld phases: pick=%.2f version=%.2f attr=%.2f icon=%.2f create=%.2f ms",
                     n, g_phPickUs / 1000.0, g_phVersionUs / 1000.0,
                     g_phAttrUs / 1000.0, g_phIconUs / 1000.0, g_phCreateUs / 1000.0);
            CrashLog(b);
        }
    }
    if (box) {
        LONG slot = g_deleteBoxCount;
        if (slot < DELETE_BOX_MAX) {
            g_deleteBoxes[slot] = box;
            g_deleteBoxCount = slot + 1;
        }
    }

    g_seriesIdx++;
    if (g_seriesIdx < g_seriesCount) {
        DWORD d = g_seriesDelays[g_seriesIdx];
        if (d < USER_TIMER_MINIMUM) d = USER_TIMER_MINIMUM;
        SetTimer(g_pumpWnd, SAYONARA_TIMER_ID, d, NULL);
    } else {
        KillTimer(g_pumpWnd, SAYONARA_TIMER_ID);
        free(g_seriesDelays);
        g_seriesDelays = NULL;
        g_seriesCount = 0;
        g_seriesIdx = 0;
    }
}

static LRESULT CALLBACK PumpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == SAYONARA_TIMER_ID) PumpSpawnNext();
        return 0;

    case WM_SAYONARA_SCHEDULE: {
        // A new series replaces any still playing.
        if (g_seriesDelays) { free(g_seriesDelays); g_seriesDelays = NULL; }
        const SeriesMsg* sm = (const SeriesMsg*)wp;
        if (!sm || !sm->delays || sm->count <= 0) {
            g_seriesCount = 0;
            g_seriesIdx = 0;
            return 0;
        }
        g_seriesCount = sm->count;
        g_seriesIdx = 0;
        g_seriesDelays = (DWORD*)malloc(sizeof(DWORD) * (size_t)sm->count);
        if (!g_seriesDelays) { g_seriesCount = 0; return 0; }
        memcpy(g_seriesDelays, sm->delays, sizeof(DWORD) * (size_t)sm->count);

        // Choose the victim file for every box in this series NOW, so that
        // spawning one later is just a lookup. Only the FIRST box of the
        // show needs System32 at all.
        {
            int n = sm->count < DELETE_SERIES_MAX ? sm->count : DELETE_SERIES_MAX;
            wchar_t chosen[DELETE_SERIES_MAX][MAX_PATH];
            for (int i = 0; i < n; i++) {
                if (!PickVictim(chosen[i], MAX_PATH))
                    chosen[i][0] = 0;
            }
            VictimCsInit();
            EnterCriticalSection(&g_victimCs);
            for (int i = 0; i < n; i++)
                wcsncpy_s(g_seriesVictims[i], MAX_PATH, chosen[i], _TRUNCATE);
            g_seriesVictimCount = n;

            // Pre-zero metadata so boxes can check whether their entry is ready.
            g_seriesMetaCount = n;
            for (int i = 0; i < n; i++)
                g_seriesMeta[i].haveMeta = false;
            InterlockedExchange(&g_seriesMetaReady, 0);
            LeaveCriticalSection(&g_victimCs);
        }

        // Extract version info, file size/date and icons for every victim in
        // this series in the background, so each box spawn is just a lookup.
        {
            HANDLE hWarm = CreateThread(NULL, 0, SeriesMetaWarmThread, NULL, 0, NULL);
            if (hWarm) CloseHandle(hWarm);
        }

        // Anchor the first box to the song beat rather than to now.
        DWORD now = GetTickCount();
        DWORD d = g_seriesDelays[0];
        if (sm->startTick > now) d += (sm->startTick - now);

        if (d < USER_TIMER_MINIMUM) d = USER_TIMER_MINIMUM;
        SetTimer(hwnd, SAYONARA_TIMER_ID, d, NULL);
        return 0;
    }

    case WM_SAYONARA_CLOSEALL:
        for (LONG i = 0; i < g_deleteBoxCount && i < DELETE_BOX_MAX; i++) {
            HWND box = g_deleteBoxes[i];
            if (box && IsWindow(box)) PostMessageW(box, WM_CLOSE, 0, 0);
            g_deleteBoxes[i] = NULL;
        }
        g_deleteBoxCount = 0;
        if (g_seriesDelays) { free(g_seriesDelays); g_seriesDelays = NULL; }
        g_seriesCount = 0;
        g_seriesIdx = 0;
        KillTimer(hwnd, SAYONARA_TIMER_ID);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI DeletePumpThread(LPVOID p) {
    (void)p;

    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = PumpProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"SayonaraDeletePump";
        RegisterClassW(&wc);
        reg = true;
    }

    g_pumpWnd = CreateWindowExW(0, L"SayonaraDeletePump", L"", 0,
                                 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                 GetModuleHandleW(NULL), NULL);
    SetEvent(g_pumpReady);

    // Build the System32 file list and warm the icon cache now, off the
    // spawn path, so the first box of the show does not pay for a directory
    // scan and a batch of icon extractions.
    HANDLE hWarm = CreateThread(NULL, 0, VictimWarmThread, NULL, 0, NULL);
    if (hWarm) CloseHandle(hWarm);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

// Starts the shared pump on first use. Returns the pump's message window.
HWND SayonaraEnsurePump(void) {
    if (g_pumpWnd) return g_pumpWnd;

    if (InterlockedCompareExchange(&g_pumpStarting, 1, 0) == 0) {
        g_pumpReady = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_pumpThread = CreateThread(NULL, 0, DeletePumpThread, NULL, 0, NULL);
        if (g_pumpThread) {
            WaitForSingleObject(g_pumpReady, 5000);
            CloseHandle(g_pumpReady);
            g_pumpReady = NULL;
        } else {
            InterlockedExchange(&g_pumpStarting, 0);
            return NULL;
        }
    } else {
        // Another thread is starting it; wait for the window to appear.
        for (int i = 0; i < 500 && !g_pumpWnd; i++) Sleep(10);
    }
    return g_pumpWnd;
}

void SayonaraDeleteScheduleFrom(const DWORD* delays, int count, DWORD startTick) {
    if (!delays || count <= 0) return;

    HWND pump = SayonaraEnsurePump();
    if (!pump) return;

    // Each scheduled series cascades from a fresh origin.
    CascadeLock();
    EnterCriticalSection(&g_cascadeCs);
    g_cascadeSlot = 0;
    g_cascadeReady = 0;
    g_cascadeDX = 1;
    g_cascadeDY = 1;
    LeaveCriticalSection(&g_cascadeCs);

    SeriesMsg sm;
    sm.delays = const_cast<DWORD*>(delays);
    sm.count = count;
    sm.startTick = startTick;

    // Synchronous, so sm and delays stay valid for the call; the pump copies.
    SendMessageW(pump, WM_SAYONARA_SCHEDULE, (WPARAM)&sm, 0);
}

void SayonaraDeleteSchedule(const DWORD* delays, int count) {
    SayonaraDeleteScheduleFrom(delays, count, 0);
}

void SayonaraDeleteCloseAll(void) {
    HWND pump = g_pumpWnd;
    if (!pump) return;

    // Synchronous, so the boxes are really gone before the payload hands over
    // and the process exits.
    SendMessageTimeoutW(pump, WM_SAYONARA_CLOSEALL, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);
}
