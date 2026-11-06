#ifndef LINESCAN_CPU_STREAM_COMPONENTS_HPP
#define LINESCAN_CPU_STREAM_COMPONENTS_HPP

// Portable CPU reference, C++11. This is deliberately not an HLS kernel.
// Feed complete rows in global order after stitching horizontal tile cores.
// Connectivity is 8-neighbour, with positive and negative responses separate.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace linescan {

struct Component {
    std::uint32_t x0;
    std::uint64_t y0;
    std::uint32_t x1;
    std::uint64_t y1;  // Half-open bounding box [x0,x1) x [y0,y1).
    std::uint64_t area;
    std::uint16_t peak;  // Absolute response amplitude, 1..255.
    int polarity;        // +1 bright, -1 dark.
    std::uint64_t frame_id;
};

namespace detail {
inline std::uint64_t checked_add_u64(std::uint64_t a, std::uint64_t b) {
    if (b > (std::numeric_limits<std::uint64_t>::max)() - a)
        throw std::overflow_error("stream component coordinate/area overflow");
    return a + b;
}
}  // namespace detail

class StreamComponents {
public:
    explicit StreamComponents(std::size_t width, std::uint64_t frame_id = 0,
                              std::uint64_t min_area = 1,
                              std::uint64_t max_area = 0)
        : width_(width), frame_id_(frame_id), min_area_(min_area),
          max_area_(max_area), y_(0), finished_(false) {
        if (width == 0 || width > 16384)
            throw std::invalid_argument("width must be in [1,16384]");
        if (min_area == 0 || (max_area != 0 && max_area < min_area))
            throw std::invalid_argument("invalid area filter");
    }

    std::size_t width() const { return width_; }
    std::uint64_t rows_consumed() const { return y_; }
    // Retained records, not bytes; scratch space during a call is also O(width).
    std::size_t active_state_count() const { return runs_.size() + stats_.size(); }

    // Input buffers each contain exactly width entries. Every mask must be 0/1,
    // every response in [-255,255], and masked responses must be nonzero.
    // Validation and all work occur before committing a new frontier; invalid
    // input, arithmetic overflow, or allocation failure leaves the stream intact.
    // Output has no cap/order guarantee. The caller owns accumulated output.
    std::vector<Component> consume_row(const std::int16_t* signed_response,
                                       const std::uint8_t* mask,
                                       std::size_t width) {
        if (finished_) throw std::logic_error("cannot consume rows after finish");
        if (width != width_ || signed_response == NULL || mask == NULL)
            throw std::invalid_argument("row width/buffer mismatch");
        for (std::size_t x = 0; x < width_; ++x) {
            const int value = signed_response[x];
            if (mask[x] > 1 || value < -255 || value > 255 ||
                (mask[x] && value == 0))
                throw std::invalid_argument("invalid response or mask value");
        }
        const std::uint64_t next_y = detail::checked_add_u64(y_, 1);

        // This union-find exists for this row only. No historical labels survive.
        std::vector<Stats> stats(stats_);
        std::vector<std::size_t> parent(stats.size());
        std::vector<unsigned char> rank(stats.size(), 0);
        for (std::size_t i = 0; i < parent.size(); ++i) parent[i] = i;
        std::vector<Run> current;
        current.reserve(width_);
        std::size_t previous_start = 0;
        std::size_t x = 0;
        while (x < width_) {
            if (!mask[x]) { ++x; continue; }
            const std::size_t x0 = x;
            const int polarity = signed_response[x] > 0 ? 1 : -1;
            std::uint16_t peak = amplitude(signed_response[x]);
            ++x;
            while (x < width_ && mask[x] &&
                   (signed_response[x] > 0 ? 1 : -1) == polarity) {
                peak = (std::max)(peak, amplitude(signed_response[x]));
                ++x;
            }
            const std::size_t x1 = x;
            std::size_t label = parent.size();
            parent.push_back(label);
            rank.push_back(0);
            Stats s = {static_cast<std::uint32_t>(x0), y_,
                       static_cast<std::uint32_t>(x1), next_y,
                       static_cast<std::uint64_t>(x1 - x0), peak, polarity};
            stats.push_back(s);
            // Equality at either edge means diagonal adjacency for half-open runs.
            while (previous_start < runs_.size() && runs_[previous_start].x1 < x0)
                ++previous_start;
            for (std::size_t i = previous_start;
                 i < runs_.size() && runs_[i].x0 <= x1; ++i) {
                const std::size_t old_root = find(parent, runs_[i].label);
                if (stats[old_root].polarity == polarity)
                    label = unite(stats, parent, rank, label, old_root);
            }
            Run r = {static_cast<std::uint32_t>(x0),
                     static_cast<std::uint32_t>(x1), label};
            current.push_back(r);
        }

        std::vector<unsigned char> live(stats.size(), 0);
        for (std::size_t i = 0; i < current.size(); ++i)
            live[find(parent, current[i].label)] = 1;
        std::vector<Component> completed;
        for (std::size_t i = 0; i < stats.size(); ++i)
            if (parent[i] == i && !live[i]) append_if_selected(completed, stats[i]);

        const std::size_t absent = (std::numeric_limits<std::size_t>::max)();
        std::vector<std::size_t> compact(stats.size(), absent);
        std::vector<Stats> next_stats;
        next_stats.reserve(current.size());
        for (std::size_t i = 0; i < current.size(); ++i) {
            const std::size_t root = find(parent, current[i].label);
            if (compact[root] == absent) {
                compact[root] = next_stats.size();
                next_stats.push_back(stats[root]);
            }
            current[i].label = compact[root];
        }
        runs_.swap(current);
        stats_.swap(next_stats);
        y_ = next_y;
        return completed;
    }

    // Idempotent. Row ingestion after this call is an error.
    std::vector<Component> finish() {
        std::vector<Component> completed;
        if (finished_) return completed;
        for (std::size_t i = 0; i < stats_.size(); ++i)
            append_if_selected(completed, stats_[i]);
        runs_.clear();
        stats_.clear();
        finished_ = true;
        return completed;
    }

private:
    struct Run { std::uint32_t x0, x1; std::size_t label; };
    struct Stats {
        std::uint32_t x0;
        std::uint64_t y0;
        std::uint32_t x1;
        std::uint64_t y1, area;
        std::uint16_t peak;
        int polarity;
    };
    static std::uint16_t amplitude(std::int16_t value) {
        return static_cast<std::uint16_t>(value < 0 ? -static_cast<int>(value) : value);
    }
    static std::size_t find(std::vector<std::size_t>& parent, std::size_t label) {
        while (parent[label] != label) {
            parent[label] = parent[parent[label]];
            label = parent[label];
        }
        return label;
    }
    static std::size_t unite(std::vector<Stats>& stats,
                             std::vector<std::size_t>& parent,
                             std::vector<unsigned char>& rank,
                             std::size_t a, std::size_t b) {
        a = find(parent, a);
        b = find(parent, b);
        if (a == b) return a;
        if (rank[a] < rank[b]) std::swap(a, b);
        const std::uint64_t area = detail::checked_add_u64(stats[a].area, stats[b].area);
        stats[a].x0 = (std::min)(stats[a].x0, stats[b].x0);
        stats[a].y0 = (std::min)(stats[a].y0, stats[b].y0);
        stats[a].x1 = (std::max)(stats[a].x1, stats[b].x1);
        stats[a].y1 = (std::max)(stats[a].y1, stats[b].y1);
        stats[a].area = area;
        stats[a].peak = (std::max)(stats[a].peak, stats[b].peak);
        parent[b] = a;
        if (rank[a] == rank[b]) ++rank[a];
        return a;
    }
    void append_if_selected(std::vector<Component>& output, const Stats& s) const {
        if (s.area < min_area_ || (max_area_ && s.area > max_area_)) return;
        Component c = {s.x0, s.y0, s.x1, s.y1, s.area, s.peak, s.polarity, frame_id_};
        output.push_back(c);
    }
    std::size_t width_;
    std::uint64_t frame_id_, min_area_, max_area_, y_;
    bool finished_;
    std::vector<Run> runs_;
    std::vector<Stats> stats_;
};

}  // namespace linescan
#endif  // LINESCAN_CPU_STREAM_COMPONENTS_HPP
