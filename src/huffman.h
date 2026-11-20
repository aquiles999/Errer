// huffman.h - Huffman coding for delta-encoded audio.
// Small delta values (0, +/-1, +/-2...) are very common in audio and get
// short codes; rare large values get longer codes.
// Bit-level I/O is self-contained, no external dependencies.

#pragma once
#include <windows.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Bit writer (compressor side)
// ---------------------------------------------------------------------------
struct BitWriter {
    BYTE* buf;
    DWORD cap;
    DWORD pos;     // byte position
    DWORD bitPos;  // bit position within current byte (0-7)
    DWORD totalBits;
};

static void BitWriterInit(BitWriter* bw, DWORD maxBits) {
    bw->cap = (maxBits / 8) + 16;
    bw->buf = (BYTE*)malloc(bw->cap);
    bw->pos = 0;
    bw->bitPos = 0;
    bw->totalBits = 0;
    if (bw->buf) ZeroMemory(bw->buf, bw->cap);
}

static void BitWriterPutBit(BitWriter* bw, int bit) {
    if (bw->pos >= bw->cap) {
        bw->cap *= 2;
        bw->buf = (BYTE*)realloc(bw->buf, bw->cap);
    }
    if (bit) bw->buf[bw->pos] |= (BYTE)(1 << bw->bitPos);
    bw->bitPos++;
    bw->totalBits++;
    if (bw->bitPos == 8) {
        bw->bitPos = 0;
        bw->pos++;
    }
}

static void BitWriterPutBits(BitWriter* bw, DWORD code, int codeBits) {
    for (int i = 0; i < codeBits; i++) {
        BitWriterPutBit(bw, (int)((code >> i) & 1));
    }
}

static void BitWriterFlush(BitWriter* bw) {
    // pad to byte boundary with zeros
    if (bw->bitPos != 0) {
        bw->bitPos = 0;
        bw->pos++;
    }
}

// ---------------------------------------------------------------------------
// Bit reader (decompressor side)
// ---------------------------------------------------------------------------
struct BitReader {
    const BYTE* buf;
    DWORD size;
    DWORD pos;
    DWORD bitPos;
    DWORD totalBits;
};

static void BitReaderInit(BitReader* br, const BYTE* buf, DWORD size) {
    br->buf = buf;
    br->size = size;
    br->pos = 0;
    br->bitPos = 0;
    br->totalBits = 0;
}

static int BitReaderGetBit(BitReader* br) {
    if (br->pos >= br->size) return -1;
    int bit = (int)((br->buf[br->pos] >> br->bitPos) & 1);
    br->bitPos++;
    br->totalBits++;
    if (br->bitPos == 8) {
        br->bitPos = 0;
        br->pos++;
    }
    return bit;
}

// ---------------------------------------------------------------------------
// Huffman tree
// ---------------------------------------------------------------------------
#define HUFF_MAX_SYMS 256
#define HUFF_MAX_NODES (HUFF_MAX_SYMS * 2)

struct HuffNode {
    int freq;
    int left, right;   // child indices, -1 if leaf
    int symbol;        // byte value if leaf
    int codeLen;
    DWORD code;
};

// Build Huffman tree from frequency counts. Returns total node count.
static int HuffBuildTree(const int* freq, HuffNode* nodes) {
    int symCount = 0;
    for (int i = 0; i < HUFF_MAX_SYMS; i++)
        if (freq[i] > 0) symCount++;

    if (symCount == 0) return 0;
    if (symCount == 1) {
        int root = 0;
        for (int i = 0; i < HUFF_MAX_SYMS; i++)
            if (freq[i] > 0) { nodes[0].symbol = i; break; }
        nodes[0].freq = freq[nodes[0].symbol];
        nodes[0].left = -1;
        nodes[0].right = -1;
        nodes[0].codeLen = 1;
        nodes[0].code = 0;
        return 1;
    }

    // Create leaf nodes
    int nodeCount = 0;
    for (int i = 0; i < HUFF_MAX_SYMS; i++) {
        if (freq[i] > 0) {
            nodes[nodeCount].freq = freq[i];
            nodes[nodeCount].left = -1;
            nodes[nodeCount].right = -1;
            nodes[nodeCount].symbol = i;
            nodes[nodeCount].codeLen = 0;
            nodes[nodeCount].code = 0;
            nodeCount++;
        }
    }

    // Build tree using simple selection (256 symbols is small)
    int active[HUFF_MAX_SYMS];
    int activeCount = 0;
    for (int i = 0; i < nodeCount; i++) active[activeCount++] = i;

    int nextNode = nodeCount;
    while (activeCount > 1) {
        int i1 = -1, i2 = -1;
        for (int a = 0; a < activeCount; a++) {
            int idx = active[a];
            if (i1 == -1 || nodes[idx].freq < nodes[i1].freq) { i2 = i1; i1 = idx; }
            else if (i2 == -1 || nodes[idx].freq < nodes[i2].freq) { i2 = idx; }
        }
        nodes[nextNode].freq = nodes[i1].freq + nodes[i2].freq;
        nodes[nextNode].left = i1;
        nodes[nextNode].right = i2;
        nodes[nextNode].symbol = -1;
        nodes[nextNode].codeLen = 0;
        nodes[nextNode].code = 0;

        int newActive[HUFF_MAX_SYMS];
        int nac = 0;
        for (int a = 0; a < activeCount; a++)
            if (active[a] != i1 && active[a] != i2)
                newActive[nac++] = active[a];
        newActive[nac++] = nextNode;
        memcpy(active, newActive, nac * sizeof(int));
        activeCount = nac;
        nextNode++;
    }

    // Assign codes by DFS from root
    int root = active[0];
    struct Frame { int node; DWORD code; int len; };
    Frame frames[HUFF_MAX_NODES * 2];
    int fsp = 0;
    frames[fsp++] = { root, 0, 0 };

    while (fsp > 0) {
        Frame f = frames[--fsp];
        int node = f.node;
        if (nodes[node].left == -1 && nodes[node].right == -1) {
            nodes[node].code = f.code;
            nodes[node].codeLen = f.len;
        } else {
            frames[fsp++] = { nodes[node].left,  f.code,           f.len + 1 };
            frames[fsp++] = { nodes[node].right, f.code | ((DWORD)1 << f.len), f.len + 1 };
        }
    }

    return nextNode;
}

// Decode a single symbol from bit reader using Huffman tree.
static int HuffDecodeSymbol(BitReader* br, const HuffNode* nodes, int root) {
    int node = root;
    while (nodes[node].left != -1 || nodes[node].right != -1) {
        int bit = BitReaderGetBit(br);
        if (bit < 0) return -1;
        node = (bit == 0) ? nodes[node].left : nodes[node].right;
        if (node < 0) return -1;
    }
    return nodes[node].symbol;
}
