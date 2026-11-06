// Standalone portable test: g++ -std=c++11 -O2 test_stream_components_cpp.cpp
#include "../cpu/stream_components.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <queue>
#include <random>
#include <sstream>
#include <tuple>
#include <vector>

namespace {
using linescan::Component;
using linescan::StreamComponents;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Exception, class F> void expect_throw(F action) {
    bool threw = false;
    try { action(); } catch (const Exception&) { threw = true; }
    require(threw, "expected exception missing");
}

typedef std::tuple<std::uint32_t, std::uint64_t, std::uint32_t,
                   std::uint64_t, std::uint64_t, unsigned, int,
                   std::uint64_t> Key;
Key key(const Component& c) {
    return Key(c.x0, c.y0, c.x1, c.y1, c.area, c.peak, c.polarity, c.frame_id);
}

void equal_components(std::vector<Component> a, std::vector<Component> b) {
    const auto less = [](const Component& x, const Component& y) { return key(x) < key(y); };
    std::sort(a.begin(), a.end(), less);
    std::sort(b.begin(), b.end(), less);
    require(a.size() == b.size(), "component count mismatch");
    for (std::size_t i = 0; i < a.size(); ++i)
        require(key(a[i]) == key(b[i]), "component statistics mismatch");
}

// Independent pixel flood fill, deliberately using no run/union-find machinery.
std::vector<Component> oracle(const std::vector<std::int16_t>& response,
                              const std::vector<std::uint8_t>& mask,
                              std::size_t width, std::uint64_t frame_id,
                              std::uint64_t min_area, std::uint64_t max_area) {
    const std::size_t height = mask.size() / width;
    std::vector<unsigned char> seen(mask.size(), 0);
    std::vector<Component> output;
    for (std::size_t start = 0; start < mask.size(); ++start) {
        if (!mask[start] || seen[start]) continue;
        const int polarity = response[start] > 0 ? 1 : -1;
        Component c = {static_cast<std::uint32_t>(width), height, 0, 0, 0, 0,
                       polarity, frame_id};
        std::queue<std::size_t> pending;
        pending.push(start);
        seen[start] = 1;
        while (!pending.empty()) {
            const std::size_t p = pending.front();
            pending.pop();
            const std::size_t x = p % width, y = p / width;
            c.x0 = (std::min)(c.x0, static_cast<std::uint32_t>(x));
            c.x1 = (std::max)(c.x1, static_cast<std::uint32_t>(x + 1));
            c.y0 = (std::min)(c.y0, static_cast<std::uint64_t>(y));
            c.y1 = (std::max)(c.y1, static_cast<std::uint64_t>(y + 1));
            ++c.area;
            c.peak = (std::max)(c.peak, static_cast<std::uint16_t>(std::abs(response[p])));
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const long nx = static_cast<long>(x) + dx, ny = static_cast<long>(y) + dy;
                if (nx < 0 || ny < 0 || nx >= static_cast<long>(width) ||
                    ny >= static_cast<long>(height)) continue;
                const std::size_t q = static_cast<std::size_t>(ny) * width + nx;
                if (mask[q] && !seen[q] && (response[q] > 0 ? 1 : -1) == polarity) {
                    seen[q] = 1;
                    pending.push(q);
                }
            }
        }
        if (c.area >= min_area && (!max_area || c.area <= max_area)) output.push_back(c);
    }
    return output;
}

void check_frame(const std::vector<std::int16_t>& response,
                  const std::vector<std::uint8_t>& mask, std::size_t width,
                  std::uint64_t frame_id = 731, std::uint64_t min_area = 1,
                  std::uint64_t max_area = 0) {
    StreamComponents stream(width, frame_id, min_area, max_area);
    std::vector<Component> actual;
    for (std::size_t y = 0; y < mask.size() / width; ++y) {
        const std::vector<Component> row = stream.consume_row(&response[y * width], &mask[y * width], width);
        actual.insert(actual.end(), row.begin(), row.end());
        require(row.size() <= width, "per-row output exceeds width");
        require(stream.active_state_count() <= 2 * width, "retained frontier exceeds bound");
        require(stream.rows_consumed() == y + 1, "row counter mismatch");
        for (std::size_t i = 0; i < row.size(); ++i)
            require(row[i].y1 <= y, "component emitted before leaving frontier");
    }
    const std::vector<Component> tail = stream.finish();
    actual.insert(actual.end(), tail.begin(), tail.end());
    require(tail.size() <= width, "finish output exceeds width");
    require(stream.active_state_count() == 0, "finish retains active state");
    require(stream.finish().empty(), "finish must be idempotent");
    equal_components(actual, oracle(response, mask, width, frame_id, min_area, max_area));
}

void random_frames() {
    std::mt19937 random(0x73288u);
    for (unsigned trial = 0; trial < 3000; ++trial) {
        const std::size_t width = 1 + random() % 35, height = random() % 35;
        std::vector<std::int16_t> response(width * height);
        std::vector<std::uint8_t> mask(width * height);
        for (std::size_t i = 0; i < response.size(); ++i) {
            // Include nonzero unmasked responses and zero only outside the mask.
            const int amplitude = 1 + random() % 255;
            response[i] = static_cast<std::int16_t>((random() & 1) ? amplitude : -amplitude);
            mask[i] = random() % 100 < (trial % 3 == 0 ? 90 : trial % 3 == 1 ? 15 : 55);
            if (!mask[i] && random() % 3 == 0) response[i] = 0;
        }
        const std::uint64_t minimum = trial % 7 == 0 ? 3 : 1;
        const std::uint64_t maximum = trial % 5 == 0 ? 14 : 0;
        check_frame(response, mask, width, trial, minimum, maximum);
    }
}

void split_merge_and_boundaries() {
    const char* rows[] = {"++..++..--..--", "++++++++------", "+.+.+.+.-.-.-.",
                          "++++++++------", ".+..+..+--..--", "..............",
                          "+-+-+-+-+-+-+-", "-+-+-+-+-+-+-+"};
    std::vector<std::int16_t> response;
    std::vector<std::uint8_t> mask;
    for (std::size_t y = 0; y < sizeof(rows) / sizeof(rows[0]); ++y)
        for (std::size_t x = 0; x < 14; ++x) {
            mask.push_back(rows[y][x] != '.');
            response.push_back(rows[y][x] == '+' ? 255 : rows[y][x] == '-' ? -255 : 0);
        }
    check_frame(response, mask, 14);
    check_frame(response, mask, 14, 19, 2, 17);
    const std::size_t width = 520, height = 131;
    response.assign(width * height, 0);
    mask.assign(width * height, 0);
    for (std::size_t y = 59; y < 69; ++y) for (std::size_t x = 252; x < 261; ++x) {
        mask[y * width + x] = 1;
        response[y * width + x] = static_cast<std::int16_t>(x == 256 ? -231 : 147);
    }
    // Diagonal connections on both sides of x=255 and y=63.
    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t p = y * width + 195 + y;
        mask[p] = 1;
        response[p] = 67;
    }
    check_frame(response, mask, width, (std::numeric_limits<std::uint64_t>::max)());
}

void full_width_no_output_cap() {
    const std::size_t width = 16384;
    std::vector<std::int16_t> response(width);
    std::vector<std::uint8_t> mask(width, 1);
    for (std::size_t x = 0; x < width; ++x) response[x] = x % 2 ? -255 : 255;
    StreamComponents stream(width);
    require(stream.consume_row(response.data(), mask.data(), width).empty(), "new components must remain live");
    require(stream.active_state_count() == 2 * width, "maximum frontier accounting mismatch");
    std::fill(mask.begin(), mask.end(), 0);
    const std::vector<Component> closed = stream.consume_row(response.data(), mask.data(), width);
    require(closed.size() == width, "output unexpectedly capped");
    for (std::size_t i = 0; i < closed.size(); ++i)
        require(closed[i].area == 1 && closed[i].peak == 255, "full-width statistics mismatch");
    require(stream.active_state_count() == 0, "closed frontier must be empty");
    require(stream.finish().empty(), "empty tail expected");
}

void long_component() {
    const std::size_t width = 64;
    StreamComponents stream(width, 999);
    std::vector<std::int16_t> response(width, -129);
    std::vector<std::uint8_t> mask(width, 1);
    for (std::size_t y = 0; y < 10000; ++y) {
        require(stream.consume_row(response.data(), mask.data(), width).empty(), "long component prematurely closed");
        require(stream.active_state_count() == 2, "history retained in long stream");
    }
    const std::vector<Component> result = stream.finish();
    require(result.size() == 1 && result[0].area == 640000 && result[0].y1 == 10000 &&
            result[0].x0 == 0 && result[0].x1 == width && result[0].y0 == 0 &&
            result[0].frame_id == 999 && result[0].polarity == -1 && result[0].peak == 129,
            "long component statistics mismatch");
}

void invalid_rows_and_lifecycle() {
    expect_throw<std::invalid_argument>([] { StreamComponents s(0); });
    expect_throw<std::invalid_argument>([] { StreamComponents s(16385); });
    expect_throw<std::invalid_argument>([] { StreamComponents s(3, 0, 0); });
    expect_throw<std::invalid_argument>([] { StreamComponents s(3, 0, 2, 1); });
    StreamComponents stream(3, 85);
    std::int16_t response[] = {4, 4, 4};
    std::uint8_t mask[] = {1, 1, 1};
    stream.consume_row(response, mask, 3);
    const auto unchanged = [&stream] {
        require(stream.rows_consumed() == 1 && stream.active_state_count() == 2,
                "invalid input changed stream state");
    };
    expect_throw<std::invalid_argument>([&] { stream.consume_row(response, mask, 2); }); unchanged();
    expect_throw<std::invalid_argument>([&] { stream.consume_row(NULL, mask, 3); }); unchanged();
    expect_throw<std::invalid_argument>([&] { stream.consume_row(response, NULL, 3); }); unchanged();
    mask[2] = 2;
    expect_throw<std::invalid_argument>([&] { stream.consume_row(response, mask, 3); }); unchanged();
    mask[2] = 1; response[2] = 0;
    expect_throw<std::invalid_argument>([&] { stream.consume_row(response, mask, 3); }); unchanged();
    mask[2] = 0; response[2] = 256;
    expect_throw<std::invalid_argument>([&] { stream.consume_row(response, mask, 3); }); unchanged();
    response[2] = -256;
    expect_throw<std::invalid_argument>([&] { stream.consume_row(response, mask, 3); }); unchanged();
    response[2] = 4; mask[2] = 1;
    stream.consume_row(response, mask, 3);
    const std::vector<Component> result = stream.finish();
    require(result.size() == 1 && result[0].area == 6 && result[0].y1 == 2,
            "invalid row partially mutated frontier");
    require(stream.finish().empty() && stream.rows_consumed() == 2, "repeat finish mismatch");
    expect_throw<std::logic_error>([&] { stream.consume_row(response, mask, 3); });
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    require(linescan::detail::checked_add_u64(maximum - 1, 1) == maximum,
            "checked arithmetic rejected representable boundary");
    expect_throw<std::overflow_error>([&] { linescan::detail::checked_add_u64(maximum, 1); });
    expect_throw<std::overflow_error>([&] { linescan::detail::checked_add_u64(maximum - 4, 5); });
    StreamComponents empty(1);
    require(empty.finish().empty() && empty.finish().empty(), "empty lifecycle mismatch");
}
}  // namespace

int main() {
    try {
        random_frames();
        split_merge_and_boundaries();
        full_width_no_output_cap();
        long_component();
        invalid_rows_and_lifecycle();
        std::cout << "PASS: C++ streaming components; 3000 random oracle frames, split/merge, "
                     "16K width, stripe boundaries, 10000-row stream, validation and lifecycle\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
