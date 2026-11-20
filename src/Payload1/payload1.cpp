#include <windows.h>
#include <string>
#include <vector>
#include <exdisp.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include "../FakeExplorer.h"
#include "../Flags.h"
#include "../SongSync.h"
#include "../CrashLog.h"
#include "../SayonaraDelete.h"
#include "../huffman.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "winmm.lib")

#define MAGIC "APDC"
#define IDR_AUDIO 112
#define IDR_AUDIO2 113
#define IDR_AUDIO3 115
#define IDR_AUDIO_ENDERMANCH 117
#define NUM_NAV 46

DWORD g_audioStartTick = 0;
static BYTE* g_audioDecompressedBuf = NULL;
static WCHAR g_mp3TempPath[MAX_PATH] = {0};
static bool g_mciOpen = false;

static const int NAV_DELAYS[46] = {
    455,   448,   401,   112,   112,
    101,   107,   440,   441,   415,
    224,   232,   416,   448,   424,
    104,   104,   113,   112,   440,
    432,   432,   232,   207,   441,
    424,   432,    95,   112,   104,
    121,   431,   449,   432,   224,
    200,   424,   464,   432,   104,
    119,   112,   112,   417,   448,
    479
};

static const wchar_t* NAV_PATHS[] = {
    L"C:\\",
    L"C:\\Users",
    L"C:\\Windows\\System32",
    L"C:\\Windows\\SysWOW64",
    L"C:\\Windows\\Temp",
    L"C:\\Program Files",
    L"C:\\Program Files (x86)",
    L"C:\\Users\\Public",
    L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",   // This PC
    L"::{26EE0668-A00A-44D7-9371-BEB064C98683}",   // Control Panel
    L"::{59031A47-3F72-44A7-89C5-5595FE6B30EE}",   // User's Files
    L"::{645FF040-5081-101B-9F08-00AA002F954E}",   // Recycle Bin
    L"::{374DE290-123F-4565-9164-39C4925E467B}"    // Downloads
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

static bool WriteUndoReg(const std::wstring& originalFormat) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    std::wstring regPath = dir + L"undo.reg";

    FILE* f = nullptr;
    _wfopen_s(&f, regPath.c_str(), L"w");
    if (!f) return false;
    fprintf(f, "Windows Registry Editor Version 5.00\r\n\r\n");
    fprintf(f, "[HKEY_CURRENT_USER\\Control Panel\\International]\r\n");
    fprintf(f, "\"sShortDate\"=\"%ls\"\r\n", originalFormat.c_str());
    bool ok = fflush(f) == 0;
    fclose(f);
    return ok;
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

// ----------------------------------------------------------------------------
// waveOut PCM engine. No MCI, no Media Foundation, no codecs, no temp files:
// the whole track is an embedded PCM WAV streamed to the sound card in
// 1-second chunks. Also supplies the song clock position, so the timeline
// never touches MCI either. Works XP -> Win11 including Win7/Vista.
// ----------------------------------------------------------------------------

#define WO_BUFS 4
#define WO_CHUNK_MS 1000

static HWAVEOUT g_wo = NULL;
static volatile LONG g_woStop = 0;
static const BYTE* g_woData = NULL;
static LONG g_woTotal = 0;    // source pcm bytes
static LONG g_woFeed = 0;     // next un-fed source byte
static LONG g_woChunk = 0;    // bytes per chunk (1 s of audio)
static LONG g_woAvgBps = 0;   // fmt.nAvgBytesPerSec
static HANDLE g_woThread = NULL;
static WAVEHDR g_woHdrs[WO_BUFS];
static BYTE* g_woBufs[WO_BUFS];
static BYTE* g_woDecoded = NULL;  // decoded PCM, owned by the engine

static DWORD WINAPI WOThread(LPVOID) {
    int idx = 0;
    while (!InterlockedCompareExchange(&g_woStop, 0, 0)) {
        WAVEHDR* h = &g_woHdrs[idx];
        BOOL free_ = ((h->dwFlags & WHDR_PREPARED) == 0) || ((h->dwFlags & WHDR_DONE) != 0);
        BOOL fedAll = (g_woFeed >= g_woTotal);
        BOOL allDone = TRUE;
        for (int i = 0; i < WO_BUFS; i++)
            if ((g_woHdrs[i].dwFlags & WHDR_PREPARED) && !(g_woHdrs[i].dwFlags & WHDR_DONE)) allDone = FALSE;

        if (free_ && !fedAll) {
            LONG take = g_woTotal - g_woFeed;
            if (take > g_woChunk) take = g_woChunk;
            memcpy(g_woBufs[idx], g_woData + g_woFeed, take);
            h->lpData = (LPSTR)g_woBufs[idx];
            h->dwBufferLength = (DWORD)take;
            h->dwFlags = 0;
            MMRESULT mrp = waveOutPrepareHeader(g_wo, h, sizeof(WAVEHDR));
            MMRESULT mrw = waveOutWrite(g_wo, h, sizeof(WAVEHDR));
            CrashLogf("waveOutWrite idx=%d take=%ld prepare=%lu write=%lu feed=%lu total=%lu",
                      idx, take, (unsigned long)mrp, (unsigned long)mrw,
                      (unsigned long)g_woFeed, (unsigned long)g_woTotal);
            g_woFeed += take;
        } else if (fedAll && allDone) {
            break;
        }
        idx = (idx + 1) % WO_BUFS;
        Sleep(2);
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Tiny IMA ADPCM decoder (used when the embedded track is tag 17). Compresses
// 4:1 at full 44.1k mono; waveOut only ever sees plain PCM, so there is no
// dependency on ACM/MCI/MF -- identical behavior on Win7 and Win11.
// ----------------------------------------------------------------------------

static const int IMA_STEP_TABLE[89] = {
      7,   8,   9,  10,  11,  12,  13,  14,  16,  17,  19,  21,  23,  25,  28,
     31,  34,  37,  41,  45,  50,  55,  60,  66,  73,  80,  88,  97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int IMA_INDEX_TABLE[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

// Decodes mono IMA ADPCM to 16-bit PCM. Returns a malloc'd buffer.
static BYTE* DecodeImaMono(const BYTE* src, DWORD srcLen, DWORD blockAlign,
                           DWORD* outBytes) {
    if (!src || blockAlign < 4) return NULL;
    DWORD nBlocks = srcLen / blockAlign;
    DWORD perBlock = 1 + (blockAlign - 4) * 2;
    BYTE* out = (BYTE*)malloc((size_t)nBlocks * perBlock * 2);
    if (!out) return NULL;
    short* dst = (short*)out;
    for (DWORD i = 0; i < nBlocks; i++) {
        const BYTE* b = src + (size_t)i * blockAlign;
        int predictor = (short)(b[0] | (b[1] << 8));
        int stepIndex = (short)(b[2] | (b[3] << 8));
        if (stepIndex < 0) stepIndex = 0;
        else if (stepIndex > 88) stepIndex = 88;
        *dst++ = (short)predictor;
        for (DWORD j = 4; j < blockAlign; j++) {
            BYTE byte = b[j];
            for (int h = 0; h < 2; h++) {
                int nib = (h == 0) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
                int step = IMA_STEP_TABLE[stepIndex];
                int delta = step >> 3;
                if (nib & 1) delta += step >> 2;
                if (nib & 2) delta += step >> 1;
                if (nib & 4) delta += step;
                if (nib & 8) predictor -= delta; else predictor += delta;
                if (predictor < -32768) predictor = -32768;
                else if (predictor > 32767) predictor = 32767;
                stepIndex += IMA_INDEX_TABLE[nib];
                if (stepIndex < 0) stepIndex = 0;
                else if (stepIndex > 88) stepIndex = 88;
                *dst++ = (short)predictor;
            }
        }
    }
    *outBytes = (DWORD)((BYTE*)dst - out);
    return out;
}

static void WOStop();

static int WOStart(const BYTE* pcm, LONG dataBytes, const WAVEFORMATEX& fmt) {
    WOStop();
    g_woAvgBps = fmt.nAvgBytesPerSec ? fmt.nAvgBytesPerSec : (fmt.nSamplesPerSec * fmt.nBlockAlign);
    g_woChunk = (LONG)((long long)g_woAvgBps * WO_CHUNK_MS / 1000);
    if (g_woChunk < fmt.nBlockAlign) g_woChunk = fmt.nBlockAlign;
    g_woData = pcm;
    g_woTotal = dataBytes;
    g_woFeed = 0;
    InterlockedExchange(&g_woStop, 0);
    for (int i = 0; i < WO_BUFS; i++) {
        g_woBufs[i] = (BYTE*)malloc(g_woChunk);
        ZeroMemory(&g_woHdrs[i], sizeof(WAVEHDR));
    }
    MMRESULT mr = waveOutOpen(&g_wo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    CrashLogf("waveOutOpen fmtTag=%u ch=%u sr=%u bps=%u blockAlign=%u avgBps=%lu -> mr=%lu",
              fmt.wFormatTag, fmt.nChannels, fmt.nSamplesPerSec, fmt.wBitsPerSample,
              fmt.nBlockAlign, (unsigned long)fmt.nAvgBytesPerSec, (unsigned long)mr);
    if (mr != MMSYSERR_NOERROR) {
        // 24-bit PCM isn't always supported by waveOut -- fall back to 16-bit.
        if (fmt.wBitsPerSample > 16) {
            CrashLogf("waveOutOpen failed %lu on %u-bit, trying 16-bit fallback",
                      (unsigned long)mr, (unsigned)fmt.wBitsPerSample);
            WAVEFORMATEX fmt16 = fmt;
            fmt16.wFormatTag = WAVE_FORMAT_PCM;
            fmt16.wBitsPerSample = 16;
            fmt16.nBlockAlign = fmt16.nChannels * 2;
            fmt16.nAvgBytesPerSec = fmt16.nSamplesPerSec * fmt16.nBlockAlign;
            fmt16.cbSize = 0;

            // Convert 24-bit -> 16-bit stereo.
            DWORD outBytes = dataBytes * 2 / 3;
            BYTE* out16 = (BYTE*)malloc(outBytes);
            if (out16) {
                DWORD src = 0, dst = 0;
                while (src + 2 < dataBytes) {   // 3 bytes per sample per channel
                    for (int ch = 0; ch < fmt.nChannels; ch++) {
                        int v24 = pcm[src] | (pcm[src + 1] << 8) | (pcm[src + 2] << 16);
                        if (v24 & 0x800000) v24 |= ~0xFFFFFF;
                        short v16 = (short)(v24 >> 8);
                        out16[dst++] = (BYTE)(v16 & 0xFF);
                        out16[dst++] = (BYTE)((v16 >> 8) & 0xFF);
                        src += 3;
                    }
                }
                DWORD outDataLen = dst;
                mr = waveOutOpen(&g_wo, WAVE_MAPPER, &fmt16, 0, 0, CALLBACK_NULL);
                if (mr == MMSYSERR_NOERROR) {
                    g_woData = out16;
                    g_woTotal = (LONG)outDataLen;
                    g_woAvgBps = fmt16.nAvgBytesPerSec;
                    g_woChunk = (LONG)((long long)g_woAvgBps * WO_CHUNK_MS / 1000);
                    if (g_woChunk < fmt16.nBlockAlign) g_woChunk = fmt16.nBlockAlign;
                    g_woDecoded = out16;   // free on WOStop
                    InterlockedExchange(&g_woStop, 0);
                    for (int i = 0; i < WO_BUFS; i++) {
                        g_woBufs[i] = (BYTE*)malloc(g_woChunk);
                        ZeroMemory(&g_woHdrs[i], sizeof(WAVEHDR));
                    }
                    g_woThread = CreateThread(NULL, 0, WOThread, NULL, 0, NULL);
                    CrashLogf("waveOutOpen 16-bit fallback succeeded");
                    return MMSYSERR_NOERROR;
                }
                free(out16);
            }
        }
        DbgLog(L"waveOutOpen failed", (HRESULT)mr);
        WOStop();
        return (int)mr;
    }
    g_woThread = CreateThread(NULL, 0, WOThread, NULL, 0, NULL);
    return MMSYSERR_NOERROR;
}

static void WOStop() {
    if (!g_wo && !g_woThread) return;
    InterlockedExchange(&g_woStop, 1);
    if (g_wo) waveOutReset(g_wo);
    if (g_woThread) {
        WaitForSingleObject(g_woThread, 800);
        CloseHandle(g_woThread);
        g_woThread = NULL;
    }
    if (g_wo) {
        for (int i = 0; i < WO_BUFS; i++)
            if (g_woHdrs[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(g_wo, &g_woHdrs[i], sizeof(WAVEHDR));
        waveOutClose(g_wo);
        g_wo = NULL;
    }
    for (int i = 0; i < WO_BUFS; i++) {
        if (g_woBufs[i]) free(g_woBufs[i]);
        g_woBufs[i] = NULL;
        ZeroMemory(&g_woHdrs[i], sizeof(WAVEHDR));
    }
    if (g_woDecoded) { free(g_woDecoded); g_woDecoded = NULL; }
    if (g_audioDecompressedBuf) { free(g_audioDecompressedBuf); g_audioDecompressedBuf = NULL; }
    CrashLogf("play-badapple stop freed=%d", g_audioDecompressedBuf ? 1 : 0);
    g_woData = NULL; g_woTotal = 0; g_woFeed = 0;
}

// Current playback position in ms, sample-accurate from the waveOut device.
long long SongAudioPosMs() {
    if (!g_wo || g_woAvgBps == 0) return -1;
    MMTIME mt;
    ZeroMemory(&mt, sizeof(mt));
    mt.wType = TIME_BYTES;
    if (waveOutGetPosition(g_wo, &mt, sizeof(mt)) != MMSYSERR_NOERROR)
        return (long long)g_woFeed * 1000 / g_woAvgBps;
    return (long long)mt.u.cb * 1000 / g_woAvgBps;
}

// ---------------------------------------------------------------------------
// LZ77 decompressor (matches compress_audio.cpp format).
// Format: 0x00 + literal | 0x01 + len + dist(2LE) | 0x02 = end
// Input is delta-encoded PCM; output must be delta-decoded to get original.
// ---------------------------------------------------------------------------
static BYTE* Lz77Decompress(const BYTE* src, DWORD srcSize, DWORD* outSize) {
    DWORD cap = srcSize * 4 + 4096;
    BYTE* out = (BYTE*)malloc(cap);
    if (!out) { CrashLogf("Lz77Decompress: malloc failed cap=%lu", cap); return NULL; }
    DWORD outPos = 0, i = 0;
    while (i < srcSize) {
        BYTE op = src[i++];
        if (op == 0x02) break; // end
        if (op == 0x00 && i + 1 <= srcSize) {
            if (outPos + 1 > cap) { cap *= 2; out = (BYTE*)realloc(out, cap); if (!out) { CrashLogf("Lz77Decompress: realloc failed at lit i=%lu", i); return NULL; } }
            out[outPos++] = src[i++];
        } else if (op == 0x01 && i + 3 <= srcSize) {
            BYTE len = src[i++];
            BYTE d0 = src[i++];
            BYTE d1 = src[i++];
            int dist = d0 | (d1 << 8);
            if (dist == 0 || dist > (int)outPos) {
                CrashLogf("Lz77Decompress: bad match at i=%lu len=%u dist=%d outPos=%lu",
                          i-4, len, dist, outPos);
                free(out); return NULL;
            }
            if (outPos + len > cap) { cap = outPos + len + 4096; out = (BYTE*)realloc(out, cap); if (!out) { CrashLogf("Lz77Decompress: realloc failed at match i=%lu", i); return NULL; } }
            for (int k = 0; k < len; k++) {
                out[outPos] = out[outPos - dist];
                outPos++;
            }
        } else {
            CrashLogf("Lz77Decompress: unknown opcode 0x%02x at i=%lu srcSize=%lu outPos=%lu",
                      op, i-1, srcSize, outPos);
            free(out);
            return NULL;
        }
    }
    *outSize = outPos;
    CrashLogf("Lz77Decompress: OK outPos=%lu srcSize=%lu", outPos, srcSize);
    return out;
}

// Delta-decode PCM in-place (inverse of compress_audio.cpp DeltaEncode).
static void DeltaDecode(BYTE* data, DWORD dataSize, const WAVEFORMATEX* wf) {
    int bytesPerSample = wf->nBlockAlign / wf->nChannels;
    int channels = wf->nChannels;
    DWORD samples = dataSize / wf->nBlockAlign;

    for (int ch = 0; ch < channels; ch++) {
        int prev = 0;
        bool havePrev = false;
        for (DWORD s = 0; s < samples; s++) {
            DWORD baseOff = s * wf->nBlockAlign + ch * bytesPerSample;
            // Read signed delta (little-endian).
            int delta = 0;
            for (int b = 0; b < bytesPerSample; b++) {
                delta |= (int)data[baseOff + b] << (8 * b);
            }
            // Sign-extend.
            if (bytesPerSample == 1) {
                if (delta & 0x80) delta |= ~0xFF;
            } else if (bytesPerSample == 2) {
                if (delta & 0x8000) delta |= ~0xFFFF;
            } else if (bytesPerSample == 3) {
                if (delta & 0x800000) delta |= ~0xFFFFFF;
            }
            int val = havePrev ? (prev + delta) : delta;
            prev = val;
            havePrev = true;
            for (int b = 0; b < bytesPerSample; b++) {
                data[baseOff + b] = (BYTE)((val >> (8 * b)) & 0xFF);
            }
        }
    }
}

// Parse a WAV from an in-memory buffer. Returns fmt + data pointer into buf
// (data is NOT copied - it points into buf, which must stay alive).
static bool ParseWavFromMemory(const BYTE* buf, DWORD size, WAVEFORMATEX* fmt, const BYTE** data, LONG* dataLen) {
    if (!buf || size < 44 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) return false;
    DWORD off = 12;
    const WAVEFORMATEX* wf = NULL;
    const BYTE* d = NULL; DWORD dl = 0;
    while (off + 8 <= size) {
        DWORD len = *(DWORD*)(buf + off + 4);
        if (memcmp(buf + off, "fmt ", 4) == 0 && len >= 16) wf = (const WAVEFORMATEX*)(buf + off + 8);
        else if (memcmp(buf + off, "data", 4) == 0) { d = buf + off + 8; dl = len; }
        off += 8 + len + (len & 1);
    }
    if (!wf || !d || dl == 0) return false;
    if (wf->wFormatTag != WAVE_FORMAT_PCM && wf->wFormatTag != 17 && wf->wFormatTag != 3) return false;
    *fmt = *wf;
    *data = d;
    *dataLen = (LONG)dl;
    return true;
}

// Extract a WAV from an RCDATA resource. Accepts plain PCM (tag 1) and
// IMA ADPCM (tag 17) -- PlayBadApple decodes the latter to PCM itself.
static bool LoadWavResource(DWORD resId, WAVEFORMATEX* fmt, const BYTE** data, LONG* dataLen) {
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(resId), (LPCWSTR)RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hg = LoadResource(NULL, hRes);
    const BYTE* p = (const BYTE*)(hg ? LockResource(hg) : NULL);
    DWORD size = p ? SizeofResource(NULL, hRes) : 0;
    return ParseWavFromMemory(p, size, fmt, data, dataLen);
}

// Load the LZ77-compressed WAV resource, decompress it at runtime, and parse.
// Format: "APDC" magic + WAVEFORMATEX + DWORD dataSize + LZ77(delta-encoded PCM)
// Returns fmt and a heap-allocated WAV buffer (freed via g_audioDecompressedBuf).
static bool LoadCompressedWavResource(DWORD resId, WAVEFORMATEX* fmt, BYTE** outData, LONG* outDataLen) {
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(resId), (LPCWSTR)RT_RCDATA);
    if (!hRes) { CrashLogf("audio resId=%lu: FindResourceW failed", resId); return false; }
    HGLOBAL hg = LoadResource(NULL, hRes);
    const BYTE* compressed = (const BYTE*)(hg ? LockResource(hg) : NULL);
    DWORD compressedSize = compressed ? SizeofResource(NULL, hRes) : 0;
    if (!compressed) { CrashLogf("audio resId=%lu: LockResource failed", resId); return false; }
    if (compressedSize < sizeof(DWORD) * 2 + sizeof(WAVEFORMATEX)) {
        CrashLogf("audio resId=%lu: compressed too small (%lu bytes)", resId, compressedSize);
        return false;
    }

    // Verify magic.
    if (memcmp(compressed, MAGIC, 4) != 0) {
        CrashLogf("audio resId=%lu: bad magic %02x %02x %02x %02x (want A P D C)",
                  resId, compressed[0], compressed[1], compressed[2], compressed[3]);
        return false;
    }

    // Read format + original data size.
    WAVEFORMATEX wf = {};
    memcpy(&wf, compressed + 4, sizeof(wf));
    DWORD origDataSize = 0;
    memcpy(&origDataSize, compressed + 4 + sizeof(wf), sizeof(origDataSize));
    CrashLogf("audio resId=%lu: magic=OK fmtTag=%u ch=%u sr=%u bps=%u blockAlign=%u origDataSize=%lu compSize=%lu",
              resId, wf.wFormatTag, wf.nChannels, wf.nSamplesPerSec, wf.wBitsPerSample,
              wf.nBlockAlign, origDataSize, compressedSize);

    // Read Huffman frequency table (256 DWORDs).
    int freq[HUFF_MAX_SYMS];
    memcpy(freq, compressed + 4 + sizeof(wf) + sizeof(origDataSize), sizeof(freq));

    // Build Huffman tree.
    HuffNode nodes[HUFF_MAX_NODES];
    int nodeCount = HuffBuildTree(freq, nodes);
    if (nodeCount == 0) {
        CrashLogf("audio resId=%lu: Huffman tree build failed", resId);
        return false;
    }
    int root = 0;
    // Find root: the last created node (nodeCount-1), or node 0 if single symbol.
    if (nodeCount > 1) root = nodeCount - 1;

    // Huffman-decode bit-packed data into delta-encoded PCM.
    const BYTE* huffData = compressed + 4 + sizeof(wf) + sizeof(origDataSize) + sizeof(freq);
    DWORD huffSize = compressedSize - (4 + sizeof(wf) + sizeof(origDataSize) + sizeof(freq));
    BitReader br;
    BitReaderInit(&br, huffData, huffSize);

    BYTE* deltaPcm = (BYTE*)malloc(origDataSize);
    if (!deltaPcm) { CrashLogf("audio resId=%lu: malloc failed", resId); return false; }

    DWORD decoded = 0;
    while (decoded < origDataSize) {
        int sym = HuffDecodeSymbol(&br, nodes, root);
        if (sym < 0) {
            CrashLogf("audio resId=%lu: Huffman decode failed at byte %lu", resId, decoded);
            free(deltaPcm);
            return false;
        }
        deltaPcm[decoded++] = (BYTE)sym;
    }

    // Delta-decode to get original PCM.
    DeltaDecode(deltaPcm, origDataSize, &wf);

    // Build a full WAV in memory: RIFF + fmt + data chunks.
    DWORD fmtChunkSize = sizeof(WAVEFORMATEX);
    DWORD riffSize = 4 + (8 + fmtChunkSize) + (8 + origDataSize);
    DWORD totalSize = 12 + fmtChunkSize + 8 + origDataSize;
    BYTE* wavBuf = (BYTE*)malloc(totalSize);
    if (!wavBuf) { free(deltaPcm); CrashLogf("audio resId=%lu: malloc failed", resId); return false; }

    DWORD pos = 0;
    memcpy(wavBuf + pos, "RIFF", 4); pos += 4;
    memcpy(wavBuf + pos, &riffSize, 4); pos += 4;
    memcpy(wavBuf + pos, "WAVE", 4); pos += 4;
    memcpy(wavBuf + pos, "fmt ", 4); pos += 4;
    memcpy(wavBuf + pos, &fmtChunkSize, 4); pos += 4;
    memcpy(wavBuf + pos, &wf, fmtChunkSize); pos += fmtChunkSize;
    memcpy(wavBuf + pos, "data", 4); pos += 4;
    memcpy(wavBuf + pos, &origDataSize, 4); pos += 4;
    memcpy(wavBuf + pos, deltaPcm, origDataSize); pos += origDataSize;
    free(deltaPcm);

    const BYTE* wavData = NULL;
    LONG wavDataLen = 0;
    WAVEFORMATEX wavFmt = {};
    if (!ParseWavFromMemory(wavBuf, totalSize, &wavFmt, &wavData, &wavDataLen)) {
        free(wavBuf);
        CrashLogf("audio resId=%lu: ParseWavFromMemory failed on rebuilt WAV", resId);
        return false;
    }

    *fmt = wavFmt;
    *outData = (BYTE*)wavData;   // points to the PCM data chunk inside wavBuf
    *outDataLen = wavDataLen;
    CrashLogf("audio resId=%lu: decompressed OK fmtTag=%u dataLen=%ld",
              resId, wavFmt.wFormatTag, wavDataLen);
    return true;
}

void PlayBadApple() {
    // Set time zero immediately so time calculations (wobble watcher,
    // payload sync fallbacks) work even if audio fails to load.
    g_audioStartTick = GetTickCount();

    UINT nDevs = waveOutGetNumDevs();
    CrashLogf("play-badapple: waveOutGetNumDevs=%u", (unsigned int)nDevs);

    // badapple.mp3 embedded RCDATA, played via MCI mciSendString.
    DWORD resId = OriginalFlag() ? IDR_AUDIO3
             : EndermanchFlag() ? IDR_AUDIO_ENDERMANCH
             : IDR_AUDIO2;
    WAVEFORMATEX fmt;
    const BYTE* resData = NULL;   // points into resource (don't free)
    LONG dataLen = 0;
    {
        wchar_t sel[512];
        wchar_t env[16] = L"<unset>";
        GetEnvironmentVariableW(L"ERRER_ORIGINAL", env, 16);
        wsprintfW(sel, L"audio resId=%lu original=%d endermanch=%d env=%s cmd=%s",
                  resId, OriginalFlag() ? 1 : 0, EndermanchFlag() ? 1 : 0,
                  env, GetCommandLineW());
        DbgLog(sel, S_OK);
    }
    if (!LoadWavResource(resId, &fmt, &resData, &dataLen)) {
        DbgLog(L"wav resource parse failed", E_FAIL);
        return;
    }
    {
        wchar_t info[96];
        wsprintfW(info, L"audio fmt tag=%u ch=%u bytes=%ld", fmt.wFormatTag, fmt.nChannels, dataLen);
        DbgLog(info, S_OK);
    }

    CrashLogf("play-badapple start fmt=%u ch=%u bytes=%ld src=%s blockAlign=%u",
              fmt.wFormatTag, fmt.nChannels, dataLen,
              OriginalFlag() ? "original" : "resource",
              fmt.nBlockAlign);

    BYTE* decodedBuf = NULL;
    if (fmt.wFormatTag == 17) {
        CrashLogf("play-badapple: decoding IMA ADPCM blockAlign=%u dataLen=%lu",
                  fmt.nBlockAlign, (unsigned long)dataLen);
        DWORD pcmBytes = 0;
        BYTE* pcm = DecodeImaMono(resData, (DWORD)dataLen, fmt.nBlockAlign, &pcmBytes);
        if (!pcm) { DbgLog(L"ima decode failed", E_OUTOFMEMORY); return; }
        CrashLogf("play-badapple: IMA decode OK pcmBytes=%lu", (unsigned long)pcmBytes);
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 2;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 2;
        fmt.cbSize = 0;
        resData = pcm;
        dataLen = (LONG)pcmBytes;
        decodedBuf = pcm;
    }
    if (fmt.wFormatTag == 3) {
        int bps = fmt.nBlockAlign / fmt.nChannels;
        DWORD samples = dataLen / bps;
        DWORD pcmBytes = samples * fmt.nChannels * 2;
        BYTE* pcm16 = (BYTE*)malloc(pcmBytes);
        if (!pcm16) { DbgLog(L"float->pcm malloc failed", E_OUTOFMEMORY); return; }
        DWORD dst = 0;
        for (DWORD s = 0; s < samples; s++) {
            for (int ch = 0; ch < fmt.nChannels; ch++) {
                DWORD off = s * fmt.nBlockAlign + ch * bps;
                float f = 0.0f;
                memcpy(&f, resData + off, 4);
                short v = (short)(f * 32767.0f);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm16[dst++] = (BYTE)(v & 0xFF);
                pcm16[dst++] = (BYTE)((v >> 8) & 0xFF);
            }
        }
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = fmt.nChannels * 2;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize = 0;
        resData = pcm16;
        dataLen = (LONG)pcmBytes;
        decodedBuf = pcm16;
    }
    g_audioStartTick = GetTickCount();
    if (WOStart(resData, dataLen, fmt) != MMSYSERR_NOERROR) {
        if (decodedBuf) free(decodedBuf);
        MessageBoxA(NULL,
            "errer: audio output failed. waveOutOpen could not open the audio device.\n"
            "The show will continue silently. Check %TEMP%\\errer_crash.log with --debug for details.",
            "errer - audio error", MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    if (decodedBuf) g_woDecoded = decodedBuf;
    // Re-anchor the instant audio is actually audible.
    for (int i = 0; i < 100; i++) {
        if (SongAudioPosMs() > 0) { g_audioStartTick = GetTickCount(); break; }
        Sleep(5);
    }
}

void StopBadApple() {
    WOStop();
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

// SongSyncWaitBeat, but pumps the window's message queue while it waits so the
// ExplorerBrowser keeps painting. The stock wait uses a bare Sleep loop, which
// leaves the window unrendered when payload 1 is on the live-clock path.
static void WaitBeatPump(DWORD songMs) {
    DWORD target = SongSyncTarget(songMs);
    if (target == 0) return;
    while ((LONG)(GetTickCount() - (LONG)target) < 0) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(4);
    }
}

static const DWORD P1_DELETE_DELAYS[] = { 100, 13821 };

void runPayload1() {
    if (!EndermanchFlag() && SongSyncWaitHostFlag()) SongSyncWaitHost();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t* fakeFormat = L"ER/R/25ER";
    std::wstring originalFormat = ReadRegString(HKEY_CURRENT_USER, L"Control Panel\\International", L"sShortDate");
    HWND hwnd = NULL;
    if (originalFormat != fakeFormat && !NoDateChangeFlag()) {
        if (!WriteUndoReg(originalFormat)) {
            MessageBoxW(NULL,
                       L"Couldn't make undo.reg in the current directory. Not continuing.",
                       L"error", MB_OK | MB_ICONERROR);
            CoUninitialize();
            TerminateProcess(GetCurrentProcess(), 4);
            return;
        }
        SetShortDateFormat(fakeFormat);
        hwnd = KillRestartExplorer();
    } else {
        hwnd = CreateFakeExplorer(L"C:\\", 80, 80, 540, 320);
    }

    std::wstring userProfile = GetExpandedUserProfile();
    std::vector<const wchar_t*> dirs;
    for (size_t i = 0; i < _countof(NAV_PATHS); i++) dirs.push_back(NAV_PATHS[i]);
    dirs.push_back(userProfile.c_str());

    DWORD songAnchor = g_audioStartTick;
    {
        wchar_t buf[64];
        DWORD n = GetEnvironmentVariableW(L"ERRER_SONG_START", buf, 64);
        if (n > 0 && n < 64) songAnchor = (DWORD)_wtoi(buf);
    }

    // Summon the delete dialogs right after the song starts, before the first
    // navigation ever lands, so they're on screen from the very beginning.
    SayonaraDeleteSchedule(P1_DELETE_DELAYS,
                           (int)(sizeof(P1_DELETE_DELAYS) / sizeof(P1_DELETE_DELAYS[0])));

    if (hwnd) {
        if (EndermanchFlag()) {
            // --endermanch: no beat anchoring, no clock skips, no delay fixes.
            // Exactly one second, then plain local pacing through the navs.
            PumpMessagesFor(1000);
            for (int i = 0; i < NUM_NAV; i++) {
                if (!IsWindow(hwnd)) break;
                NavigateFakeExplorer(hwnd, dirs[i % dirs.size()]);
                PumpMessagesFor(NAV_DELAYS[i]);
            }
        } else if (SongSyncAvailable() || songAnchor) {
            long long beats[NUM_NAV];
            long long acc = 1609;
            for (int i = 0; i < NUM_NAV; i++) { beats[i] = acc; acc += NAV_DELAYS[i]; }

            if (SongSyncAvailable()) {
                for (int i = 0; i < NUM_NAV; i++) {
                    if (!IsWindow(hwnd)) break;
                    WaitBeatPump((DWORD)beats[i]);
                    NavigateFakeExplorer(hwnd, dirs[i % dirs.size()]);
                }
            } else for (int i = 0; i < NUM_NAV; i++) {
                if (!IsWindow(hwnd)) break;
                DWORD now = GetTickCount();
                DWORD target = songAnchor + (DWORD)beats[i];
                if ((LONG)(now - target) >= 0) continue;
                PumpMessagesFor(target - now);
                NavigateFakeExplorer(hwnd, dirs[i % dirs.size()]);
            }
        } else {
            PumpMessagesFor(1609);
            for (int i = 0; i < NUM_NAV; i++) {
                if (!IsWindow(hwnd)) break;
                NavigateFakeExplorer(hwnd, dirs[i % dirs.size()]);
                PumpMessagesFor(NAV_DELAYS[i]);
            }
        }
        // Keep the explorer alive across payload transitions; don't destroy it
        // here so it carries over into payload 2.
    }
    // Keep the explorer and delete dialogs alive across payload transitions
    // (P1 -> P2); do not close them here.
    CrashLog("end");
    TerminateProcess(GetCurrentProcess(), 0);
}
