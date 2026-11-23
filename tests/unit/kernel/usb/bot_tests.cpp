#include <catch2/catch_test_macros.hpp>

#include "kernel/usb/storage/bot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace
{

using namespace FeatherOS::Usb::Storage;
using namespace FeatherOS::Usb::Storage::Bot;
using FeatherOS::Usb::Status;
using FeatherOS::Usb::Succeeded;
using Scsi::Direction;

using Log = std::vector<std::string>;

const char* StageName(Stage stage)
{
    switch (stage)
    {
    case Stage::Command:
        return "command";
    case Stage::DataIn:
        return "data-in";
    case Stage::DataOut:
        return "data-out";
    default:
        return "status";
    }
}

void Put32(uint8_t* out, uint32_t value)
{
    for (uint32_t i = 0; i < 4; i++)
    {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t Get32(const uint8_t* in)
{
    return static_cast<uint32_t>(in[0]) | static_cast<uint32_t>(in[1]) << 8 |
           static_cast<uint32_t>(in[2]) << 16 | static_cast<uint32_t>(in[3]) << 24;
}

class ModelPipe final : public Pipe
{
public:
    Status cbw_status = Status::Success;
    Status data_status = Status::Success;
    uint32_t data_actual = 0;
    std::vector<Status> csw_status{Status::Success};
    uint32_t csw_actual = CswSize;
    uint8_t csw_code = 0;
    uint32_t csw_residue = 0;
    std::function<void(std::span<uint8_t>, uint32_t tag)> shape_csw;
    bool fail_start = false;
    Stage fail_start_stage = Stage::Command;
    Status fail_start_status = Status::NoDevice;
    Status recover_status = Status::Success;

    Log log;
    std::vector<uint8_t> cbw;
    std::vector<uint64_t> deadlines;
    std::vector<uint32_t> lengths;
    uint64_t now = 1000;

    std::span<uint8_t> Buffer(Stage stage) override
    {
        switch (stage)
        {
        case Stage::Command:
            return command_;
        case Stage::Status:
            return status_;
        default:
            return data_;
        }
    }

    bool Start(Stage stage, uint32_t length, Status* status) override
    {
        log.push_back(std::string("start:") + StageName(stage));
        lengths.push_back(length);
        if (fail_start && stage == fail_start_stage)
        {
            *status = fail_start_status;
            return false;
        }
        current_ = stage;
        if (stage == Stage::Command)
        {
            cbw.assign(command_.begin(), command_.end());
        }
        return true;
    }

    Status Finish(uint64_t deadline_ms, uint32_t* actual) override
    {
        log.push_back(std::string("finish:") + StageName(current_));
        deadlines.push_back(deadline_ms);
        switch (current_)
        {
        case Stage::Command:
            *actual = CbwSize;
            return cbw_status;
        case Stage::DataIn:
        case Stage::DataOut:
            *actual = data_actual;
            return data_status;
        default:
            break;
        }
        for (auto& byte : status_)
        {
            byte = 0;
        }
        Put32(&status_[0], CswSignature);
        Put32(&status_[4], Get32(&cbw[4]));
        Put32(&status_[8], csw_residue);
        status_[12] = csw_code;
        if (shape_csw)
        {
            shape_csw(status_, Get32(&cbw[4]));
        }
        *actual = csw_actual;
        const Status result = csw_status.front();
        if (csw_status.size() > 1)
        {
            csw_status.erase(csw_status.begin());
        }
        return result;
    }

    Status Recover() override
    {
        log.push_back("recover");
        return recover_status;
    }

    uint64_t NowMilliseconds() override { return now; }

private:
    std::array<uint8_t, CbwSize> command_{};
    std::array<uint8_t, CswSize> status_{};
    std::array<uint8_t, 4096> data_{};
    Stage current_ = Stage::Command;
};

constexpr uint32_t Tag = 0xAABBCCDD;

Command Inquiry36()
{
    return {Scsi::Inquiry(36), Direction::In, 36, DataTimeoutMs};
}

Command ReadCommand(uint32_t blocks = 8)
{
    return {Scsi::Read(0, blocks), Direction::In, blocks * 512, DataTimeoutMs};
}

Command WriteCommand(uint32_t blocks = 8)
{
    return {Scsi::Write(0, blocks), Direction::Out, blocks * 512, DataTimeoutMs};
}

Command OutNoWrite(uint32_t length)
{
    Scsi::Cdb cdb{};
    cdb.bytes[0] = 0x15;
    cdb.length = 6;
    return {cdb, Direction::Out, length, DataTimeoutMs};
}

Command NoData()
{
    return {Scsi::TestUnitReady(), Direction::None, 0, DataTimeoutMs};
}

const Log Plain{"start:command", "finish:command", "start:status", "finish:status"};
const Log PlainIn{"start:command", "finish:command", "start:data-in", "finish:data-in",
                  "start:status", "finish:status"};
const Log PlainOut{"start:command", "finish:command", "start:data-out", "finish:data-out",
                   "start:status", "finish:status"};

Log With(Log log, const char* extra)
{
    log.emplace_back(extra);
    return log;
}

std::vector<uint8_t> Csw(uint32_t tag, uint32_t residue, uint8_t status)
{
    std::vector<uint8_t> bytes(CswSize, 0);
    Put32(&bytes[0], CswSignature);
    Put32(&bytes[4], tag);
    Put32(&bytes[8], residue);
    bytes[12] = status;
    return bytes;
}

CswCheck Check(const std::vector<uint8_t>& bytes, uint32_t actual, uint32_t requested = 512)
{
    uint32_t residue = 0xDEADBEEF;
    return CheckCsw(std::span<const uint8_t>(bytes.data(), bytes.size()), actual, Tag, requested,
                    &residue);
}

TEST_CASE("bot T10: a CBW has the exact byte layout", "[usb][storage][bot]")
{
    std::array<uint8_t, CbwSize> cbw{};
    cbw.fill(0xEE);
    PackCbw(cbw.data(), Tag, 4096, Direction::In, Scsi::Read(0x12345678, 8));
    const std::array<uint8_t, CbwSize> expected{
        0x55, 0x53, 0x42, 0x43, 0xDD, 0xCC, 0xBB, 0xAA, 0x00, 0x10, 0x00, 0x00, 0x80, 0x00, 0x0A,
        0x28, 0x00, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00};
    CHECK(cbw == expected);

    PackCbw(cbw.data(), 1, 512, Direction::Out, Scsi::Write(0, 1));
    CHECK(cbw[12] == 0x00);
    CHECK(cbw[13] == 0);
    CHECK(cbw[14] == 10);
    CHECK(Get32(&cbw[8]) == 512);

    PackCbw(cbw.data(), 2, 0, Direction::None, Scsi::TestUnitReady());
    CHECK(cbw[12] == 0x00);
    CHECK(cbw[14] == 6);
    CHECK(Get32(&cbw[8]) == 0);
    for (uint32_t i = 15; i < CbwSize; i++)
    {
        CHECK(cbw[i] == 0);
    }

    PackCbw(cbw.data(), 3, 16, Direction::In, Scsi::Read(1ULL << 33, 1));
    CHECK(cbw[14] == 16);
    CHECK(cbw[15] == 0x88);
    CHECK(cbw[30] == 0);
}

TEST_CASE("bot T10: Execute sends the CBW with the given tag and LUN 0", "[usb][storage][bot]")
{
    ModelPipe pipe;
    Execute(pipe, Tag, ReadCommand());
    REQUIRE(pipe.cbw.size() == CbwSize);
    CHECK(Get32(&pipe.cbw[0]) == CbwSignature);
    CHECK(Get32(&pipe.cbw[4]) == Tag);
    CHECK(Get32(&pipe.cbw[8]) == 4096);
    CHECK(pipe.cbw[12] == 0x80);
    CHECK(pipe.cbw[13] == 0);
    CHECK(pipe.cbw[14] == 10);
    CHECK(pipe.cbw[15] == 0x28);

    ModelPipe other;
    Execute(other, Tag + 1, WriteCommand());
    CHECK(Get32(&other.cbw[4]) == Tag + 1);
    CHECK(other.cbw[12] == 0x00);
    CHECK(other.cbw[13] == 0);
}

TEST_CASE("bot T10: a CSW is checked field by field", "[usb][storage][bot]")
{
    uint32_t residue = 0xDEADBEEF;
    auto bytes = Csw(Tag, 0, 0);
    CHECK(CheckCsw(std::span<const uint8_t>(bytes), CswSize, Tag, 512, &residue) ==
          CswCheck::Passed);
    CHECK(residue == 0);

    bytes = Csw(Tag, 100, 1);
    CHECK(CheckCsw(std::span<const uint8_t>(bytes), CswSize, Tag, 512, &residue) ==
          CswCheck::Failed);
    CHECK(residue == 100);

    bytes = Csw(Tag, 512, 2);
    CHECK(CheckCsw(std::span<const uint8_t>(bytes), CswSize, Tag, 512, &residue) ==
          CswCheck::PhaseError);
    CHECK(residue == 512);

    residue = 0xDEADBEEF;
    bytes = Csw(Tag, 0, 0);
    bytes[0] ^= 1;
    CHECK(Check(bytes, CswSize) == CswCheck::Invalid);
    bytes = Csw(Tag, 0, 0);
    bytes[3] ^= 0x80;
    CHECK(Check(bytes, CswSize) == CswCheck::Invalid);

    CHECK(Check(Csw(Tag + 1, 0, 0), CswSize) == CswCheck::Invalid);
    CHECK(Check(Csw(Tag ^ 0x80000000, 0, 0), CswSize) == CswCheck::Invalid);
    CHECK(Check(Csw(0, 0, 0), CswSize) == CswCheck::Invalid);

    CHECK(Check(Csw(Tag, 0, 0), CswSize - 1) == CswCheck::Invalid);
    CHECK(Check(Csw(Tag, 0, 0), 0) == CswCheck::Invalid);
    CHECK(Check(Csw(Tag, 0, 0), CswSize + 1) == CswCheck::Invalid);
    auto longer = Csw(Tag, 0, 0);
    longer.push_back(0);
    CHECK(Check(longer, CswSize + 1) == CswCheck::Invalid);
    auto shorter = Csw(Tag, 0, 0);
    shorter.pop_back();
    CHECK(Check(shorter, CswSize) == CswCheck::Invalid);

    CHECK(Check(Csw(Tag, 0, 3), CswSize) == CswCheck::Invalid);
    CHECK(Check(Csw(Tag, 0, 0xFF), CswSize) == CswCheck::Invalid);
    CHECK(Check(Csw(Tag, 513, 0), CswSize) == CswCheck::Invalid);
    CHECK(Check(Csw(Tag, 513, 1), CswSize) == CswCheck::Invalid);
    CHECK(Check(Csw(Tag, 513, 2), CswSize) == CswCheck::Invalid);
    CHECK(Check(Csw(Tag, 1, 0), CswSize, 0) == CswCheck::Invalid);
    CHECK(Check(Csw(Tag, 512, 0), CswSize) == CswCheck::Passed);
    CHECK(Check(Csw(Tag, 0, 0), CswSize, 0) == CswCheck::Passed);
}

TEST_CASE("bot T11 case 1: Hn = Dn passes", "[usb][storage][bot]")
{
    ModelPipe pipe;
    const Result result = Execute(pipe, Tag, NoData());
    CHECK(result.outcome == Outcome::Passed);
    CHECK(result.actual == 0);
    CHECK(result.residue == 0);
    CHECK(!result.recovered);
    CHECK(pipe.log == Plain);
}

TEST_CASE("bot T11 cases 2 and 3: Hn < Di and Hn < Do end in reset recovery",
          "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.csw_code = 2;
    const Result result = Execute(pipe, Tag, NoData());
    CHECK(result.outcome == Outcome::TransportFailed);
    CHECK(result.recovered);
    CHECK(pipe.log == With(Plain, "recover"));
}

TEST_CASE("bot T11 case 4: Hi > Dn, no data or a stall, then the CSW", "[usb][storage][bot]")
{
    SECTION("a stall, then a passed CSW with the whole residue")
    {
        ModelPipe pipe;
        pipe.data_status = Status::Stall;
        pipe.csw_residue = 36;
        const Result result = Execute(pipe, Tag, Inquiry36());
        CHECK(result.outcome == Outcome::Passed);
        CHECK(result.actual == 0);
        CHECK(result.residue == 36);
        CHECK(!result.recovered);
        CHECK(pipe.log == PlainIn);
    }
    SECTION("a READ is not a success")
    {
        ModelPipe pipe;
        pipe.data_status = Status::Stall;
        pipe.csw_residue = 4096;
        const Result result = Execute(pipe, Tag, ReadCommand());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(!result.recovered);
        CHECK(pipe.log == PlainIn);
    }
    SECTION("a failed CSW asks for REQUEST SENSE")
    {
        ModelPipe pipe;
        pipe.data_status = Status::Stall;
        pipe.csw_code = 1;
        pipe.csw_residue = 4096;
        const Result result = Execute(pipe, Tag, ReadCommand());
        CHECK(result.outcome == Outcome::CheckCondition);
        CHECK(result.residue == 4096);
        CHECK(!result.recovered);
        CHECK(pipe.log == PlainIn);
    }
}

TEST_CASE("bot T11 case 5: Hi > Di, short data, residue = difference", "[usb][storage][bot]")
{
    SECTION("INQUIRY-like commands use length minus residue")
    {
        ModelPipe pipe;
        pipe.data_status = Status::ShortPacket;
        pipe.data_actual = 36;
        pipe.csw_residue = 156;
        Command command{Scsi::ModeSense6(0x3F, 192), Direction::In, 192, DataTimeoutMs};
        const Result result = Execute(pipe, Tag, command);
        CHECK(result.outcome == Outcome::Passed);
        CHECK(result.actual == 36);
        CHECK(result.residue == 156);
        CHECK(pipe.log == PlainIn);
    }
    SECTION("the actual count never exceeds what arrived")
    {
        ModelPipe pipe;
        pipe.data_status = Status::ShortPacket;
        pipe.data_actual = 30;
        pipe.csw_residue = 156;
        Command command{Scsi::ModeSense6(0x3F, 192), Direction::In, 192, DataTimeoutMs};
        CHECK(Execute(pipe, Tag, command).actual == 30);
    }
    SECTION("a stall with a residue")
    {
        ModelPipe pipe;
        pipe.data_status = Status::Stall;
        pipe.data_actual = 4;
        pipe.csw_residue = 32;
        const Result result = Execute(pipe, Tag, Inquiry36());
        CHECK(result.outcome == Outcome::Passed);
        CHECK(result.actual == 4);
        CHECK(pipe.log == PlainIn);
    }
    SECTION("a short READ fails without recovery")
    {
        ModelPipe pipe;
        pipe.data_status = Status::ShortPacket;
        pipe.data_actual = 2048;
        pipe.csw_residue = 2048;
        const Result result = Execute(pipe, Tag, ReadCommand());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(!result.recovered);
        CHECK(pipe.log == PlainIn);
    }
    SECTION("a READ whose CSW claims full data but arrived short fails")
    {
        ModelPipe pipe;
        pipe.data_status = Status::ShortPacket;
        pipe.data_actual = 2048;
        const Result result = Execute(pipe, Tag, ReadCommand());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(!result.recovered);
    }
}

TEST_CASE("bot T11 case 6: Hi = Di passes", "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.data_actual = 4096;
    const Result result = Execute(pipe, Tag, ReadCommand());
    CHECK(result.outcome == Outcome::Passed);
    CHECK(result.actual == 4096);
    CHECK(result.residue == 0);
    CHECK(!result.recovered);
    CHECK(pipe.log == PlainIn);
    CHECK(pipe.lengths == std::vector<uint32_t>{CbwSize, 4096, CswSize});
}

TEST_CASE("bot T11 case 7: Hi < Di, Hi bytes then a phase error", "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.data_actual = 36;
    pipe.csw_code = 2;
    pipe.csw_residue = 0;
    const Result result = Execute(pipe, Tag, Inquiry36());
    CHECK(result.outcome == Outcome::TransportFailed);
    CHECK(result.recovered);
    CHECK(pipe.log == With(PlainIn, "recover"));
}

TEST_CASE("bot T11 case 8: Hi != Do, a stall on IN then a phase error", "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.data_status = Status::Stall;
    pipe.csw_code = 2;
    const Result result = Execute(pipe, Tag, Inquiry36());
    CHECK(result.outcome == Outcome::TransportFailed);
    CHECK(result.recovered);
    CHECK(pipe.log == With(PlainIn, "recover"));
}

TEST_CASE("bot T11 case 9: Ho > Dn, a stall on OUT, then the CSW", "[usb][storage][bot]")
{
    SECTION("a WRITE fails without recovery")
    {
        ModelPipe pipe;
        pipe.data_status = Status::Stall;
        pipe.data_actual = 0;
        pipe.csw_residue = 4096;
        const Result result = Execute(pipe, Tag, WriteCommand());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(!result.recovered);
        CHECK(pipe.log == PlainOut);
    }
    SECTION("another command uses length minus residue")
    {
        ModelPipe pipe;
        pipe.data_status = Status::Stall;
        pipe.csw_residue = 24;
        const Result result = Execute(pipe, Tag, OutNoWrite(24));
        CHECK(result.outcome == Outcome::Passed);
        CHECK(result.actual == 0);
        CHECK(pipe.log == PlainOut);
    }
}

TEST_CASE("bot T11 case 10: Ho != Di ends in reset recovery", "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.data_actual = 4096;
    pipe.csw_code = 2;
    const Result result = Execute(pipe, Tag, WriteCommand());
    CHECK(result.outcome == Outcome::TransportFailed);
    CHECK(result.recovered);
    CHECK(pipe.log == With(PlainOut, "recover"));
}

TEST_CASE("bot T11 case 11: Ho > Do, accepted with a residue", "[usb][storage][bot]")
{
    SECTION("a WRITE fails unless the residue is 0")
    {
        ModelPipe pipe;
        pipe.data_actual = 4096;
        pipe.csw_residue = 512;
        const Result result = Execute(pipe, Tag, WriteCommand());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(result.residue == 512);
        CHECK(!result.recovered);
        CHECK(pipe.log == PlainOut);
    }
    SECTION("a stall on OUT")
    {
        ModelPipe pipe;
        pipe.data_status = Status::Stall;
        pipe.data_actual = 2048;
        pipe.csw_residue = 2048;
        const Result result = Execute(pipe, Tag, WriteCommand());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(!result.recovered);
    }
}

TEST_CASE("bot T11 case 12: Ho = Do passes", "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.data_actual = 4096;
    const Result result = Execute(pipe, Tag, WriteCommand());
    CHECK(result.outcome == Outcome::Passed);
    CHECK(result.actual == 4096);
    CHECK(result.residue == 0);
    CHECK(!result.recovered);
    CHECK(pipe.log == PlainOut);
}

TEST_CASE("bot T11 case 13: Ho < Do ends in reset recovery", "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.data_actual = 512;
    pipe.csw_code = 2;
    pipe.csw_residue = 3584;
    const Result result = Execute(pipe, Tag, WriteCommand());
    CHECK(result.outcome == Outcome::TransportFailed);
    CHECK(result.recovered);
    CHECK(pipe.log == With(PlainOut, "recover"));
}

TEST_CASE("bot T11: a CBW that is not a Success ends in reset recovery", "[usb][storage][bot]")
{
    for (Status status : {Status::Stall, Status::TransactionError, Status::Babble, Status::Timeout,
                          Status::Cancelled, Status::ControllerError, Status::ShortPacket})
    {
        ModelPipe pipe;
        pipe.cbw_status = status;
        const Result result = Execute(pipe, Tag, ReadCommand());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(result.recovered);
        CHECK(pipe.log == Log{"start:command", "finish:command", "recover"});
    }
}

TEST_CASE("bot T11: a CBW that cannot be started", "[usb][storage][bot]")
{
    SECTION("a gone device is reported without recovery")
    {
        ModelPipe pipe;
        pipe.fail_start = true;
        const Result result = Execute(pipe, Tag, ReadCommand());
        CHECK(result.outcome == Outcome::NoDevice);
        CHECK(!result.recovered);
        CHECK(pipe.log == Log{"start:command"});
    }
    SECTION("another start failure ends in reset recovery")
    {
        ModelPipe pipe;
        pipe.fail_start = true;
        pipe.fail_start_status = Status::ControllerError;
        const Result result = Execute(pipe, Tag, ReadCommand());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(result.recovered);
        CHECK(pipe.log == Log{"start:command", "recover"});
    }
}

TEST_CASE("bot T11: a data error other than a stall ends in reset recovery",
          "[usb][storage][bot]")
{
    for (Status status : {Status::TransactionError, Status::Babble, Status::Timeout,
                          Status::Cancelled, Status::ControllerError})
    {
        for (const Command& command : {ReadCommand(), WriteCommand()})
        {
            ModelPipe pipe;
            pipe.data_status = status;
            const Result result = Execute(pipe, Tag, command);
            CHECK(result.outcome == Outcome::TransportFailed);
            CHECK(result.recovered);
            const char* data = command.direction == Direction::In ? "data-in" : "data-out";
            CHECK(pipe.log == Log{"start:command", "finish:command", std::string("start:") + data,
                                  std::string("finish:") + data, "recover"});
        }
    }
}

TEST_CASE("bot T11: a vanished device during data needs no recovery", "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.data_status = Status::NoDevice;
    const Result result = Execute(pipe, Tag, ReadCommand());
    CHECK(result.outcome == Outcome::NoDevice);
    CHECK(!result.recovered);
    CHECK(pipe.log == Log{"start:command", "finish:command", "start:data-in", "finish:data-in"});
}

TEST_CASE("bot T11: a CSW stall is read once more", "[usb][storage][bot]")
{
    SECTION("the second read succeeds")
    {
        ModelPipe pipe;
        pipe.csw_status = {Status::Stall, Status::Success};
        const Result result = Execute(pipe, Tag, NoData());
        CHECK(result.outcome == Outcome::Passed);
        CHECK(!result.recovered);
        CHECK(pipe.log == Log{"start:command", "finish:command", "start:status", "finish:status",
                              "start:status", "finish:status"});
    }
    SECTION("a second stall ends in reset recovery")
    {
        ModelPipe pipe;
        pipe.csw_status = {Status::Stall};
        const Result result = Execute(pipe, Tag, NoData());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(result.recovered);
        CHECK(pipe.log == Log{"start:command", "finish:command", "start:status", "finish:status",
                              "start:status", "finish:status", "recover"});
    }
    SECTION("a data stall and a CSW stall")
    {
        ModelPipe pipe;
        pipe.data_status = Status::Stall;
        pipe.csw_status = {Status::Stall, Status::Success};
        pipe.csw_residue = 36;
        const Result result = Execute(pipe, Tag, Inquiry36());
        CHECK(result.outcome == Outcome::Passed);
        CHECK(pipe.log.size() == 8);
    }
}

TEST_CASE("bot T11: a CSW error other than a stall ends in reset recovery", "[usb][storage][bot]")
{
    for (Status status : {Status::TransactionError, Status::Babble, Status::Timeout,
                          Status::Cancelled, Status::ControllerError})
    {
        ModelPipe pipe;
        pipe.csw_status = {status};
        const Result result = Execute(pipe, Tag, NoData());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(result.recovered);
        CHECK(pipe.log == With(Plain, "recover"));
    }
}

TEST_CASE("bot T11: an invalid CSW ends in reset recovery", "[usb][storage][bot]")
{
    const std::vector<std::function<void(ModelPipe&)>> breakers{
        [](ModelPipe& pipe)
        { pipe.shape_csw = [](std::span<uint8_t> csw, uint32_t) { csw[0] ^= 0xFF; }; },
        [](ModelPipe& pipe)
        {
            pipe.shape_csw = [](std::span<uint8_t> csw, uint32_t tag)
            { Put32(&csw[4], tag + 1); };
        },
        [](ModelPipe& pipe) { pipe.csw_actual = CswSize - 1; },
        [](ModelPipe& pipe)
        {
            pipe.csw_actual = CswSize - 1;
            pipe.csw_status = {Status::ShortPacket};
        },
        [](ModelPipe& pipe) { pipe.csw_actual = CswSize + 1; },
        [](ModelPipe& pipe) { pipe.csw_status = {Status::Babble}; },
        [](ModelPipe& pipe) { pipe.csw_code = 3; },
        [](ModelPipe& pipe) { pipe.csw_residue = 37; },
    };
    for (const auto& breaker : breakers)
    {
        ModelPipe pipe;
        pipe.data_actual = 36;
        breaker(pipe);
        const Result result = Execute(pipe, Tag, Inquiry36());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(result.recovered);
        REQUIRE(pipe.log.size() >= 2);
        CHECK(pipe.log.back() == "recover");
        CHECK(std::count(pipe.log.begin(), pipe.log.end(), "recover") == 1);
    }
}

TEST_CASE("bot T11: nothing is sent between a failure and the recovery", "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.csw_code = 2;
    Execute(pipe, Tag, ReadCommand());
    REQUIRE(pipe.log.size() >= 2);
    CHECK(pipe.log[pipe.log.size() - 2] == "finish:status");
    CHECK(pipe.log.back() == "recover");
    CHECK(pipe.deadlines.size() == 3);
}

TEST_CASE("bot T11: a failed recovery is reported", "[usb][storage][bot]")
{
    SECTION("the device is gone")
    {
        ModelPipe pipe;
        pipe.csw_code = 2;
        pipe.recover_status = Status::NoDevice;
        const Result result = Execute(pipe, Tag, NoData());
        CHECK(result.outcome == Outcome::NoDevice);
        CHECK(result.recovered);
    }
    SECTION("any other failure is a transport failure")
    {
        ModelPipe pipe;
        pipe.csw_code = 2;
        pipe.recover_status = Status::Timeout;
        const Result result = Execute(pipe, Tag, NoData());
        CHECK(result.outcome == Outcome::TransportFailed);
        CHECK(result.recovered);
        CHECK(std::count(pipe.log.begin(), pipe.log.end(), "recover") == 1);
    }
}

TEST_CASE("bot T11: CHECK CONDITION is returned without recovery", "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.csw_code = 1;
    const Result result = Execute(pipe, Tag, NoData());
    CHECK(result.outcome == Outcome::CheckCondition);
    CHECK(!result.recovered);
    CHECK(pipe.log == Plain);
}

TEST_CASE("bot T11: every stage gets its own deadline", "[usb][storage][bot]")
{
    ModelPipe pipe;
    pipe.now = 50000;
    pipe.data_actual = 4096;
    Command command = ReadCommand();
    command.timeout_ms = SlowCommandTimeoutMs;
    Execute(pipe, Tag, command);
    REQUIRE(pipe.deadlines.size() == 3);
    CHECK(pipe.deadlines[0] == 50000 + CommandTimeoutMs);
    CHECK(pipe.deadlines[1] == 50000 + SlowCommandTimeoutMs);
    CHECK(pipe.deadlines[2] == 50000 + SlowCommandTimeoutMs);
}

TEST_CASE("bot T11: a command with a length but no direction sends no data stage",
          "[usb][storage][bot]")
{
    ModelPipe pipe;
    Command command = NoData();
    command.length = 512;
    const Result result = Execute(pipe, Tag, command);
    CHECK(result.outcome == Outcome::Passed);
    CHECK(pipe.log == Plain);
    CHECK(Get32(&pipe.cbw[8]) == 0);
}

} // namespace
