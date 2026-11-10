#include "goal_parsing/ParallelGoalReader.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {

// Bytes the scanner may read past the '\n' sentinel at end of input.
constexpr std::size_t tail_padding = 64;

[[noreturn]] void throw_system_error(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

// Returns the offset just past the next '}' outside comments, or npos.
// Comments are skipped exactly as the scanner does: "//" up to the newline, "/*" up to "*/".
// In valid GOAL, '/' only starts comments and '}' only closes rank blocks; for invalid
// input the scanner reports the error before reaching a misplaced boundary.
std::size_t find_block_end(std::string_view text, std::size_t position) {
    constexpr auto npos = std::string_view::npos;
    auto close = text.find('}', position);
    while (close != npos) {
        // memchr-based searches; most blocks contain no '/' at all.
        const auto slash = text.substr(position, close - position).find('/');
        if (slash == npos) return close + 1;
        const auto comment = position + slash;
        const char next = comment + 1 < text.size() ? text[comment + 1] : '\0';
        if (next == '/') {
            const auto line_end = text.find('\n', comment + 2);
            position = line_end == npos ? text.size() : line_end + 1;
        } else if (next == '*') {
            const auto comment_end = text.find("*/", comment + 2);
            position = comment_end == npos ? text.size() : comment_end + 2;
        } else {
            position = comment + 1;
        }
        if (close < position) close = text.find('}', position);
    }
    return npos;
}

int get_thread_number() {
#ifdef _OPENMP
    return omp_get_thread_num();
#else
    return 0;
#endif
}

// Prints a syntax error's source excerpt, as the sequential reader does, then rethrows.
[[noreturn]] void rethrow_reported(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const goal_parsing::GoalSyntaxError& syntax_error) {
        std::fputs(syntax_error.diagnostic().c_str(), stderr);
        throw;
    }
}

} // namespace

struct ParallelGoalReader::MappedFile {
    void* data = nullptr;
    std::size_t size = 0;

    explicit MappedFile(const std::string& path) {
        const int descriptor = open(path.c_str(), O_RDONLY);
        if (descriptor < 0) throw std::runtime_error("Couldn't open input file " + path);
        struct stat status{};
        if (fstat(descriptor, &status) != 0) {
            close(descriptor);
            throw_system_error("Cannot inspect " + path);
        }
        size = static_cast<std::size_t>(status.st_size);
        if (size != 0) {
            data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, descriptor, 0);
            if (data == MAP_FAILED) {
                data = nullptr;
                close(descriptor);
                throw_system_error("Cannot map " + path);
            }
        }
        close(descriptor);
    }
    ~MappedFile() {
        if (data) munmap(data, size);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};

bool ParallelGoalReader::supports(const std::string& path) {
    struct stat status{};
    return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

int ParallelGoalReader::get_thread_count(int threads) {
    if (threads > 0) return threads;
#ifdef _OPENMP
    return omp_get_max_threads();
#else
    return 1;
#endif
}

ParallelGoalReader::ParallelGoalReader(const std::string& path,
                                       std::vector<traceschema::FieldValueInfo> custom_fields)
    : fields_(std::move(custom_fields)), file_(std::make_unique<MappedFile>(path)) {
    text_ = {static_cast<const char*>(file_->data), file_->size};
    find_block_ends(1);
    goal_parsing::ParsedRankBlock first;
    try {
        first = parse_block(0, 0);
    } catch (...) {
        rethrow_reported(std::current_exception());
    }
    // Same checks, in the same order, as GoalReader::read_rank.
    if (first.num_ranks == 0) throw std::invalid_argument("Number of ranks undefined");
    if (first.rank != 0) throw std::invalid_argument("Rank blocks must be numbered consecutively from zero");
    num_ranks_ = first.num_ranks;
    first_rank_ = std::move(first.graph);
    first_rank_->SetRank(0);
    first_rank_->SetNumRanks(num_ranks_);
    // Later blocks are not needed: the sequential reader stops after num_ranks blocks.
    find_block_ends(num_ranks_);
}

ParallelGoalReader::~ParallelGoalReader() = default;

void ParallelGoalReader::find_block_ends(std::size_t count) {
    auto position = block_ends_.empty() ? 0 : block_ends_.back();
    while (block_ends_.size() < count) {
        const auto end = find_block_end(text_, position);
        if (end == std::string_view::npos) break;
        block_ends_.push_back(end);
        position = end;
    }
    if (block_ends_.size() < count) {
        tail_.assign(text_.substr(position));
        tail_.push_back('\n');
        tail_.append(tail_padding, '\0');
    }
}

goal_parsing::ParsedRankBlock ParallelGoalReader::parse_block(std::uint32_t index,
                                                              std::uint32_t num_ranks) const {
    const auto begin = index == 0 ? 0 : block_ends_[index - 1];
    const auto block = index < block_ends_.size()
        ? text_.substr(begin, block_ends_[index] - begin)
        : std::string_view(tail_).substr(0, tail_.size() - tail_padding);
    return goal_parsing::parse_rank_block(fields_, block, num_ranks, text_.substr(0, begin));
}

std::unique_ptr<Goal> ParallelGoalReader::read_rank(std::uint32_t rank) {
    if (rank == 0 && first_rank_) return std::move(first_rank_);
    auto block = parse_block(rank, num_ranks_);
    if (block.num_ranks != num_ranks_)
        throw std::invalid_argument("num_ranks is redeclared with a different value inside the schedule");
    if (block.rank != rank) throw std::invalid_argument("Rank blocks must be numbered consecutively from zero");
    block.graph->SetRank(rank);
    block.graph->SetNumRanks(num_ranks_);
    return std::move(block.graph);
}

void ParallelGoalReader::read_all(int threads, const RankCallbacks& callbacks) {
    // Blocks past the last closing brace are all the incomplete tail; parsing it once
    // reports the same error the sequential reader hits there.
    const auto count = static_cast<std::int64_t>(
        std::min<std::size_t>(num_ranks_, block_ends_.size() + 1));

    // Ranks above the lowest failure so far are skipped; ranks below it still run, so the
    // reported error is the first one in rank order, as in a sequential pass.
    std::atomic<std::uint32_t> first_failure{std::numeric_limits<std::uint32_t>::max()};
    std::mutex error_mutex;
    std::exception_ptr error;
    const auto fail = [&](std::uint32_t rank) {
        const std::lock_guard lock(error_mutex);
        if (rank < first_failure.load()) {
            error = std::current_exception();
            first_failure.store(rank);
        }
    };
    const auto skipped = [&](std::uint32_t rank) {
        return rank > first_failure.load(std::memory_order_relaxed);
    };
    const auto parse = [&](std::uint32_t rank) -> std::unique_ptr<Goal> {
        if (skipped(rank)) return nullptr;
        try {
            auto graph = read_rank(rank);
            if (callbacks.analyse) callbacks.analyse(get_thread_number(), *graph);
            return graph;
        } catch (...) {
            fail(rank);
            return nullptr;
        }
    };
    const auto finish = [&](std::uint32_t rank, Goal* graph) {
        if (!graph || !callbacks.finish || skipped(rank)) return;
        try {
            callbacks.finish(*graph);
        } catch (...) {
            fail(rank);
        }
    };

    const int team = get_thread_count(threads);
    if (callbacks.finish_in_rank_order) {
        // Each rank waits until the previous one is finished. Waiting threads sleep: with
        // `omp ordered` they spin, and when every core runs a thread the spinning starves the
        // thread that writes. Ranks are handed out in order, so the previous rank is always
        // already being processed by another thread.
        std::mutex order_mutex;
        std::condition_variable order_changed;
        std::int64_t next_to_finish = 0;
        #pragma omp parallel for schedule(dynamic, 1) num_threads(team)
        for (std::int64_t rank = 0; rank < count; ++rank) {
            auto graph = parse(static_cast<std::uint32_t>(rank));
            {
                std::unique_lock lock(order_mutex);
                order_changed.wait(lock, [&] { return next_to_finish == rank; });
            }
            // Only this rank can pass the wait until next_to_finish advances.
            finish(static_cast<std::uint32_t>(rank), graph.get());
            {
                const std::lock_guard lock(order_mutex);
                ++next_to_finish;
            }
            order_changed.notify_all();
        }
    } else {
        #pragma omp parallel for schedule(dynamic, 1) num_threads(team)
        for (std::int64_t rank = 0; rank < count; ++rank) {
            auto graph = parse(static_cast<std::uint32_t>(rank));
            if (graph && callbacks.finish) {
                #pragma omp critical(parallel_goal_reader_finish)
                finish(static_cast<std::uint32_t>(rank), graph.get());
            }
        }
    }
    if (error) rethrow_reported(error);
    if (count < num_ranks_)
        throw std::invalid_argument("Unexpected end of input: missing rank or closing bracket");
}
