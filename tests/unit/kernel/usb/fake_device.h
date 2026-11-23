#ifndef FEATHEROS_TESTS_USB_FAKE_DEVICE_H
#define FEATHEROS_TESTS_USB_FAKE_DEVICE_H

#include "kernel/usb/host_controller.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace FeatherOS::Usb::Testing
{

// A device behind a root port: it answers control requests from its descriptors, hands out
// queued reports on interrupt IN endpoints, and fails on demand.
class ScriptedDevice
{
public:
    enum class Outcome
    {
        Ok,
        Stall,
        Hang,
        TransactionError,
        Babble,
    };

    struct Request
    {
        uint8_t type;
        uint8_t request;
        uint16_t value;
        uint16_t index;
        uint16_t length;
    };

    ScriptedDevice(std::vector<uint8_t> device, std::vector<uint8_t> configuration);

    void AddString(uint8_t index, const std::string& text);
    void SetReportDescriptor(uint8_t interface, std::vector<uint8_t> descriptor);
    void RecordData(const Request& request, std::vector<uint8_t> data);
    // The data stages of the control requests of one request code, in order.
    std::vector<std::vector<uint8_t>> DataOf(uint8_t request) const;
    // Descriptor length 0: bMaxPacketSize0 as bytes; a SuperSpeed device writes the exponent.
    uint32_t MaxPacket0(Speed speed) const;

    virtual ~ScriptedDevice() = default;
    ScriptedDevice(const ScriptedDevice&) = delete;
    ScriptedDevice& operator=(const ScriptedDevice&) = delete;

    Outcome Control(const Request& request, std::vector<uint8_t>* response);
    // The next count requests (of one request code, or any) end this way.
    void FailControl(Outcome outcome, int count, int request = -1);

    void QueueReport(uint8_t endpoint, std::vector<uint8_t> report);
    virtual bool TakeReport(uint8_t endpoint, std::vector<uint8_t>* report);
    virtual bool HasReport(uint8_t endpoint) const;
    void FailEndpoint(uint8_t endpoint, Outcome outcome, int count);
    Outcome EndpointOutcome(uint8_t endpoint);

    // Bulk OUT data handed to the device, one entry per call; the default keeps it.
    virtual void ReceiveBulk(uint8_t endpoint, std::span<const uint8_t> data);
    // Up to max bytes for a bulk IN TD; the default serves the queued reports, Hang when there
    // is none (the device answers NAK).
    virtual Outcome ProduceBulk(uint8_t endpoint, uint32_t max, std::vector<uint8_t>* out);
    const std::vector<std::vector<uint8_t>>& BulkReceived(uint8_t endpoint) const;
    // The data toggle the device expects (OUT) or sends next (IN); CLEAR_FEATURE(ENDPOINT_HALT)
    // resets it.
    bool Toggle(uint8_t endpoint) const;
    void FlipToggle(uint8_t endpoint);

    // Called as each control request arrives, before it is answered.
    std::function<void(const Request&)> on_request;

    const std::vector<Request>& Requests() const;
    uint32_t Count(uint8_t type, uint8_t request) const;
    uint8_t configuration() const;

protected:
    // Class requests a subclass answers; returns false for the ones it leaves to the base.
    virtual bool HandleClass(const Request& request, std::vector<uint8_t>* response,
                             Outcome* outcome);
    // SET_CONFIGURATION arrived: what a port reset and a new enumeration also reset.
    virtual void Configured();

private:
    struct Fault
    {
        Outcome outcome;
        int remaining;
        int request;
    };

    std::vector<uint8_t> device_;
    std::vector<uint8_t> configuration_;
    std::map<uint8_t, std::vector<uint8_t>> strings_;
    std::map<uint8_t, std::vector<uint8_t>> report_descriptors_;
    std::vector<std::pair<Request, std::vector<uint8_t>>> data_;
    std::vector<Request> requests_;
    std::vector<Fault> control_faults_;
    std::map<uint8_t, std::deque<std::vector<uint8_t>>> reports_;
    std::map<uint8_t, std::vector<Fault>> endpoint_faults_;
    std::map<uint8_t, std::vector<std::vector<uint8_t>>> bulk_out_;
    std::map<uint8_t, bool> toggles_;
    uint8_t configuration_value_ = 0;
};

// A bulk-only mass-storage device over a model disk image. Faults apply to the next commands, in
// order; a stage fault is armed for the stage of whichever command comes next.
class ScriptedMassStorage : public ScriptedDevice
{
public:
    struct Model
    {
        uint32_t block_size = 512;
        uint64_t blocks = 2048;
        bool removable = true;
        bool write_protected = false;
        uint8_t type = 0;
        std::string vendor = "QEMU";
        std::string product = "QEMU HARDDISK";
    };

    enum class Stage
    {
        Command,
        Data,
        Status,
    };

    enum class Fault
    {
        Stall,
        Hang,
        TransactionError,
        // on_unplug runs as the stage starts; a data or status IN then never completes.
        Unplug,
    };

    enum class Reply
    {
        PhaseError,
        ShortData,
        BadSignature,
        BadTag,
        ResidueLies,
    };

    struct Logged
    {
        uint8_t opcode;
        uint64_t lba;
        uint32_t blocks;
        uint32_t tag;
        // START STOP UNIT's byte 4.
        uint8_t flags = 0;
    };

    ScriptedMassStorage(Speed speed, Model model);

    std::vector<uint8_t>& Image();
    const Model& Params() const;

    void FailStage(Stage stage, Fault fault, int count = 1);
    void FailReply(Reply reply, int count = 1);
    // The next commands but INQUIRY and REQUEST SENSE: NOT READY, becoming ready.
    void NotReady(int count);
    void UnitAttention(uint8_t asc, int count = 1);
    // The next commands but INQUIRY and REQUEST SENSE: ABORTED COMMAND.
    void AbortedCommand(int count);
    // The switch flipped after the probe: WRITE answers DATA PROTECT, MODE SENSE the new state.
    void WriteProtect(bool protect);
    void NoMedium(bool missing);
    void SynchronizeCacheUnsupported(bool unsupported);
    // SYNCHRONIZE CACHE answers DATA PROTECT, as a drive whose switch was flipped may.
    void SynchronizeCacheDataProtect(bool protect);
    void ReportBlocks(uint64_t blocks);
    // GET MAX LUN answers this, or stalls.
    void SetMaxLun(uint8_t lun, bool stall = false);

    std::function<void()> on_unplug;

    const std::vector<Logged>& Commands() const;
    uint32_t BotResets() const;

    void ReceiveBulk(uint8_t endpoint, std::span<const uint8_t> data) override;
    Outcome ProduceBulk(uint8_t endpoint, uint32_t max, std::vector<uint8_t>* out) override;

protected:
    bool HandleClass(const Request& request, std::vector<uint8_t>* response,
                     Outcome* outcome) override;
    void Configured() override;

private:
    enum class Phase
    {
        Command,
        DataOut,
    };

    struct Item
    {
        bool status;
        std::vector<uint8_t> bytes;
        // A failed IN command without data: the data stage stalls, then the CSW follows.
        bool stall = false;
    };

    struct Cbw
    {
        uint32_t tag;
        uint32_t length;
        bool in;
        uint8_t cdb[16];
    };

    struct Answer
    {
        uint8_t status = 0;
        std::vector<uint8_t> data;
        uint32_t residue = 0;
        bool apply_write = false;
        uint64_t lba = 0;
        uint32_t blocks = 0;
    };

    bool TakeStageFault(Stage stage, Fault* fault);
    bool TakeReply(Reply* reply);
    void Execute(const Cbw& cbw);
    Answer Check(const Cbw& cbw);
    void CheckCondition(Answer* answer, const Cbw& cbw, uint8_t key, uint8_t asc, uint8_t ascq);
    void QueueStatus(const Cbw& cbw, const Answer& answer);
    std::vector<uint8_t> Csw(uint32_t tag, uint8_t status, uint32_t residue);

    Model model_;
    std::vector<uint8_t> image_;
    std::map<Stage, std::deque<Fault>> stage_faults_;
    std::deque<Reply> replies_;
    int not_ready_ = 0;
    int aborted_ = 0;
    std::deque<uint8_t> unit_attentions_;
    bool no_medium_ = false;
    bool sync_unsupported_ = false;
    bool sync_data_protect_ = false;
    bool ejected_ = false;
    uint64_t reported_blocks_ = 0;
    uint8_t max_lun_ = 0;
    bool stall_max_lun_ = false;
    uint8_t sense_[3] = {0, 0, 0};
    std::vector<Logged> commands_;
    uint32_t resets_ = 0;
    Phase phase_ = Phase::Command;
    Cbw pending_cbw_{};
    Answer pending_answer_;
    std::vector<uint8_t> received_;
    std::deque<Item> in_queue_;
    bool hung_ = false;
};

std::vector<uint8_t> MakeDeviceDescriptor(uint16_t vendor, uint16_t product, uint8_t max_packet0,
                                           uint8_t device_class = 0, uint8_t product_string = 2,
                                           uint16_t usb_version = 0x0200);
std::vector<uint8_t> KeyboardConfiguration();
std::vector<uint8_t> MouseConfiguration();
std::vector<uint8_t> ComboConfiguration();
// One HID interface that is not a boot interface (subclass 0).
std::vector<uint8_t> GenericKeyboardConfiguration(uint16_t report_length);
// The same for a mouse, with eight-byte reports.
std::vector<uint8_t> GenericMouseConfiguration(uint16_t report_length, bool boot_interface = false);
std::vector<uint8_t> HubConfiguration();
std::vector<uint8_t> SuperSpeedHubConfiguration();
std::vector<uint8_t> MassStorageConfiguration();

// bMaxPacketSize0 a device of this speed reports (the exponent at SuperSpeed).
uint8_t MaxPacket0Field(Speed speed);

// The report descriptors of QEMU's boot keyboard and boot mouse.
const std::vector<uint8_t>& BootKeyboardDescriptor();
const std::vector<uint8_t>& BootMouseDescriptor();

// A hub: its ports hold devices that are plugged in and out, and a port reports changes the way a
// hub does (status and change words, C_PORT_RESET after a reset). The hub's status change
// endpoint 0x81 has data while any change bit is set.
class ScriptedHub : public ScriptedDevice
{
public:
    static constexpr uint16_t StatusConnection = 0x0001;
    static constexpr uint16_t StatusEnable = 0x0002;
    static constexpr uint16_t StatusOverCurrent = 0x0008;
    static constexpr uint16_t StatusReset = 0x0010;

    ScriptedHub(bool super_speed, uint8_t ports, std::vector<uint8_t> device,
                std::vector<uint8_t> configuration, uint8_t think_time = 1);

    void Plug(uint8_t port, std::shared_ptr<ScriptedDevice> child, Speed speed);
    void Unplug(uint8_t port);
    void OverCurrent(uint8_t port);
    // Raises the connection change without a change of state, as a flapping connector does.
    void Flap(uint8_t port);
    // A reset on this port never completes.
    void HoldReset(uint8_t port, bool hold);
    // The port enables but reports no reset change.
    void SkipResetChange(uint8_t port, bool skip);
    std::shared_ptr<ScriptedDevice> Child(uint8_t port) const;
    uint8_t Ports() const;
    bool SuperSpeed() const;

    uint16_t Status(uint8_t port) const;
    uint16_t Change(uint8_t port) const;
    bool Powered(uint8_t port) const;
    uint32_t Resets(uint8_t port) const;
    // The feature selectors of every SET_FEATURE and CLEAR_FEATURE sent to a port, in order.
    const std::vector<std::pair<uint8_t, uint8_t>>& PortSets() const;
    const std::vector<std::pair<uint8_t, uint8_t>>& PortClears() const;
    // wValue of every SET_HUB_DEPTH, and the alternate settings selected.
    const std::vector<uint16_t>& Depths() const;
    const std::vector<uint16_t>& Alternates() const;
    // The ports in the order resets were requested.
    const std::vector<uint8_t>& ResetOrder() const;

    bool HasReport(uint8_t endpoint) const override;
    bool TakeReport(uint8_t endpoint, std::vector<uint8_t>* report) override;

protected:
    bool HandleClass(const Request& request, std::vector<uint8_t>* response,
                     Outcome* outcome) override;

private:
    struct Port
    {
        uint16_t status = 0;
        uint16_t change = 0;
        bool powered = false;
        bool hold_reset = false;
        bool skip_reset_change = false;
        uint32_t resets = 0;
        Speed speed = Speed::Full;
        std::shared_ptr<ScriptedDevice> child;
    };

    uint16_t SpeedBits(Speed speed) const;
    std::vector<uint8_t> Descriptor() const;

    bool super_speed_;
    uint8_t think_time_;
    std::vector<Port> ports_;
    std::vector<std::pair<uint8_t, uint8_t>> sets_;
    std::vector<std::pair<uint8_t, uint8_t>> clears_;
    std::vector<uint16_t> depths_;
    std::vector<uint16_t> alternates_;
    std::vector<uint8_t> reset_order_;
};

// speed is Full, High or Super; a high-speed hub with multi_tt reports protocol 2.
std::shared_ptr<ScriptedHub> MakeHub(Speed speed, uint8_t ports, bool multi_tt = false,
                                     uint8_t think_time = 1);

// A bulk-only mass storage device: IN 0x81 and OUT 0x02.
std::shared_ptr<ScriptedDevice> MakeMassStorage(Speed speed);
std::shared_ptr<ScriptedMassStorage> MakeDisk(Speed speed,
                                              ScriptedMassStorage::Model model = {});
std::shared_ptr<ScriptedDevice> MakeKeyboard(Speed speed, uint8_t max_packet0 = 0);
std::shared_ptr<ScriptedDevice> MakeMouse(Speed speed, uint8_t max_packet0 = 0);
// A report-protocol mouse: buttons, X, Y, wheel and AC Pan, one byte each after the buttons.
std::shared_ptr<ScriptedDevice> MakeWheelMouse(Speed speed, bool boot_interface = false);

} // namespace FeatherOS::Usb::Testing

#endif
