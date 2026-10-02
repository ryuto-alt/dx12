#include "resource/EnvironmentConvert.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace dx12e::envconv
{

namespace
{

// ───────────────────────────────────────────────────────────────────────────
//  zlib の inflate(RFC 1950/1951)。EXR の ZIP / ZIPS 圧縮の展開だけに使う小さな実装。
//  (assimp が同梱の zlib と別のシンボルを増やさないため、外部ライブラリを足さずに自前で持つ。)
// ───────────────────────────────────────────────────────────────────────────
struct Huff
{
    uint16_t count[16];
    uint16_t symbol[288];
};

struct Infl
{
    const uint8_t* in;
    size_t         inLen;
    size_t         inPos = 0;
    uint32_t       bitBuf = 0;
    int            bitCnt = 0;
    uint8_t*       out;
    size_t         outCap;
    size_t         outPos = 0;
    bool           err = false;
};

int GetBits(Infl& s, int need)
{
    uint32_t val = s.bitBuf;
    while (s.bitCnt < need)
    {
        if (s.inPos >= s.inLen) { s.err = true; return 0; }
        val |= static_cast<uint32_t>(s.in[s.inPos++]) << s.bitCnt;
        s.bitCnt += 8;
    }
    s.bitBuf = val >> need;
    s.bitCnt -= need;
    return static_cast<int>(val & ((1u << need) - 1));
}

// 戻り値: 0 = 完全 / 正 = 不完全(許容) / 負 = 過剰
int Construct(Huff& h, const uint16_t* length, int n)
{
    for (int l = 0; l <= 15; ++l) h.count[l] = 0;
    for (int i = 0; i < n; ++i) ++h.count[length[i]];
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l <= 15; ++l)
    {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return left;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = static_cast<uint16_t>(offs[l] + h.count[l]);
    for (int i = 0; i < n; ++i)
        if (length[i] != 0) h.symbol[offs[length[i]]++] = static_cast<uint16_t>(i);
    return left;
}

int Decode(Infl& s, const Huff& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len)
    {
        code |= GetBits(s, 1);
        if (s.err) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

bool Codes(Infl& s, const Huff& lencode, const Huff& distcode)
{
    static const uint16_t lens[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint16_t lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint16_t dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;)
    {
        int sym = Decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 256)
        {
            if (s.outPos >= s.outCap) return false;
            s.out[s.outPos++] = static_cast<uint8_t>(sym);
        }
        else if (sym == 256)
            return true;
        else
        {
            sym -= 257;
            if (sym >= 29) return false;
            int len = lens[sym] + GetBits(s, lext[sym]);
            const int ds = Decode(s, distcode);
            if (ds < 0 || ds >= 30) return false;
            const size_t dist = static_cast<size_t>(dists[ds] + GetBits(s, dext[ds]));
            if (s.err || dist > s.outPos || s.outPos + len > s.outCap) return false;
            for (int i = 0; i < len; ++i, ++s.outPos) s.out[s.outPos] = s.out[s.outPos - dist];
        }
    }
}

// zlib ストリームを outCap バイトぶん展開する。ちょうど outCap バイトにならなければ false。
bool ZlibInflate(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap)
{
    if (inLen < 2) return false;
    Infl s{in, inLen};
    s.inPos = 2;   // zlib ヘッダ(CMF, FLG)を飛ばす
    s.out = out;
    s.outCap = outCap;

    static Huff fixedLen, fixedDist;
    static bool fixedReady = false;
    if (!fixedReady)
    {
        uint16_t lengths[288];
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        Construct(fixedLen, lengths, 288);
        for (i = 0; i < 30; ++i) lengths[i] = 5;
        Construct(fixedDist, lengths, 30);
        fixedReady = true;
    }

    int last;
    do
    {
        last = GetBits(s, 1);
        const int type = GetBits(s, 2);
        if (s.err) return false;
        if (type == 0)
        {
            s.bitBuf = 0; s.bitCnt = 0;
            if (s.inPos + 4 > s.inLen) return false;
            const unsigned len  = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
            const unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
            s.inPos += 4;
            if (len != (~nlen & 0xFFFFu)) return false;
            if (s.inPos + len > s.inLen || s.outPos + len > s.outCap) return false;
            std::memcpy(s.out + s.outPos, s.in + s.inPos, len);
            s.inPos += len;
            s.outPos += len;
        }
        else if (type == 1)
        {
            if (!Codes(s, fixedLen, fixedDist)) return false;
        }
        else if (type == 2)
        {
            static const uint16_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            const int nlen  = GetBits(s, 5) + 257;
            const int ndist = GetBits(s, 5) + 1;
            const int ncode = GetBits(s, 4) + 4;
            if (s.err || nlen > 286 || ndist > 30) return false;
            uint16_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint16_t>(GetBits(s, 3));
            if (s.err) return false;
            Huff lencode, distcode;
            if (Construct(lencode, lengths, 19) != 0) return false;
            int idx = 0;
            while (idx < nlen + ndist)
            {
                int sym = Decode(s, lencode);
                if (sym < 0) return false;
                if (sym < 16)
                    lengths[idx++] = static_cast<uint16_t>(sym);
                else
                {
                    int len = 0, rep;
                    if (sym == 16)
                    {
                        if (idx == 0) return false;
                        len = lengths[idx - 1];
                        rep = 3 + GetBits(s, 2);
                    }
                    else if (sym == 17) rep = 3 + GetBits(s, 3);
                    else                rep = 11 + GetBits(s, 7);
                    if (s.err || idx + rep > nlen + ndist) return false;
                    while (rep--) lengths[idx++] = static_cast<uint16_t>(len);
                }
            }
            if (lengths[256] == 0) return false;
            int err = Construct(lencode, lengths, nlen);
            if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
            err = Construct(distcode, lengths + nlen, ndist);
            if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
            if (!Codes(s, lencode, distcode)) return false;
        }
        else
            return false;
    } while (!last);
    return s.outPos == outCap;
}

// ───────────────────────────────────────────────────────────────────────────
//  OpenEXR: PIZ(Huffman + ウェーブレット + LUT)の展開
//  ImfPizCompressor / ImfHuf / ImfWav の公開仕様どおりの実装。
// ───────────────────────────────────────────────────────────────────────────
constexpr int kHufEncBits = 16;
constexpr int kHufEncSize = (1 << kHufEncBits) + 1;
constexpr int kHufDecBits = 14;
constexpr int kHufDecSize = 1 << kHufDecBits;
constexpr int kHufDecMask = kHufDecSize - 1;
constexpr int kShortZeroRun = 59;
constexpr int kLongZeroRun  = 63;
constexpr int kShortestLongRun = 2 + kLongZeroRun - kShortZeroRun;

struct HufDec
{
    int len = 0;
    int lit = 0;
    std::vector<int>* p = nullptr;   // 長い符号(> kHufDecBits)の候補
};

struct HufBits
{
    const uint8_t* p;
    const uint8_t* end;
    uint64_t c = 0;
    int lc = 0;
};

inline bool HufGetBits(HufBits& b, int n, uint64_t& out)
{
    while (b.lc < n)
    {
        if (b.p >= b.end) return false;
        b.c = (b.c << 8) | *b.p++;
        b.lc += 8;
    }
    b.lc -= n;
    out = (b.c >> b.lc) & ((1ull << n) - 1);
    return true;
}

bool HufUncompress(const uint8_t* comp, size_t nComp, uint16_t* raw, size_t nRaw)
{
    if (nComp == 0) return nRaw == 0;
    if (nComp < 20) return false;
    auto rd = [&](size_t o) {
        return static_cast<uint32_t>(comp[o]) | (static_cast<uint32_t>(comp[o + 1]) << 8) |
               (static_cast<uint32_t>(comp[o + 2]) << 16) | (static_cast<uint32_t>(comp[o + 3]) << 24);
    };
    const uint32_t im = rd(0), iM = rd(4), nBits = rd(12);
    if (im >= static_cast<uint32_t>(kHufEncSize) || iM >= static_cast<uint32_t>(kHufEncSize) || im > iM)
        return false;
    const uint8_t* ptr = comp + 20;
    const uint8_t* end = comp + nComp;
    if (static_cast<uint64_t>(ptr - comp) + (nBits + 7) / 8 > nComp) return false;

    // 符号長テーブルの読み出し
    std::vector<uint64_t> hcode(kHufEncSize, 0);
    {
        HufBits b{ptr, end};
        for (uint32_t i = im; i <= iM; ++i)
        {
            uint64_t l;
            if (!HufGetBits(b, 6, l)) return false;
            hcode[i] = l;
            if (l == static_cast<uint64_t>(kLongZeroRun))
            {
                uint64_t z;
                if (!HufGetBits(b, 8, z)) return false;
                int zerun = static_cast<int>(z) + kShortestLongRun;
                if (i + zerun > iM + 1) return false;
                while (zerun--) hcode[i++] = 0;
                --i;
            }
            else if (l >= static_cast<uint64_t>(kShortZeroRun))
            {
                int zerun = static_cast<int>(l) - kShortZeroRun + 2;
                if (i + zerun > iM + 1) return false;
                while (zerun--) hcode[i++] = 0;
                --i;
            }
        }
        ptr = b.p;
    }
    // 正準符号表
    {
        uint64_t n[59] = {};
        for (int i = 0; i < kHufEncSize; ++i) n[hcode[i]] += 1;
        uint64_t c = 0;
        for (int i = 58; i > 0; --i)
        {
            const uint64_t nc = (c + n[i]) >> 1;
            n[i] = c;
            c = nc;
        }
        for (int i = 0; i < kHufEncSize; ++i)
        {
            const int l = static_cast<int>(hcode[i]);
            if (l > 0) hcode[i] = static_cast<uint64_t>(l) | (n[l]++ << 6);
        }
    }
    auto hLen  = [&](uint64_t v) { return static_cast<int>(v & 63); };
    auto hCode = [&](uint64_t v) { return v >> 6; };

    // 復号表
    std::vector<HufDec> dec(kHufDecSize);
    std::vector<std::unique_ptr<std::vector<int>>> owned;
    bool ok = true;
    for (uint32_t s = im; s <= iM && ok; ++s)
    {
        const uint64_t c = hCode(hcode[s]);
        const int l = hLen(hcode[s]);
        if (c >> l) { ok = false; break; }
        if (l > kHufDecBits)
        {
            HufDec& pl = dec[c >> (l - kHufDecBits)];
            if (pl.len) { ok = false; break; }
            if (!pl.p)
            {
                owned.push_back(std::make_unique<std::vector<int>>());
                pl.p = owned.back().get();
            }
            pl.lit++;
            pl.p->push_back(static_cast<int>(s));
        }
        else if (l)
        {
            const size_t base = static_cast<size_t>(c << (kHufDecBits - l));
            const size_t cnt  = static_cast<size_t>(1) << (kHufDecBits - l);
            for (size_t i = 0; i < cnt; ++i)
            {
                HufDec& pl = dec[base + i];
                if (pl.len || pl.p) { ok = false; break; }
                pl.len = l;
                pl.lit = static_cast<int>(s);
            }
        }
    }
    if (!ok) return false;

    // 復号
    const uint32_t rlc = iM;
    uint64_t c = 0;
    int lc = 0;
    const uint8_t* in = ptr;
    const uint8_t* ie = ptr + (nBits + 7) / 8;
    uint16_t* out = raw;
    uint16_t* oe = raw + nRaw;

    auto getChar = [&]() { c = (c << 8) | *in++; lc += 8; };
    auto getCode = [&](int po) -> bool {
        if (static_cast<uint32_t>(po) == rlc)
        {
            if (lc < 8)
            {
                if (in >= ie) return false;
                getChar();
            }
            lc -= 8;
            unsigned cs = static_cast<unsigned char>(c >> lc);
            if (out + cs > oe || out == raw) return false;
            const uint16_t s = out[-1];
            while (cs-- > 0) *out++ = s;
        }
        else if (out < oe)
            *out++ = static_cast<uint16_t>(po);
        else
            return false;
        return true;
    };

    while (in < ie)
    {
        getChar();
        while (lc >= kHufDecBits)
        {
            const HufDec& pl = dec[static_cast<size_t>((c >> (lc - kHufDecBits)) & kHufDecMask)];
            if (pl.len)
            {
                lc -= pl.len;
                if (!getCode(pl.lit)) return false;
            }
            else
            {
                if (!pl.p) return false;
                int j;
                for (j = 0; j < pl.lit; ++j)
                {
                    const int sym = (*pl.p)[j];
                    const int l = hLen(hcode[sym]);
                    while (lc < l && in < ie) getChar();
                    if (lc >= l && hCode(hcode[sym]) == ((c >> (lc - l)) & ((1ull << l) - 1)))
                    {
                        lc -= l;
                        if (!getCode(sym)) return false;
                        break;
                    }
                }
                if (j == pl.lit) return false;
            }
        }
    }
    const int i = static_cast<int>((8 - nBits) & 7);
    c >>= i;
    lc -= i;
    while (lc > 0)
    {
        const HufDec& pl = dec[static_cast<size_t>((c << (kHufDecBits - lc)) & kHufDecMask)];
        if (!pl.len) return false;
        lc -= pl.len;
        if (!getCode(pl.lit)) return false;
    }
    return out == oe;
}

inline void Wdec14(uint16_t l, uint16_t h, uint16_t& a, uint16_t& b)
{
    const int ls = static_cast<int16_t>(l);
    const int hs = static_cast<int16_t>(h);
    const int ai = ls + (hs & 1) + (hs >> 1);
    a = static_cast<uint16_t>(ai);
    b = static_cast<uint16_t>(ai - hs);
}

inline void Wdec16(uint16_t l, uint16_t h, uint16_t& a, uint16_t& b)
{
    const int m = l, d = h;
    const int bb = (m - (d >> 1)) & 0xffff;
    const int aa = (d + bb - (1 << 15)) & 0xffff;
    b = static_cast<uint16_t>(bb);
    a = static_cast<uint16_t>(aa);
}

void Wav2Decode(uint16_t* in, int nx, int ox, int ny, int oy, uint16_t mx)
{
    const bool w14 = (mx < (1 << 14));
    const int n = (nx > ny) ? ny : nx;
    int p = 1;
    while (p <= n) p <<= 1;
    p >>= 1;
    int p2 = p;
    p >>= 1;
    while (p >= 1)
    {
        uint16_t* py = in;
        uint16_t* ey = in + static_cast<ptrdiff_t>(oy) * (ny - p2);
        const ptrdiff_t oy1 = static_cast<ptrdiff_t>(oy) * p;
        const ptrdiff_t oy2 = static_cast<ptrdiff_t>(oy) * p2;
        const ptrdiff_t ox1 = static_cast<ptrdiff_t>(ox) * p;
        const ptrdiff_t ox2 = static_cast<ptrdiff_t>(ox) * p2;
        uint16_t i00, i01, i10, i11;
        auto dec = [&](uint16_t l, uint16_t h, uint16_t& a, uint16_t& b) {
            if (w14) Wdec14(l, h, a, b); else Wdec16(l, h, a, b);
        };
        for (; py <= ey; py += oy2)
        {
            uint16_t* px = py;
            uint16_t* ex = py + static_cast<ptrdiff_t>(ox) * (nx - p2);
            for (; px <= ex; px += ox2)
            {
                uint16_t* p01 = px + ox1;
                uint16_t* p10 = px + oy1;
                uint16_t* p11 = p10 + ox1;
                dec(*px, *p10, i00, i10);
                dec(*p01, *p11, i01, i11);
                dec(i00, i01, *px, *p01);
                dec(i10, i11, *p10, *p11);
            }
            if (nx & p)
            {
                uint16_t* p10 = px + oy1;
                dec(*px, *p10, i00, *p10);
                *px = i00;
            }
        }
        if (ny & p)
        {
            uint16_t* px = py;
            uint16_t* ex = py + static_cast<ptrdiff_t>(ox) * (nx - p2);
            for (; px <= ex; px += ox2)
            {
                uint16_t* p01 = px + ox1;
                dec(*px, *p01, i00, *p01);
                *px = i00;
            }
        }
        p2 = p;
        p >>= 1;
    }
}

// ───────────────────────────────────────────────────────────────────────────
//  OpenEXR リーダ本体
// ───────────────────────────────────────────────────────────────────────────
struct ExrChannel
{
    std::string name;
    int type = 1;   // 0 UINT / 1 HALF / 2 FLOAT
    int xs = 1, ys = 1;
    int bytes() const { return type == 1 ? 2 : 4; }
};

struct Reader
{
    const uint8_t* d;
    size_t         n;
    size_t         pos = 0;
    bool           bad = false;

    bool Have(size_t k) const { return pos + k <= n; }
    uint8_t U8() { if (!Have(1)) { bad = true; return 0; } return d[pos++]; }
    int32_t I32()
    {
        if (!Have(4)) { bad = true; return 0; }
        const uint32_t v = static_cast<uint32_t>(d[pos]) | (static_cast<uint32_t>(d[pos + 1]) << 8) |
                           (static_cast<uint32_t>(d[pos + 2]) << 16) | (static_cast<uint32_t>(d[pos + 3]) << 24);
        pos += 4;
        return static_cast<int32_t>(v);
    }
    std::string CStr()
    {
        std::string s;
        while (true)
        {
            if (!Have(1)) { bad = true; return s; }
            const char c = static_cast<char>(d[pos++]);
            if (c == 0) break;
            s.push_back(c);
            if (s.size() > 255) { bad = true; break; }
        }
        return s;
    }
};

// ZIP / RLE の後処理(OpenEXR の predictor + 偶奇バイトの並べ替えを戻す)
void ExrUnpredict(std::vector<uint8_t>& buf, std::vector<uint8_t>& tmp)
{
    const size_t n = buf.size();
    for (size_t i = 1; i < n; ++i)
        buf[i] = static_cast<uint8_t>(buf[i - 1] + buf[i] - 128);
    tmp.resize(n);
    const size_t half = (n + 1) / 2;
    size_t t1 = 0, t2 = half, o = 0;
    while (o < n)
    {
        tmp[o++] = buf[t1++];
        if (o >= n) break;
        tmp[o++] = buf[t2++];
    }
    buf.swap(tmp);
}

bool ExrRleDecode(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap)
{
    size_t ip = 0, op = 0;
    while (ip < inLen)
    {
        const int8_t c = static_cast<int8_t>(in[ip++]);
        if (c < 0)
        {
            const size_t cnt = static_cast<size_t>(-c);
            if (ip + cnt > inLen || op + cnt > outCap) return false;
            std::memcpy(out + op, in + ip, cnt);
            ip += cnt;
            op += cnt;
        }
        else
        {
            const size_t cnt = static_cast<size_t>(c) + 1;
            if (ip >= inLen || op + cnt > outCap) return false;
            std::memset(out + op, in[ip++], cnt);
            op += cnt;
        }
    }
    return op == outCap;
}

bool ExrPizDecode(const uint8_t* in, size_t inLen, uint8_t* out, size_t outBytes,
                  const std::vector<ExrChannel>& ch, int width, int lines)
{
    if (inLen < 4) return false;
    size_t pos = 0;
    auto u16 = [&]() { const uint16_t v = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8)); pos += 2; return v; };
    const uint16_t minNZ = u16();
    const uint16_t maxNZ = u16();
    if (maxNZ >= 8192) return false;
    std::vector<uint8_t> bitmap(8192, 0);
    if (minNZ <= maxNZ)
    {
        const size_t len = static_cast<size_t>(maxNZ - minNZ) + 1;
        if (pos + len > inLen) return false;
        std::memcpy(bitmap.data() + minNZ, in + pos, len);
        pos += len;
    }
    std::vector<uint16_t> lut(65536, 0);
    int k = 0;
    for (int i = 0; i < 65536; ++i)
        if (i == 0 || (bitmap[i >> 3] & (1 << (i & 7)))) lut[k++] = static_cast<uint16_t>(i);
    const uint16_t maxValue = static_cast<uint16_t>(k - 1);

    if (pos + 4 > inLen) return false;
    const int32_t hufLen = static_cast<int32_t>(in[pos] | (in[pos + 1] << 8) | (in[pos + 2] << 16) | (in[pos + 3] << 24));
    pos += 4;
    if (hufLen < 0 || pos + static_cast<size_t>(hufLen) > inLen) return false;

    const size_t nWords = outBytes / 2;
    std::vector<uint16_t> tmp(nWords);
    if (!HufUncompress(in + pos, static_cast<size_t>(hufLen), tmp.data(), nWords)) return false;

    struct Cd { uint16_t* start; uint16_t* end; int nx, ny, size; };
    std::vector<Cd> cds(ch.size());
    uint16_t* tEnd = tmp.data();
    for (size_t i = 0; i < ch.size(); ++i)
    {
        cds[i].start = tEnd;
        cds[i].end = tEnd;
        cds[i].nx = width;
        cds[i].ny = lines;
        cds[i].size = ch[i].bytes() / 2;
        tEnd += static_cast<size_t>(cds[i].nx) * cds[i].ny * cds[i].size;
    }
    if (tEnd != tmp.data() + nWords) return false;
    for (auto& cd : cds)
        for (int j = 0; j < cd.size; ++j)
            Wav2Decode(cd.start + j, cd.nx, cd.size, cd.ny, cd.nx * cd.size, maxValue);
    for (auto& v : tmp) v = lut[v];

    uint8_t* op = out;
    for (int y = 0; y < lines; ++y)
        for (auto& cd : cds)
        {
            const size_t nbytes = static_cast<size_t>(cd.nx) * cd.size * 2;
            std::memcpy(op, cd.end, nbytes);
            cd.end += static_cast<size_t>(cd.nx) * cd.size;
            op += nbytes;
        }
    return true;
}

} // namespace

bool IsEquirectExtension(const std::string& e)
{
    return e == ".hdr" || e == ".exr";
}

bool DecodeExr(const uint8_t* data, size_t size, DirectX::ScratchImage& out, std::string& err)
{
    using namespace DirectX;
    Reader r{data, size};
    if (size < 8 || data[0] != 0x76 || data[1] != 0x2f || data[2] != 0x31 || data[3] != 0x01)
    {
        err = "OpenEXR ファイルではありません";
        return false;
    }
    r.pos = 4;
    const int32_t ver = r.I32();
    if (ver & 0x200)  { err = "タイル形式の EXR には対応していません(スキャンライン形式で保存してください)"; return false; }
    if (ver & 0x800)  { err = "deep 形式の EXR には対応していません"; return false; }
    if (ver & 0x1000) { err = "マルチパートの EXR には対応していません"; return false; }

    std::vector<ExrChannel> ch;
    int compression = -1;
    int xmin = 0, ymin = 0, xmax = -1, ymax = -1;
    bool haveDw = false;
    while (!r.bad)
    {
        const std::string name = r.CStr();
        if (name.empty()) break;
        const std::string type = r.CStr();
        const int32_t sz = r.I32();
        if (r.bad || sz < 0 || !r.Have(static_cast<size_t>(sz))) { err = "EXR のヘッダが壊れています"; return false; }
        const size_t attrEnd = r.pos + static_cast<size_t>(sz);
        if (name == "channels")
        {
            while (r.pos < attrEnd && !r.bad)
            {
                ExrChannel c;
                c.name = r.CStr();
                if (c.name.empty()) break;
                c.type = r.I32();
                r.pos += 4;   // pLinear + reserved
                c.xs = r.I32();
                c.ys = r.I32();
                ch.push_back(c);
            }
        }
        else if (name == "compression")
            compression = r.U8();
        else if (name == "dataWindow")
        {
            xmin = r.I32(); ymin = r.I32(); xmax = r.I32(); ymax = r.I32();
            haveDw = true;
        }
        r.pos = attrEnd;
    }
    if (r.bad || ch.empty() || compression < 0 || !haveDw) { err = "EXR のヘッダが壊れています"; return false; }
    const int width = xmax - xmin + 1, height = ymax - ymin + 1;
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768) { err = "EXR の画像サイズが不正です"; return false; }
    for (const auto& c : ch)
    {
        if (c.type < 0 || c.type > 2) { err = "EXR のチャンネル形式が不正です"; return false; }
        if (c.xs != 1 || c.ys != 1) { err = "サブサンプリングされた EXR には対応していません"; return false; }
    }

    int linesPerBlock = 1;
    switch (compression)
    {
    case 0: case 1: case 2: linesPerBlock = 1; break;
    case 3: linesPerBlock = 16; break;
    case 4: linesPerBlock = 32; break;
    default:
        err = "この圧縮方式の EXR には対応していません(PXR24 / B44 / DWA)。ZIP か PIZ か無圧縮で保存してください";
        return false;
    }

    // チャンネルの対応(R G B A、無ければ Y)
    int idxR = -1, idxG = -1, idxB = -1, idxA = -1, idxY = -1;
    for (size_t i = 0; i < ch.size(); ++i)
    {
        const std::string& nm = ch[i].name;
        const std::string tail = nm.substr(nm.find_last_of('.') == std::string::npos ? 0 : nm.find_last_of('.') + 1);
        if (tail == "R" || tail == "r") idxR = static_cast<int>(i);
        else if (tail == "G" || tail == "g") idxG = static_cast<int>(i);
        else if (tail == "B" || tail == "b") idxB = static_cast<int>(i);
        else if (tail == "A" || tail == "a") idxA = static_cast<int>(i);
        else if (tail == "Y" || tail == "y") idxY = static_cast<int>(i);
    }
    if (idxR < 0 || idxG < 0 || idxB < 0)
    {
        if (idxY < 0) { err = "EXR に R/G/B チャンネルがありません"; return false; }
        idxR = idxG = idxB = idxY;
    }

    size_t bytesPerLine = 0;
    std::vector<size_t> chOffset(ch.size());
    for (size_t i = 0; i < ch.size(); ++i)
    {
        chOffset[i] = bytesPerLine;
        bytesPerLine += static_cast<size_t>(width) * ch[i].bytes();
    }

    const int nBlocks = (height + linesPerBlock - 1) / linesPerBlock;
    if (!r.Have(static_cast<size_t>(nBlocks) * 8)) { err = "EXR のオフセット表が壊れています"; return false; }
    std::vector<uint64_t> offsets(static_cast<size_t>(nBlocks));
    for (auto& o : offsets)
    {
        uint64_t v = 0;
        for (int b = 0; b < 8; ++b) v |= static_cast<uint64_t>(data[r.pos + b]) << (8 * b);
        r.pos += 8;
        o = v;
    }

    if (FAILED(out.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, static_cast<size_t>(width),
                                static_cast<size_t>(height), 1, 1)))
    {
        err = "画像メモリを確保できません";
        return false;
    }
    const Image* img = out.GetImage(0, 0, 0);

    std::vector<uint8_t> block, tmp;
    for (int bi = 0; bi < nBlocks; ++bi)
    {
        const uint64_t off = offsets[static_cast<size_t>(bi)];
        if (off + 8 > size) { err = "EXR のデータが途中で切れています"; return false; }
        Reader br{data, size, static_cast<size_t>(off)};
        const int y0 = br.I32();
        const int dsize = br.I32();
        if (br.bad || dsize < 0 || !br.Have(static_cast<size_t>(dsize))) { err = "EXR のデータが途中で切れています"; return false; }
        const int rowStart = y0 - ymin;
        if (rowStart < 0 || rowStart >= height) { err = "EXR のブロック位置が不正です"; return false; }
        const int lines = std::min(linesPerBlock, height - rowStart);
        const size_t rawSize = bytesPerLine * static_cast<size_t>(lines);
        const uint8_t* src = data + br.pos;

        block.resize(rawSize);
        if (compression == 0 || static_cast<size_t>(dsize) == rawSize)
        {
            if (static_cast<size_t>(dsize) != rawSize) { err = "EXR のデータサイズが不正です"; return false; }
            std::memcpy(block.data(), src, rawSize);
        }
        else if (compression == 2 || compression == 3)
        {
            if (!ZlibInflate(src, static_cast<size_t>(dsize), block.data(), rawSize)) { err = "EXR(ZIP)の展開に失敗しました"; return false; }
            ExrUnpredict(block, tmp);
        }
        else if (compression == 1)
        {
            if (!ExrRleDecode(src, static_cast<size_t>(dsize), block.data(), rawSize)) { err = "EXR(RLE)の展開に失敗しました"; return false; }
            ExrUnpredict(block, tmp);
        }
        else if (compression == 4)
        {
            if (!ExrPizDecode(src, static_cast<size_t>(dsize), block.data(), rawSize, ch, width, lines))
            {
                err = "EXR(PIZ)の展開に失敗しました";
                return false;
            }
        }

        for (int ly = 0; ly < lines; ++ly)
        {
            float* dst = reinterpret_cast<float*>(img->pixels + static_cast<size_t>(rowStart + ly) * img->rowPitch);
            const uint8_t* line = block.data() + static_cast<size_t>(ly) * bytesPerLine;
            auto fetch = [&](int ci, int x) -> float {
                const ExrChannel& c = ch[static_cast<size_t>(ci)];
                const uint8_t* p = line + chOffset[static_cast<size_t>(ci)] + static_cast<size_t>(x) * c.bytes();
                if (c.type == 1)
                {
                    const uint16_t h = static_cast<uint16_t>(p[0] | (p[1] << 8));
                    return DirectX::PackedVector::XMConvertHalfToFloat(h);
                }
                if (c.type == 2)
                {
                    uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                 (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
                    float f;
                    std::memcpy(&f, &v, 4);
                    return f;
                }
                uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                             (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
                return static_cast<float>(v);
            };
            for (int x = 0; x < width; ++x)
            {
                dst[x * 4 + 0] = fetch(idxR, x);
                dst[x * 4 + 1] = fetch(idxG, x);
                dst[x * 4 + 2] = fetch(idxB, x);
                dst[x * 4 + 3] = idxA >= 0 ? fetch(idxA, x) : 1.0f;
            }
        }
    }
    return true;
}

bool DecodeEquirect(const uint8_t* data, size_t size, const std::string& ext,
                    DirectX::ScratchImage& out, std::string& err)
{
    using namespace DirectX;
    if (ext == ".exr")
        return DecodeExr(data, size, out, err);
    if (ext == ".hdr")
    {
        ScratchImage tmp;
        if (FAILED(LoadFromHDRMemory(data, size, nullptr, tmp)))
        {
            err = "HDR(Radiance)のデコードに失敗しました";
            return false;
        }
        if (tmp.GetMetadata().format == DXGI_FORMAT_R32G32B32A32_FLOAT)
        {
            out = std::move(tmp);
            return true;
        }
        if (FAILED(Convert(tmp.GetImages(), tmp.GetImageCount(), tmp.GetMetadata(),
                           DXGI_FORMAT_R32G32B32A32_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, out)))
        {
            err = "HDR の形式変換に失敗しました";
            return false;
        }
        return true;
    }
    err = "対応していない拡張子です(.hdr / .exr)";
    return false;
}

uint32_t ChooseFaceSize(uint32_t srcWidth, uint32_t maxFace)
{
    uint32_t target = std::max<uint32_t>(srcWidth / 4, 1);
    uint32_t p = 1;
    while ((p << 1) <= target) p <<= 1;   // 2 のべきへ切り捨て
    return std::clamp<uint32_t>(p, 256, std::max<uint32_t>(maxFace, 256));
}

bool EquirectToCube(const DirectX::ScratchImage& equirect, uint32_t faceSize,
                    DirectX::ScratchImage& outCube, std::string& err)
{
    using namespace DirectX;
    const TexMetadata& sm = equirect.GetMetadata();
    if (sm.format != DXGI_FORMAT_R32G32B32A32_FLOAT || sm.width < 8 || sm.height < 4 || faceSize < 4)
    {
        err = "equirect 画像が不正です";
        return false;
    }

    // 元が面の解像度より十分大きいときは先に縮める(エイリアシング防止)。
    const ScratchImage* srcPtr = &equirect;
    ScratchImage reduced;
    if (sm.width > static_cast<size_t>(faceSize) * 4)
    {
        if (SUCCEEDED(Resize(equirect.GetImages(), equirect.GetImageCount(), sm,
                             static_cast<size_t>(faceSize) * 4, static_cast<size_t>(faceSize) * 2,
                             TEX_FILTER_BOX, reduced)) &&
            reduced.GetMetadata().format == DXGI_FORMAT_R32G32B32A32_FLOAT)
            srcPtr = &reduced;
    }
    const Image* src = srcPtr->GetImage(0, 0, 0);
    const int sw = static_cast<int>(src->width), sh = static_cast<int>(src->height);

    ScratchImage cube;
    if (FAILED(cube.InitializeCube(DXGI_FORMAT_R16G16B16A16_FLOAT, faceSize, faceSize, 1, 1)))
    {
        err = "キューブのメモリを確保できません";
        return false;
    }

    const float kPi = 3.14159265358979f;
    auto fetch = [&](int x, int y, float* o) {
        x %= sw; if (x < 0) x += sw;
        y = std::clamp(y, 0, sh - 1);
        const float* p = reinterpret_cast<const float*>(src->pixels + static_cast<size_t>(y) * src->rowPitch) + x * 4;
        for (int i = 0; i < 4; ++i) o[i] = std::isfinite(p[i]) ? std::max(p[i], 0.0f) : 0.0f;
    };

    const float N = static_cast<float>(faceSize);
    for (uint32_t f = 0; f < 6; ++f)
    {
        const Image* fi = cube.GetImage(0, f, 0);
        for (uint32_t py = 0; py < faceSize; ++py)
        {
            auto* dst = reinterpret_cast<PackedVector::HALF*>(fi->pixels + static_cast<size_t>(py) * fi->rowPitch);
            const float v = 1.0f - 2.0f * (static_cast<float>(py) + 0.5f) / N;
            for (uint32_t px = 0; px < faceSize; ++px)
            {
                const float u = 2.0f * (static_cast<float>(px) + 0.5f) / N - 1.0f;
                float d[3];
                switch (f)   // D3D のキューブ面順: +X -X +Y -Y +Z -Z(手続きスカイと同じ規約)
                {
                case 0:  d[0] =  1; d[1] =  v; d[2] = -u; break;
                case 1:  d[0] = -1; d[1] =  v; d[2] =  u; break;
                case 2:  d[0] =  u; d[1] =  1; d[2] = -v; break;
                case 3:  d[0] =  u; d[1] = -1; d[2] =  v; break;
                case 4:  d[0] =  u; d[1] =  v; d[2] =  1; break;
                default: d[0] = -u; d[1] =  v; d[2] = -1; break;
                }
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                const float dx = d[0] / len, dy = d[1] / len, dz = d[2] / len;
                const float uu = 0.5f + std::atan2(dx, dz) / (2.0f * kPi);
                const float vv = 0.5f - std::asin(std::clamp(dy, -1.0f, 1.0f)) / kPi;
                const float fx = uu * sw - 0.5f, fy = vv * sh - 0.5f;
                const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
                const float tx = fx - x0, ty = fy - y0;
                float a[4], b[4], c[4], e[4];
                fetch(x0, y0, a); fetch(x0 + 1, y0, b); fetch(x0, y0 + 1, c); fetch(x0 + 1, y0 + 1, e);
                for (int k = 0; k < 4; ++k)
                {
                    const float top = a[k] + (b[k] - a[k]) * tx;
                    const float bot = c[k] + (e[k] - c[k]) * tx;
                    float val = top + (bot - top) * ty;
                    val = std::min(val, kMaxHalf);   // fp16 の上限(太陽が inf にならない)
                    if (k == 3) val = 1.0f;
                    dst[px * 4 + k] = PackedVector::XMConvertFloatToHalf(val);
                }
            }
        }
    }

    // ミップ(IBL のプリフィルタが元環境のミップを引く)
    ScratchImage mipped;
    if (FAILED(GenerateMipMaps(cube.GetImages(), cube.GetImageCount(), cube.GetMetadata(),
                               TEX_FILTER_BOX | TEX_FILTER_WRAP, 0, mipped)))
    {
        err = "ミップの生成に失敗しました";
        return false;
    }
    outCube = std::move(mipped);
    return true;
}

} // namespace dx12e::envconv
