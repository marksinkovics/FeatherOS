#include <catch2/catch_test_macros.hpp>

#include "kernel/log_ring.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{

using FeatherOS::LogRing;

class SmallRing
{
public:
    explicit SmallRing(size_t capacity) : storage(capacity, '#'), ring(storage.data(), capacity) {}

    void Append(const std::string& line)
    {
        ring.Append(line.data(), line.size());
    }

    std::string Snapshot(size_t capacity = 4096)
    {
        std::vector<char> buffer(capacity);
        const size_t length = ring.Snapshot(buffer.data(), buffer.size());
        return {buffer.data(), length};
    }

private:
    std::vector<char> storage;
    LogRing ring;
};

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = text.find('\n', start);
        REQUIRE(end != std::string::npos);
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

} // namespace

TEST_CASE("log ring: an empty ring snapshots as empty text", "[logring]")
{
    SmallRing ring(32);
    REQUIRE(ring.Snapshot().empty());
    char one[1] = {'x'};
    LogRing raw(one, 1);
    REQUIRE(raw.Snapshot(one, 1) == 0);
    REQUIRE(one[0] == '\0');
}

TEST_CASE("log ring: lines come back in order with a newline each", "[logring]")
{
    SmallRing ring(64);
    ring.Append("one");
    ring.Append("two\n");
    ring.Append("");
    REQUIRE(ring.Snapshot() == "one\ntwo\n\n");
}

TEST_CASE("log ring: overwriting drops whole oldest lines", "[logring]")
{
    SmallRing ring(40);
    for (int number = 0; number < 100; ++number)
    {
        ring.Append("line " + std::to_string(number));
        const std::string text = ring.Snapshot();
        const auto lines = Lines(text);
        REQUIRE_FALSE(lines.empty());
        REQUIRE(lines.back() == "line " + std::to_string(number));
        REQUIRE(text.size() <= 40);
        for (size_t index = 0; index < lines.size(); ++index)
        {
            const int expected = number - static_cast<int>(lines.size() - 1 - index);
            REQUIRE(lines[index] == "line " + std::to_string(expected));
        }
    }
}

TEST_CASE("log ring: a line that fills the ring exactly keeps only itself", "[logring]")
{
    SmallRing ring(16);
    ring.Append("old");
    ring.Append(std::string(15, 'x'));
    REQUIRE(ring.Snapshot() == std::string(15, 'x') + "\n");
}

TEST_CASE("log ring: a line longer than the ring is cut to fit", "[logring]")
{
    SmallRing ring(16);
    ring.Append("old");
    ring.Append(std::string(100, 'y'));
    REQUIRE(ring.Snapshot() == std::string(15, 'y') + "\n");
    ring.Append("next");
    REQUIRE(ring.Snapshot() == "next\n");
}

TEST_CASE("log ring: lines wrap around the end of the storage", "[logring]")
{
    SmallRing ring(20);
    ring.Append("aaaaaaa");
    ring.Append("bbbbbbb");
    ring.Append("cccc");
    REQUIRE(ring.Snapshot() == "bbbbbbb\ncccc\n");
    ring.Append("ddddddd");
    REQUIRE(ring.Snapshot() == "cccc\nddddddd\n");
}

TEST_CASE("log ring: a short snapshot buffer gets the newest whole lines", "[logring]")
{
    SmallRing ring(64);
    ring.Append("first");
    ring.Append("second");
    ring.Append("third");
    REQUIRE(ring.Snapshot(20) == "first\nsecond\nthird\n");
    REQUIRE(ring.Snapshot(19) == "second\nthird\n");
    REQUIRE(ring.Snapshot(14) == "second\nthird\n");
    REQUIRE(ring.Snapshot(13) == "third\n");
    REQUIRE(ring.Snapshot(7) == "third\n");
    REQUIRE(ring.Snapshot(6).empty());
    REQUIRE(ring.Snapshot(1).empty());
    REQUIRE(ring.Snapshot(0).empty());
}

TEST_CASE("log ring: the kernel's ring takes 64 KiB and snapshots whole lines", "[logring]")
{
    namespace KernelLog = FeatherOS::KernelLog;
    for (int number = 0; number < 5000; ++number)
    {
        KernelLog::Append(("KERN/I: ring fill " + std::to_string(number)).c_str());
    }
    std::vector<char> buffer(KernelLog::Capacity + 1);
    const size_t length = KernelLog::Snapshot(buffer.data(), buffer.size());
    REQUIRE(length <= KernelLog::Capacity);
    REQUIRE(length > KernelLog::Capacity - 64);
    const std::string text(buffer.data(), length);
    REQUIRE(text.rfind("KERN/I: ring fill 4999\n") == text.size() - 23);
    REQUIRE(text.compare(0, 5, "KERN/") == 0);
    KernelLog::Append(nullptr);
}

TEST_CASE("log ring: readers see whole lines while two writers append", "[logring][concurrency]")
{
    constexpr int PerWriter = 4000;
    SmallRing ring(512);
    std::atomic<bool> done = false;
    std::atomic<int> failures = 0;
    std::vector<std::thread> writers;
    for (int writer = 0; writer < 2; ++writer)
    {
        writers.emplace_back(
            [&ring, writer]
            {
                for (int number = 0; number < PerWriter; ++number)
                {
                    const std::string line = "W" + std::to_string(writer) + " " +
                                             std::to_string(number) + " " + std::string(20, 'z');
                    ring.Append(line);
                }
            });
    }
    std::thread reader(
        [&]
        {
            int last[2] = {-1, -1};
            while (!done.load())
            {
                const std::string text = ring.Snapshot();
                size_t start = 0;
                int seen[2] = {-1, -1};
                while (start < text.size())
                {
                    const size_t end = text.find('\n', start);
                    if (end == std::string::npos)
                    {
                        ++failures;
                        return;
                    }
                    const std::string line = text.substr(start, end - start);
                    start = end + 1;
                    const size_t space = line.find(' ', 3);
                    if (line.size() < 4 || line[0] != 'W' || (line[1] != '0' && line[1] != '1') ||
                        space == std::string::npos || line.substr(space + 1) != std::string(20, 'z'))
                    {
                        ++failures;
                        return;
                    }
                    const int writer = line[1] - '0';
                    const int number = std::atoi(line.substr(2).c_str());
                    if (seen[writer] != -1 && number != seen[writer] + 1)
                    {
                        ++failures;
                    }
                    seen[writer] = number;
                }
                for (int writer = 0; writer < 2; ++writer)
                {
                    if (seen[writer] != -1 && seen[writer] < last[writer])
                    {
                        ++failures;
                    }
                    if (seen[writer] != -1)
                    {
                        last[writer] = seen[writer];
                    }
                }
            }
        });
    for (std::thread& writer : writers)
    {
        writer.join();
    }
    done = true;
    reader.join();
    REQUIRE(failures.load() == 0);
    const auto lines = Lines(ring.Snapshot());
    REQUIRE_FALSE(lines.empty());
}
