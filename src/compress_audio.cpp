// compress_audio.cpp - Build-time compressor for badapple.wav
// Delta-encodes PCM audio then Huffman-compresses the delta values.
// Small deltas (0, +/-1, +/-2...) are very common in audio and get short codes.
// Self-contained, works on Win7. Matching decompressor in payload1.cpp.
#include <windows.h>
#include <stdio.h>
#include "huffman.h"

#define MAGIC "APDC"   // Audio Delta Packed

// Compressed format (Huffman-coded delta PCM):
//   "APDC" (4 bytes) + WAVEFORMATEX + DWORD origDataSize
//   + 256 x DWORD Huffman frequency counts (1024 bytes)
//   + Huffman-encoded delta-encoded PCM (bit-packed, byte-padded)

static bool ReadFileBytes(const wchar_t* path, BYTE** out, DWORD* outSize) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, NULL);
    BYTE* buf = (BYTE*)malloc(size);
    if (!buf) { CloseHandle(h); return false; }
    DWORD read = 0;
    if (!ReadFile(h, buf, size, &read, NULL) || read != size) {
        free(buf); CloseHandle(h); return false;
    }
    CloseHandle(h);
    *out = buf;
    *outSize = size;
    return true;
}

static bool WriteFileBytes(const wchar_t* path, const BYTE* data, DWORD size) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data, size, &written, NULL) && written == size;
    CloseHandle(h);
    return ok;
}

// Parse WAV header fields from raw bytes.
static bool ParseWavHeader(const BYTE* buf, DWORD size,
                           WAVEFORMATEX* wf, DWORD* dataOffset, DWORD* dataSize) {
    if (size < 44 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return false;
    DWORD off = 12;
    while (off + 8 <= size) {
        DWORD len = *(DWORD*)(buf + off + 4);
        if (memcmp(buf + off, "fmt ", 4) == 0 && len >= 16) {
            *wf = *(const WAVEFORMATEX*)(buf + off + 8);
        } else if (memcmp(buf + off, "data", 4) == 0) {
            *dataOffset = off + 8;
            *dataSize = len;
            return true;
        }
        off += 8 + len + (len & 1);
    }
    return false;
}

// Delta-encode PCM data in-place.
static void DeltaEncode(BYTE* data, DWORD dataSize, const WAVEFORMATEX* wf) {
    int bytesPerSample = wf->nBlockAlign / wf->nChannels;
    int channels = wf->nChannels;
    DWORD samples = dataSize / wf->nBlockAlign;

    for (int ch = 0; ch < channels; ch++) {
        int prev = 0;
        bool havePrev = false;
        for (DWORD s = 0; s < samples; s++) {
            DWORD baseOff = s * wf->nBlockAlign + ch * bytesPerSample;
            int val = 0;
            for (int b = 0; b < bytesPerSample; b++) {
                val |= (int)data[baseOff + b] << (8 * b);
            }
            if (bytesPerSample == 1) {
                if (val & 0x80) val |= ~0xFF;
            } else if (bytesPerSample == 2) {
                if (val & 0x8000) val |= ~0xFFFF;
            } else if (bytesPerSample == 3) {
                if (val & 0x800000) val |= ~0xFFFFFF;
            }
            int delta = havePrev ? (val - prev) : val;
            prev = val;
            havePrev = true;
            for (int b = 0; b < bytesPerSample; b++) {
                data[baseOff + b] = (BYTE)((delta >> (8 * b)) & 0xFF);
            }
        }
    }
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 3) {
        wprintf(L"Usage: compress_audio.exe <input.wav> <output.cmp>\n");
        return 1;
    }

    BYTE* input = NULL;
    DWORD inputSize = 0;
    if (!ReadFileBytes(argv[1], &input, &inputSize)) {
        wprintf(L"Failed to read %s\n", argv[1]);
        return 1;
    }
    wprintf(L"Input: %s (%lu bytes = %.2f MB)\n", argv[1], inputSize,
            (double)inputSize / (1024.0 * 1024.0));

    WAVEFORMATEX wf = {};
    DWORD dataOff = 0, dataSize = 0;
    if (!ParseWavHeader(input, inputSize, &wf, &dataOff, &dataSize)) {
        wprintf(L"Failed to parse WAV header\n");
        free(input);
        return 1;
    }
    // Normalize WAVE_FORMAT_EXTENSIBLE (0xFFFE) to plain PCM (1). We delta
    // encode raw bytes so the exact subformat doesn't matter; the WAV header
    // just needs to round-trip correctly.
    if (wf.wFormatTag == 0xFFFE) {
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.cbSize = 0;
    }
    wprintf(L"  format: tag=%u ch=%u sr=%u bps=%u blockAlign=%u dataSize=%lu\n",
            wf.wFormatTag, wf.nChannels, wf.nSamplesPerSec, wf.wBitsPerSample,
            wf.nBlockAlign, dataSize);

    BYTE* pcm = (BYTE*)malloc(dataSize);
    if (!pcm) { wprintf(L"Out of memory\n"); free(input); return 1; }
    memcpy(pcm, input + dataOff, dataSize);
    DeltaEncode(pcm, dataSize, &wf);
    wprintf(L"  delta-encoded %lu bytes of PCM\n", dataSize);

    // Count byte frequencies in delta-encoded data.
    int freq[HUFF_MAX_SYMS] = {0};
    for (DWORD i = 0; i < dataSize; i++) freq[pcm[i]]++;

    // Build Huffman tree.
    HuffNode nodes[HUFF_MAX_NODES];
    int nodeCount = HuffBuildTree(freq, nodes);
    if (nodeCount == 0) { wprintf(L"No data to compress\n"); free(pcm); free(input); return 1; }
    wprintf(L"  Huffman tree: %d nodes\n", nodeCount);

    // Write output: magic + WAVEFORMATEX + dataSize + freq table + encoded bits.
    DWORD maxBits = dataSize * 16 + 256;  // upper bound: 16 bits per byte worst case
    BitWriter bw;
    BitWriterInit(&bw, maxBits);
    if (!bw.buf) { wprintf(L"Out of memory\n"); free(pcm); free(input); return 1; }

    DWORD outPos = 0;
    BYTE* out = (BYTE*)malloc(maxBits / 8 + 4096);
    if (!out) { wprintf(L"Out of memory\n"); free(bw.buf); free(pcm); free(input); return 1; }

    out[outPos++] = 'A'; out[outPos++] = 'P'; out[outPos++] = 'D'; out[outPos++] = 'C';
    memcpy(out + outPos, &wf, sizeof(wf)); outPos += sizeof(wf);
    memcpy(out + outPos, &dataSize, sizeof(dataSize)); outPos += sizeof(dataSize);

    // Write Huffman frequency table (256 DWORDs = 1024 bytes).
    for (int i = 0; i < HUFF_MAX_SYMS; i++) {
        memcpy(out + outPos, &freq[i], sizeof(freq[i])); outPos += sizeof(freq[i]);
    }

    // Encode each delta byte using Huffman codes.
    DWORD encodedBytes = 0;
    for (DWORD i = 0; i < dataSize; i++) {
        int sym = pcm[i];
        // Find the HuffNode for this symbol
        for (int n = 0; n < nodeCount; n++) {
            if (nodes[n].left == -1 && nodes[n].right == -1 && nodes[n].symbol == sym) {
                BitWriterPutBits(&bw, nodes[n].code, nodes[n].codeLen);
                encodedBytes++;
                break;
            }
        }
    }
    BitWriterFlush(&bw);

    // Copy bit-packed data into output buffer.
    memcpy(out + outPos, bw.buf, bw.pos); outPos += bw.pos;
    free(bw.buf);

    wprintf(L"  encoded %lu bytes, Huffman output %lu bytes (incl 1024 byte freq table)\n",
            dataSize, outPos);

    if (!WriteFileBytes(argv[2], out, outPos)) {
        wprintf(L"Failed to write %s\n", argv[2]);
        free(out); free(pcm); free(input); return 1;
    }

    wprintf(L"Output: %s (%lu bytes = %.2f MB, ratio %.2fx)\n", argv[2],
            outPos, (double)outPos / (1024.0 * 1024.0),
            (double)inputSize / (double)outPos);

    free(out);
    free(pcm);
    free(input);
    return 0;
}

