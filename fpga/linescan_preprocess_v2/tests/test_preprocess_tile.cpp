#include "../hls/preprocess_tile.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Job {
    uint32_t memory_words, input_offset, stride_bytes, width, height;
    uint32_t x, y, w, h, radius, calibration, output, polarity, threshold, flags;
};

uint32_t invoke(std::vector<uint32_t>& m, const Job& j) {
    return preprocess_tile(m.data(), j.memory_words, j.input_offset,
        j.stride_bytes, j.width, j.height, j.x, j.y, j.w, j.h,
        j.radius, j.calibration, j.output, j.polarity, j.threshold, j.flags);
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Independent oracle: materialize the entire corrected image, then enumerate
// each mathematical square neighborhood directly. No tile halo, ring buffer,
// packed-span cache, running sum or history registers are shared with the DUT.
std::vector<uint32_t> reference(const std::vector<uint32_t>& m, const Job& j) {
    std::vector<unsigned> corrected(size_t(j.width) * j.height);
    std::vector<bool> valid(corrected.size(), true);
    for (uint32_t yy = 0; yy < j.height; ++yy) {
        for (uint32_t xx = 0; xx < j.width; ++xx) {
            const size_t index = size_t(yy) * j.width + xx;
            const unsigned raw = (m[j.input_offset + size_t(yy) * (j.stride_bytes / 4) + xx / 4]
                                    >> ((xx % 4) * 8)) & 255;
            if (!(j.flags & 1)) {
                corrected[index] = raw;
                continue;
            }
            const uint32_t calibration = m[j.calibration + xx];
            const uint64_t gain = calibration / 256;
            valid[index] = gain != 0;
            const int delta = std::max(0, int(raw) - int(calibration % 256));
            corrected[index] = gain == 0 ? 0 : unsigned(std::min<uint64_t>(255,
                (uint64_t(delta) * gain + 32768) / 65536));
        }
    }
    std::vector<uint32_t> output;
    for (uint32_t yy = j.y; yy < j.y + j.h; ++yy) {
        for (uint32_t xx = j.x; xx < j.x + j.w; ++xx) {
            uint64_t sum = 0;
            bool neighborhood_valid = true;
            unsigned count = 0;
            for (int dy = -int(j.radius); dy <= int(j.radius); ++dy) {
                for (int dx = -int(j.radius); dx <= int(j.radius); ++dx) {
                    const int nx = std::max(0, std::min(int(j.width) - 1, int(xx) + dx));
                    const int ny = std::max(0, std::min(int(j.height) - 1, int(yy) + dy));
                    const size_t index = size_t(ny) * j.width + nx;
                    sum += corrected[index];
                    neighborhood_valid = neighborhood_valid && valid[index];
                    ++count;
                }
            }
            const unsigned center = corrected[size_t(yy) * j.width + xx];
            const unsigned background = unsigned((sum + count / 2) / count);
            const int difference = neighborhood_valid ? int(center) - int(background) : 0;
            const bool bright = difference >= int(j.threshold);
            const bool dark = -difference >= int(j.threshold);
            const bool pass = neighborhood_valid && (j.polarity == 0 ? bright || dark :
                               j.polarity == 1 ? bright : dark);
            const unsigned signed9 = difference < 0 ? unsigned(512 + difference) : unsigned(difference);
            output.push_back(center + 256U * background + 65536U * signed9 +
                (neighborhood_valid ? (1U << 25) : 0) + (pass ? (1U << 26) : 0));
        }
    }
    return output;
}

unsigned accepted_cases = 0;
unsigned rejected_cases = 0;

void check(std::vector<uint32_t>& memory, const Job& job, const std::string& name) {
    const std::vector<uint32_t> before = memory;
    const std::vector<uint32_t> expected = reference(before, job);
    const uint32_t status = invoke(memory, job);
    require(status == PREPROCESS_OK, name + ": status " + std::to_string(status));
    for (size_t i = 0; i < memory.size(); ++i) {
        const bool is_output = i >= job.output && i < job.output + expected.size();
        const uint32_t value = is_output ? expected[i - job.output] : before[i];
        require(memory[i] == value, name + ": mismatch at word " + std::to_string(i));
        if (is_output) require((memory[i] & 0xf8000000U) == 0, name + ": reserved bits set");
    }
    ++accepted_cases;
}

struct Fixture {
    Job j;
    std::vector<uint32_t> memory;
    Fixture(uint32_t width, uint32_t height, uint32_t pad_words = 0) {
        j = {};
        j.width = width;
        j.height = height;
        j.stride_bytes = ((width + 3) / 4 + pad_words) * 4;
        j.input_offset = 5;
        j.calibration = j.input_offset + j.stride_bytes / 4 * height + 7;
        j.output = j.calibration + width + 11;
        j.w = std::min(width, 256U);
        j.h = std::min(height, 64U);
        j.threshold = 1;
        j.memory_words = j.output + j.w * j.h + 17;
        memory.assign(j.memory_words, 0xa55a3cc3U);
        for (unsigned x = 0; x < width; ++x)
            memory[j.calibration + x] = 65536U << 8;
    }
    void pixel(uint32_t x, uint32_t y, uint32_t value) {
        uint32_t& word = memory[j.input_offset + y * (j.stride_bytes / 4) + x / 4];
        const unsigned shift = x % 4 * 8;
        word = (word & ~(255U << shift)) | (value << shift);
    }
    void fill_random(std::mt19937& rng) {
        for (unsigned y = 0; y < j.height; ++y)
            for (unsigned x = 0; x < j.width; ++x) pixel(x, y, rng() % 256);
    }
};

void reject(std::vector<uint32_t>& memory, const Job& j, unsigned expected,
            const std::string& label) {
    const std::vector<uint32_t> before = memory;
    const unsigned status = invoke(memory, j);
    require(status == expected, label + ": expected status " + std::to_string(expected) +
            ", got " + std::to_string(status));
    require(memory == before, label + ": memory changed on rejection");
    ++rejected_cases;
}

void directed(std::mt19937& rng) {
    Fixture one(1, 1, 2);
    one.pixel(0, 0, 91);
    one.j.radius = 7;
    check(one.memory, one.j, "one pixel replicated 225 times");
    one.j.flags = 1;
    one.memory[one.j.calibration] = 0;
    check(one.memory, one.j, "one invalid pixel");
    one.memory[one.j.calibration] = (32768U << 8) | 90U;
    one.j.radius = 0;
    check(one.memory, one.j, "calibration half rounding");
    require((one.memory[one.j.output] & 255U) == 1, "Q16 half rounds up");
    one.memory[one.j.calibration] = 0xffffff00U;
    check(one.memory, one.j, "maximum gain saturation");
    require((one.memory[one.j.output] & 255U) == 255, "maximum gain saturates");
    one.memory[one.j.calibration] = (65536U << 8) | 255U;
    check(one.memory, one.j, "dark subtraction floors at zero");

    Fixture signs(3, 3);
    for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 3; ++x) signs.pixel(x, y, 0);
    signs.pixel(1, 1, 90);
    signs.j.x = signs.j.y = 1;
    signs.j.w = signs.j.h = 1;
    signs.j.radius = 1;
    signs.j.threshold = 80;
    for (unsigned p = 0; p < 3; ++p) {
        signs.j.polarity = p;
        check(signs.memory, signs.j, "positive threshold inclusive polarity " + std::to_string(p));
        require(bool(signs.memory[signs.j.output] & (1U << 26)) == (p != 2), "bright inclusive");
    }
    for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 3; ++x) signs.pixel(x, y, 90);
    signs.pixel(1, 1, 0);
    for (unsigned p = 0; p < 3; ++p) {
        signs.j.polarity = p;
        check(signs.memory, signs.j, "negative threshold inclusive polarity " + std::to_string(p));
        require(bool(signs.memory[signs.j.output] & (1U << 26)) == (p != 1), "dark inclusive");
    }
    signs.j.flags = 1;
    signs.memory[signs.j.calibration] = 0;
    check(signs.memory, signs.j, "invalid neighboring column suppresses signed and pass");
    require(((signs.memory[signs.j.output] >> 16) & 0x7ffU) == 0, "invalid neighborhood flags");

    Fixture full(400, 100, 3);
    full.fill_random(rng);
    full.j.x = 43; full.j.y = 17; full.j.w = 256; full.j.h = 64;
    full.j.radius = 7;
    check(full.memory, full.j, "maximum 270-pixel padded width unaligned packed words");
    // Repeated invocations change banks, diameter, calibration and tile boundaries.
    const unsigned radii[] = {0, 7, 1, 6, 2, 5, 3, 4, 0, 7};
    for (unsigned n = 0; n < sizeof(radii) / sizeof(radii[0]); ++n) {
        full.j.radius = radii[n];
        full.j.x = n % 2 ? 400 - full.j.w : 0;
        full.j.y = n % 2 ? 100 - full.j.h : 0;
        full.j.flags = n % 2;
        full.j.polarity = n % 3;
        check(full.memory, full.j, "consecutive changed configuration " + std::to_string(n));
    }
    Fixture wide(16384, 2, 2);
    wide.fill_random(rng);
    wide.j.x = 16384 - 256; wide.j.radius = 7; wide.j.flags = 1;
    check(wide.memory, wide.j, "maximum declared image width");
    Fixture tall(3, 65536, 1);
    tall.fill_random(rng);
    tall.j.y = 65536 - tall.j.h; tall.j.radius = 7;
    check(tall.memory, tall.j, "maximum declared image height");

    Fixture rounding(3, 3);
    const unsigned values[] = {0, 0, 1, 0, 0, 1, 1, 1, 1};
    for (unsigned i = 0; i < 9; ++i) rounding.pixel(i % 3, i / 3, values[i]);
    rounding.j.x = rounding.j.y = 1; rounding.j.w = rounding.j.h = 1; rounding.j.radius = 1;
    check(rounding.memory, rounding.j, "mean rounded only once");
    require(((rounding.memory[rounding.j.output] >> 8) & 255) == 1, "5/9 rounds to one");
    Fixture max_declared(2, 2);
    max_declared.j.memory_words = 1U << 29;
    check(max_declared.memory, max_declared.j, "maximum memory capacity accepted without huge allocation");
    max_declared.j.calibration = 0xffffffffU;
    check(max_declared.memory, max_declared.j, "disabled calibration offset ignored");
}

void randomized(std::mt19937& rng) {
    for (unsigned n = 0; n < 300; ++n) {
        Fixture f(1 + rng() % 600, 1 + rng() % 130, rng() % 8);
        f.fill_random(rng);
        f.j.w = 1 + rng() % std::min(256U, f.j.width);
        f.j.h = 1 + rng() % std::min(64U, f.j.height);
        f.j.x = rng() % (f.j.width - f.j.w + 1);
        f.j.y = rng() % (f.j.height - f.j.h + 1);
        if (n % 4 == 0) f.j.x = 0;
        if (n % 4 == 1) f.j.x = f.j.width - f.j.w;
        if (n % 4 == 2) f.j.y = 0;
        if (n % 4 == 3) f.j.y = f.j.height - f.j.h;
        f.j.radius = n % 8;
        f.j.flags = rng() % 2;
        f.j.polarity = rng() % 3;
        f.j.threshold = 1 + rng() % 255;
        for (unsigned x = 0; x < f.j.width; ++x) {
            uint32_t gain;
            switch (rng() % 7) {
                case 0: gain = 0; break;
                case 1: gain = 65536; break;
                case 2: gain = 0xffffff; break;
                case 3: gain = 32768; break;
                case 4: gain = 1; break;
                default: gain = rng() % 262145; break;
            }
            f.memory[f.j.calibration + x] = (gain << 8) | (rng() % 256);
        }
        check(f.memory, f.j, "random seed 0x51ca2022 case " + std::to_string(n));
    }
}

void rejections() {
    Fixture f(20, 10, 3);
    const Job base = f.j;
    Job j = base;
    j.memory_words = 0; reject(f.memory, j, 1, "zero memory capacity");
    j = base; j.memory_words = (1U << 29) + 1; reject(f.memory, j, 1, "capacity above max");
    const std::vector<uint32_t> before = f.memory;
    require(preprocess_tile(0, base.memory_words, base.input_offset, base.stride_bytes,
        base.width, base.height, base.x, base.y, base.w, base.h, base.radius,
        base.calibration, base.output, base.polarity, base.threshold, base.flags) == 1,
        "null pointer rejection");
    require(f.memory == before, "null pointer preserves actual allocation");
    ++rejected_cases;
    struct Mutation { uint32_t Job::*field; uint32_t value; unsigned status; const char* label; };
    const Mutation cases[] = {
        {&Job::width, 0, 2, "zero width"}, {&Job::width, 16385, 2, "large width"},
        {&Job::height, 0, 2, "zero height"}, {&Job::height, 65537, 2, "large height"},
        {&Job::w, 0, 2, "zero core width"}, {&Job::w, 257, 2, "large core width"},
        {&Job::h, 0, 2, "zero core height"}, {&Job::h, 65, 2, "large core height"},
        {&Job::x, 1, 2, "core right outside"}, {&Job::y, 1, 2, "core bottom outside"},
        {&Job::x, 0xffffffffU, 2, "core x addition widened"},
        {&Job::y, 0xffffffffU, 2, "core y addition widened"},
        {&Job::stride_bytes, 19, 2, "short stride"},
        {&Job::stride_bytes, 33, 2, "unaligned stride"},
        {&Job::radius, 8, 3, "large radius"}, {&Job::polarity, 3, 3, "unknown polarity"},
        {&Job::threshold, 0, 3, "zero threshold"}, {&Job::threshold, 256, 3, "large threshold"},
        {&Job::flags, 2, 3, "unknown flags"}, {&Job::flags, 0x80000001U, 3, "upper flags"},
        {&Job::input_offset, 0xffffffffU, 4, "input overflow widened"},
        {&Job::output, 0xffffffffU, 4, "output overflow widened"},
        {&Job::stride_bytes, 0xfffffffcU, 4, "input multiplication widened"},
        {&Job::output, base.input_offset, 5, "output aliases input"},
        {&Job::output, base.input_offset + 20 / 4, 5, "output aliases row padding"}
    };
    for (const Mutation& c : cases) { j = base; j.*(c.field) = c.value; reject(f.memory, j, c.status, c.label); }
    j = base; j.flags = 1; j.calibration = 0xffffffffU;
    reject(f.memory, j, 4, "calibration end widened");
    j = base; j.flags = 1; j.output = base.calibration;
    reject(f.memory, j, 5, "output aliases enabled calibration");
    j = base; j.output = base.input_offset + base.stride_bytes / 4 * base.height - 1;
    reject(f.memory, j, 5, "output aliases final padding word");
    j = base; j.memory_words = base.output + base.w * base.h - 1;
    reject(f.memory, j, 4, "one word short output range");
    j = base; j.flags = 1; j.memory_words = base.calibration + base.width - 1;
    j.output = 0; j.w = j.h = 1;
    reject(f.memory, j, 4, "one word short calibration range");
    j = base; j.memory_words = base.input_offset + base.stride_bytes / 4 * base.height - 1;
    j.output = 0; j.w = j.h = 1;
    reject(f.memory, j, 4, "one word short full input including final padding");
    // Adjacent ranges are legal. Disabled calibration may occupy output.
    j = base; j.output = base.input_offset + base.stride_bytes / 4 * base.height;
    j.calibration = j.output; j.w = j.h = 1;
    check(f.memory, j, "adjacent input output and ignored calibration alias");
}
}

int main() {
    try {
        std::mt19937 rng(0x51ca2022U);
        directed(rng);
        randomized(rng);
        rejections();
        std::cout << "PASS preprocess_tile: " << accepted_cases << " accepted cases, "
                  << rejected_cases << " rejected cases; independent full-image oracle; seed=0x51ca2022\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "FAIL preprocess_tile: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
