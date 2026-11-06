#include "../hls/quant_engine.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
struct Args {
    uint32_t words, op, in, in2, out, weights, bias, mult, shift;
    uint32_t c, h, w, co, k, s, p, flags;
    Args() : words(8192), op(QE_CONV), in(16), in2(2048), out(4096),
        weights(1024), bias(3000), mult(3100), shift(3200),
        c(2), h(4), w(5), co(3), k(3), s(1), p(1), flags(QE_BIAS) {}
};
uint32_t run(std::vector<uint32_t>& m, const Args& a) {
    return quant_engine(&m[0], a.words, a.op, a.in, a.in2, a.out, a.weights,
        a.bias, a.mult, a.shift, a.c, a.h, a.w, a.co, a.k, a.s, a.p, a.flags);
}
void require(bool b, const char* message) {
    if (!b) throw std::runtime_error(message);
}
int64_t signed_word(uint32_t v) {
    int32_t result;
    std::memcpy(&result, &v, sizeof(v));
    return result;
}
int signed_byte(uint32_t v) {
    int8_t result;
    uint8_t low = uint8_t(v & 255);
    std::memcpy(&result, &low, sizeof(low));
    return result;
}
uint32_t limit32(int64_t n) {
    n = std::min<int64_t>(std::numeric_limits<int32_t>::max(),
        std::max<int64_t>(std::numeric_limits<int32_t>::min(), n));
    return uint32_t(n);
}
// Independent host reference: division/remainder rounding, signed types only
// after memcpy decoding, and a flattened dense/depthwise weight iterator.
std::vector<uint32_t> reference(const std::vector<uint32_t>& m, const Args& a) {
    std::vector<uint32_t> output;
    if (a.op == QE_CONV) {
        int oh = (int(a.h) + 2 * int(a.p) - int(a.k)) / int(a.s) + 1;
        int ow = (int(a.w) + 2 * int(a.p) - int(a.k)) / int(a.s) + 1;
        int wc = (a.flags & QE_DEPTHWISE) ? 1 : int(a.c);
        for (int co = 0; co < int(a.co); ++co)
            for (int oy = 0; oy < oh; ++oy)
                for (int ox = 0; ox < ow; ++ox) {
                    int64_t sum = (a.flags & QE_BIAS) ? signed_word(m[a.bias + co]) : 0;
                    for (int wi = 0; wi < wc * int(a.k * a.k); ++wi) {
                        int ci = (a.flags & QE_DEPTHWISE) ? co : wi / int(a.k * a.k);
                        int ky = (wi / int(a.k)) % int(a.k), kx = wi % int(a.k);
                        int iy = oy * int(a.s) - int(a.p) + ky;
                        int ix = ox * int(a.s) - int(a.p) + kx;
                        if (iy >= 0 && iy < int(a.h) && ix >= 0 && ix < int(a.w))
                            sum += int64_t(signed_byte(m[a.in + (ci * a.h + iy) * a.w + ix])) *
                                   signed_byte(m[a.weights + co * wc * a.k * a.k + wi]);
                    }
                    output.push_back(limit32(sum));
                }
    } else if (a.op == QE_MAXPOOL) {
        for (uint32_t c = 0; c < a.c; ++c)
            for (uint32_t y = 0; y < a.h / 2; ++y)
                for (uint32_t x = 0; x < a.w / 2; ++x) {
                    uint32_t base = a.in + (c * a.h + 2 * y) * a.w + 2 * x;
                    int best = std::max(std::max(signed_byte(m[base]), signed_byte(m[base + 1])),
                        std::max(signed_byte(m[base + a.w]), signed_byte(m[base + a.w + 1])));
                    output.push_back(uint32_t(best) & 255U);
                }
    } else if (a.op == QE_REQUANT) {
        for (uint32_t i = 0; i < a.c * a.h * a.w; ++i) {
            uint32_t c = i / (a.h * a.w);
            int64_t product = signed_word(m[a.in + i]) * int64_t(m[a.mult + c]);
            int64_t divisor = int64_t(1) << m[a.shift + c];
            int64_t rounded = product / divisor;
            int64_t remainder = product % divisor;
            if (remainder != 0 && std::abs(remainder) * 2 >= divisor)
                rounded += product < 0 ? -1 : 1;
            rounded = std::max<int64_t>(-128, std::min<int64_t>(127, rounded));
            if ((a.flags & QE_RELU) && rounded < 0) rounded = 0;
            output.push_back(uint32_t(rounded) & 255U);
        }
    } else {
        for (uint32_t i = 0; i < a.c * a.h * a.w; ++i)
            output.push_back(limit32(signed_word(m[a.in + i]) + signed_word(m[a.in2 + i])));
    }
    return output;
}
void check_valid(std::vector<uint32_t>& m, const Args& a) {
    std::vector<uint32_t> before = m, expected = reference(m, a);
    require(run(m, a) == QE_OK, "valid request failed");
    for (size_t i = 0; i < m.size(); ++i) {
        uint32_t wanted = i >= a.out && i - a.out < expected.size() ? expected[i - a.out] : before[i];
        require(m[i] == wanted, "reference mismatch or write beyond output");
    }
    // Stateful or stale accumulator errors are detected by the same transaction again.
    require(run(m, a) == QE_OK, "repeated request failed");
    for (size_t i = 0; i < expected.size(); ++i)
        require(m[a.out + i] == expected[i], "repeated request changed output");
}
void check_error(std::vector<uint32_t>& m, const Args& a, uint32_t code) {
    std::vector<uint32_t> before = m;
    require(run(m, a) == code, "wrong error status");
    require(m == before, "rejected request changed memory");
}
Args simple(uint32_t op) {
    Args a;
    a.op = op; a.co = a.k = a.s = a.p = a.flags = 0;
    return a;
}
void random_tests() {
    std::mt19937 random(0x7832a1);
    for (int trial = 0; trial < 80; ++trial) {
        Args a;
        a.c = 1 + random() % 5; a.co = 1 + random() % 5;
        a.h = 3 + random() % 5; a.w = 3 + random() % 6;
        a.k = (random() & 1) ? 1 : 3; a.s = 1 + random() % 2; a.p = random() % 2;
        a.flags = (random() & 1) ? QE_BIAS : 0;
        if (random() & 1) { a.flags |= QE_DEPTHWISE; a.co = a.c; }
        std::vector<uint32_t> m(a.words);
        for (size_t i = 0; i < m.size(); ++i) m[i] = random();
        check_valid(m, a);
    }
    for (uint32_t op = QE_MAXPOOL; op <= QE_ADD_SAT; ++op) {
        for (int trial = 0; trial < 25; ++trial) {
            Args a = simple(op);
            a.c = 1 + random() % 5; a.h = 2 + random() % 7; a.w = 2 + random() % 7;
            a.flags = op == QE_REQUANT && (random() & 1) ? QE_RELU : 0;
            std::vector<uint32_t> m(a.words);
            for (size_t i = 0; i < m.size(); ++i) m[i] = random();
            for (uint32_t c = 0; c < a.c; ++c) {
                m[a.mult + c] = random() & 0x7fffffffU;
                m[a.shift + c] = random() % 32;
            }
            check_valid(m, a);
        }
    }
}
void edge_tests() {
    Args a = simple(QE_REQUANT);
    a.c = 4; a.h = 1; a.w = 8;
    std::vector<uint32_t> m(a.words, 0xd7a581c3U);
    const int32_t data[8] = {1, -1, 3, -3, 255, -257, 2147483647, (-2147483647 - 1)};
    for (uint32_t c = 0; c < a.c; ++c) {
        m[a.mult + c] = c == 2 ? 0 : c == 3 ? 2147483647U : 1;
        m[a.shift + c] = c == 0 ? 1 : c == 1 ? 0 : 31;
        for (int i = 0; i < 8; ++i) m[a.in + c * 8 + i] = uint32_t(data[i]);
    }
    check_valid(m, a);
    require(m[a.out] == 1 && m[a.out + 1] == 255 && m[a.out + 2] == 2 && m[a.out + 3] == 254,
        "ties must round away from zero");
    a.flags = QE_RELU; check_valid(m, a);

    a = simple(QE_ADD_SAT); a.c = 1; a.h = 1; a.w = 4;
    m[a.in] = 0x7fffffff; m[a.in2] = 1;
    m[a.in + 1] = 0x80000000U; m[a.in2 + 1] = uint32_t(-1);
    m[a.in + 2] = 0x80000000U; m[a.in2 + 2] = 0x7fffffff;
    m[a.in + 3] = 0x7fffffff; m[a.in2 + 3] = 0x7fffffff;
    check_valid(m, a);

    // Bias+MAC must accumulate in INT64 and clip only once: positive first
    // contribution crosses INT32_MAX, then a negative tap cancels it.
    a = Args(); a.c = 2; a.h = a.w = a.k = a.s = a.co = 1; a.p = 0;
    m[a.in] = 127; m[a.in + 1] = 127;
    m[a.weights] = 127; m[a.weights + 1] = uint32_t(-127);
    m[a.bias] = 0x7fffffffU;
    check_valid(m, a); require(m[a.out] == 0x7fffffffU, "intermediate clipping detected");
    m[a.weights + 1] = 127; check_valid(m, a);
    m[a.bias] = 0x80000000U; m[a.in] = uint32_t(-128); m[a.in + 1] = uint32_t(-128);
    check_valid(m, a);

    // Limit dimensions, channel limit, negative bytes and ignored high bits.
    a = Args(); a.c = a.co = 64; a.h = a.w = a.k = a.s = 1; a.p = 0;
    a.flags = QE_DEPTHWISE;
    for (int c = 0; c < 64; ++c) { m[a.in + c] = 0x98765480U; m[a.weights + c] = 0xff007fU; }
    check_valid(m, a);
    a = Args(); a.c = a.co = a.k = a.s = 1; a.p = a.flags = 0; a.h = a.w = 128;
    a.words = 40000; a.weights = 17000; a.out = 18000;
    m.assign(a.words, 0x123456ffU); check_valid(m, a);
    // Last output word exactly ends at declared memory limit.
    a = simple(QE_MAXPOOL); a.c = 1; a.h = a.w = 2; a.out = a.words - 1;
    m.assign(a.words, 0xffffffffU); check_valid(m, a);
}
void rejection_tests() {
    std::vector<uint32_t> m(8192, 0x5a5a5a5a);
    Args a, b;
    b = a; b.op = 0; check_error(m, b, QE_ERR_OPCODE);
    b.op = 0xffffffffU; check_error(m, b, QE_ERR_OPCODE);
    b = a; b.words = 0; check_error(m, b, QE_ERR_MEMORY);
    b.words = (1U << 24) + 1; check_error(m, b, QE_ERR_MEMORY);
    require(quant_engine(0, 8192, QE_CONV, 0,0,0,0,0,0,0,1,1,1,1,1,1,0,0) == QE_ERR_MEMORY,
        "null pointer rejected");
    for (int which = 0; which < 3; ++which) {
        b = a; (which == 0 ? b.c : which == 1 ? b.h : b.w) = 0;
        check_error(m, b, QE_ERR_DIMENSION);
        b = a; (which == 0 ? b.c : which == 1 ? b.h : b.w) = 0xffffffffU;
        check_error(m, b, QE_ERR_DIMENSION);
    }
    b = a; b.co = 65; check_error(m,b,QE_ERR_DIMENSION);
    b = a; b.k = 2; check_error(m,b,QE_ERR_PARAMETER);
    b = a; b.s = 0; check_error(m,b,QE_ERR_PARAMETER);
    b = a; b.p = 2; check_error(m,b,QE_ERR_PARAMETER);
    b = a; b.flags |= QE_RELU; check_error(m,b,QE_ERR_PARAMETER);
    b = a; b.flags |= QE_DEPTHWISE; check_error(m,b,QE_ERR_DIMENSION);
    b = a; b.h = 1; b.p = 0; check_error(m,b,QE_ERR_DIMENSION);
    b = a; b.h = 128; b.k = 1; check_error(m,b,QE_ERR_DIMENSION);
    b = a; b.in = 0xffffffffU; check_error(m,b,QE_ERR_RANGE);
    b = a; b.out = 0xfffffff0U; check_error(m,b,QE_ERR_RANGE);
    b = a; b.weights = a.words - 1; check_error(m,b,QE_ERR_RANGE);
    b = a; b.bias = a.words - 1; check_error(m,b,QE_ERR_RANGE);
    b = a; b.out = a.in + 1; check_error(m,b,QE_ERR_OVERLAP);
    b = a; b.out = a.weights; check_error(m,b,QE_ERR_OVERLAP);
    b = a; b.out = a.bias; check_error(m,b,QE_ERR_OVERLAP);
    for (uint32_t op = QE_MAXPOOL; op <= QE_ADD_SAT; ++op) {
        a = simple(op);
        b = a; b.co = 1; check_error(m,b,QE_ERR_PARAMETER);
        b = a; b.k = 1; check_error(m,b,QE_ERR_PARAMETER);
        b = a; b.s = 1; check_error(m,b,QE_ERR_PARAMETER);
        b = a; b.p = 1; check_error(m,b,QE_ERR_PARAMETER);
        b = a; b.flags = QE_BIAS; check_error(m,b,QE_ERR_PARAMETER);
    }
    a = simple(QE_MAXPOOL); a.h = 1; check_error(m,a,QE_ERR_DIMENSION);
    a = simple(QE_ADD_SAT); b = a; b.in2 = a.words - 1; check_error(m,b,QE_ERR_RANGE);
    b = a; b.in2 = a.out; check_error(m,b,QE_ERR_OVERLAP);
    a = simple(QE_REQUANT);
    b = a; b.mult = a.words - 1; check_error(m,b,QE_ERR_RANGE);
    b = a; b.shift = a.words - 1; check_error(m,b,QE_ERR_RANGE);
    b = a; b.out = a.mult; check_error(m,b,QE_ERR_OVERLAP);
    b = a; b.out = a.shift; check_error(m,b,QE_ERR_OVERLAP);
    for (uint32_t c = 0; c < a.c; ++c) { m[a.mult+c] = 1; m[a.shift+c] = 0; }
    m[a.mult + a.c - 1] = 0x80000000U; check_error(m,a,QE_ERR_SCALE);
    m[a.mult + a.c - 1] = 1; m[a.shift + a.c - 1] = 32; check_error(m,a,QE_ERR_SCALE);
    m[a.shift + a.c - 1] = 0xffffffffU; check_error(m,a,QE_ERR_SCALE);
    // Read-read overlap is legal and does not create hidden state.
    a = simple(QE_ADD_SAT); a.in2 = a.in; check_valid(m,a);
}
}
int main() {
    try {
        random_tests(); edge_tests(); rejection_tests();
        std::cout << "quant_engine: all independent reference, boundary and no-write rejection tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "quant_engine test FAILED: " << e.what() << "\n";
        return 1;
    }
}
