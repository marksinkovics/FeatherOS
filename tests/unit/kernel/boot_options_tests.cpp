#include <catch2/catch_test_macros.hpp>

#include "kernel/boot_options.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/kernel_test_mode.h"

#include <string>

using FeatherOS::BootOptions::ParseRoot;
using FeatherOS::BootOptions::RootRequest;

TEST_CASE("root= names the root partition")
{
    REQUIRE(ParseRoot(nullptr).request == RootRequest::Absent);
    REQUIRE(ParseRoot("").request == RootRequest::Absent);
    REQUIRE(ParseRoot("quiet test=storage").request == RootRequest::Absent);

    const auto root = ParseRoot("root=vda4");
    REQUIRE(root.request == RootRequest::Partition);
    REQUIRE(root.partition == 4);
    REQUIRE(std::string(root.disk) == "vda");
    REQUIRE(ParseRoot("root=vda16").partition == 16);
    REQUIRE(ParseRoot("root=vda9999").request == RootRequest::Partition);
}

TEST_CASE("root= can sit among other options")
{
    REQUIRE(ParseRoot("  quiet   root=vda3  test=shell ").partition == 3);
    REQUIRE(ParseRoot("test=storage root=vda2").partition == 2);
    // The last one wins.
    REQUIRE(ParseRoot("root=vda1 root=vda5").partition == 5);
    REQUIRE(ParseRoot("root=vda1 root=sda1").request == RootRequest::Partition);
    REQUIRE(std::string(ParseRoot("root=vda1 root=sda1").disk) == "sda");
    REQUIRE(ParseRoot("root=vda1 root=vda").request == RootRequest::Malformed);
    // Words that only end or start like it are other options.
    REQUIRE(ParseRoot("chroot=vda4 xroot=vda4 rootfs=vda4").request == RootRequest::Absent);

    FeatherOS::KernelTest::Mode mode = FeatherOS::KernelTest::Mode::None;
    REQUIRE(FeatherOS::KernelTest::ParseCommandLine("root=vda4 test=storage", &mode));
    REQUIRE(mode == FeatherOS::KernelTest::Mode::Storage);
}

TEST_CASE("T31: root= accepts vd, sd and nvme names with a partition number")
{
    struct Accepted
    {
        const char* text;
        const char* disk;
        size_t partition;
    };
    for (const Accepted& expected :
         {Accepted{"root=vda3", "vda", 3}, Accepted{"root=vdb1", "vdb", 1},
          Accepted{"root=sda2", "sda", 2}, Accepted{"root=sdz16", "sdz", 16},
          Accepted{"root=nvme0n1p2", "nvme0n1", 2}, Accepted{"root=nvme12n3p16", "nvme12n3", 16},
          Accepted{"root=nvme0n10p9999", "nvme0n10", 9999}})
    {
        INFO(expected.text);
        const auto root = ParseRoot(expected.text);
        REQUIRE(root.request == RootRequest::Partition);
        REQUIRE(std::string(root.disk) == expected.disk);
        REQUIRE(root.partition == expected.partition);
    }
}

TEST_CASE("T31: a root= that is not a disk name and a partition number is malformed")
{
    for (const char* text :
         {"root=", "root=vda", "root=vda0", "root=vda01", "root=vda4x", "root=vda-1",
          "root=vda10000", "root=vda99999999999999999999", "root=/dev/vda1", "root=VDA1",
          "root=vd", "root=vd1", "root=sd", "root=sda", "root=sda02", "root=sda2x", "root=sdaa1",
          "root=xyz1", "root=nvme", "root=nvme0", "root=nvme0n1", "root=nvme0n1p", "root=nvme0n1p0",
          "root=nvme0n1p01", "root=nvme0n1-2", "root=nvme0n1p2x", "root=nvme01n1p1",
          "root=nvme0n0p1", "root=nvme0n01p1", "root=nvme-1n1p1", "root=nvmen1p1",
          "root=nvme123456789012345n1p1", "root=nvme0n123456789012345p1", "root=nvme0p1",
          "root=hda1", "root=mmcblk0p1"})
    {
        INFO(text);
        const auto root = ParseRoot(text);
        REQUIRE(root.request == RootRequest::Malformed);
        REQUIRE(root.disk[0] == '\0');
        REQUIRE(root.partition == 0);
    }
}

TEST_CASE("T31: the last root= wins, and a vda one parses as before")
{
    REQUIRE(ParseRoot("root=nvme0n1p2 root=vda3").partition == 3);
    REQUIRE(std::string(ParseRoot("root=nvme0n1p2 root=vda3").disk) == "vda");
    REQUIRE(std::string(ParseRoot("root=vda3 root=nvme0n1p2").disk) == "nvme0n1");
    REQUIRE(ParseRoot("root=vda3 root=bad").request == RootRequest::Malformed);
    REQUIRE(ParseRoot("root=bad root=sda1").request == RootRequest::Partition);
    const auto root = ParseRoot("quiet root=vda3 test=storage");
    REQUIRE(root.request == RootRequest::Partition);
    REQUIRE(root.partition == 3);
    REQUIRE(std::string(root.disk) == "vda");
}

TEST_CASE("the root is the requested partition, else the lowest-numbered volume")
{
    using FeatherOS::VFS::ChooseRoot;
    using FeatherOS::VFS::NoRoot;
    const size_t volumes[] = {0, 3, 2, 4};
    REQUIRE(ChooseRoot(volumes, 4, 0) == 2);
    REQUIRE(ChooseRoot(volumes, 4, 4) == 3);
    REQUIRE(ChooseRoot(volumes, 4, 3) == 1);
    // A request for a partition that holds no volume chooses nothing; the caller then asks again.
    REQUIRE(ChooseRoot(volumes, 4, 1) == NoRoot);
    REQUIRE(ChooseRoot(volumes, 4, 9) == NoRoot);
    const size_t none[] = {0, 0, 0, 0};
    REQUIRE(ChooseRoot(none, 4, 0) == NoRoot);
}

TEST_CASE("usb= switches USB off or to polling")
{
    using FeatherOS::BootOptions::ParseUsb;
    using FeatherOS::BootOptions::UsbMode;
    REQUIRE(ParseUsb(nullptr) == UsbMode::Default);
    REQUIRE(ParseUsb("quiet test=usb") == UsbMode::Default);
    REQUIRE(ParseUsb("usb=off") == UsbMode::Off);
    REQUIRE(ParseUsb("root=vda3 usb=poll test=pci") == UsbMode::Poll);
    // The last one wins, and anything else counts as absent.
    REQUIRE(ParseUsb("usb=off usb=poll") == UsbMode::Poll);
    REQUIRE(ParseUsb("usb=poll usb=on") == UsbMode::Default);
    REQUIRE(ParseUsb("usb= usb=offx xusb=off usbhid=off") == UsbMode::Default);

    FeatherOS::BootOptions::SelectUsb(UsbMode::Poll);
    REQUIRE(FeatherOS::BootOptions::SelectedUsb() == UsbMode::Poll);
    FeatherOS::BootOptions::SelectUsb(UsbMode::Default);
    REQUIRE(FeatherOS::BootOptions::SelectedUsb() == UsbMode::Default);
}

TEST_CASE("usbhid=report and ps2=off")
{
    using FeatherOS::BootOptions::ParsePs2;
    using FeatherOS::BootOptions::ParseUsbHid;
    using FeatherOS::BootOptions::Ps2Mode;
    using FeatherOS::BootOptions::UsbHidMode;
    REQUIRE(ParseUsbHid(nullptr) == UsbHidMode::Default);
    REQUIRE(ParseUsbHid("quiet usb=off") == UsbHidMode::Default);
    REQUIRE(ParseUsbHid("usbhid=report") == UsbHidMode::Report);
    REQUIRE(ParseUsbHid("root=vda3 usbhid=report test=usb") == UsbHidMode::Report);
    REQUIRE(ParseUsbHid("usbhid=report usbhid=boot") == UsbHidMode::Default);
    REQUIRE(ParseUsbHid("usbhid= usbhid=reports xusbhid=report usb=report") == UsbHidMode::Default);
    REQUIRE(ParsePs2(nullptr) == Ps2Mode::Default);
    REQUIRE(ParsePs2("ps2=off") == Ps2Mode::Off);
    REQUIRE(ParsePs2("usb=off ps2=off test=input") == Ps2Mode::Off);
    REQUIRE(ParsePs2("ps2=off ps2=on") == Ps2Mode::Default);
    REQUIRE(ParsePs2("ps2= ps2=offx xps2=off") == Ps2Mode::Default);

    FeatherOS::BootOptions::SelectUsbHid(UsbHidMode::Report);
    REQUIRE(FeatherOS::BootOptions::SelectedUsbHid() == UsbHidMode::Report);
    FeatherOS::BootOptions::SelectUsbHid(UsbHidMode::Default);
    FeatherOS::BootOptions::SelectPs2(Ps2Mode::Off);
    REQUIRE(FeatherOS::BootOptions::SelectedPs2() == Ps2Mode::Off);
    FeatherOS::BootOptions::SelectPs2(Ps2Mode::Default);
}

TEST_CASE("storage= selects the NVMe and AHCI write mode")
{
    using FeatherOS::BootOptions::ParseStorage;
    using FeatherOS::BootOptions::StorageMode;
    REQUIRE(ParseStorage(nullptr) == StorageMode::Auto);
    REQUIRE(ParseStorage("") == StorageMode::Auto);
    REQUIRE(ParseStorage("quiet test=storage") == StorageMode::Auto);
    REQUIRE(ParseStorage("storage=ro") == StorageMode::ReadOnly);
    REQUIRE(ParseStorage("storage=rw") == StorageMode::ReadWrite);
    REQUIRE(ParseStorage("root=vda3 storage=rw test=pci") == StorageMode::ReadWrite);
    // The last one wins, and anything else counts as absent.
    REQUIRE(ParseStorage("storage=ro storage=rw") == StorageMode::ReadWrite);
    REQUIRE(ParseStorage("storage=rw storage=ro") == StorageMode::ReadOnly);
    REQUIRE(ParseStorage("storage=rw storage=bogus") == StorageMode::Auto);
    REQUIRE(ParseStorage("storage=ro storage=") == StorageMode::Auto);
    REQUIRE(ParseStorage("storage=bogus") == StorageMode::Auto);
    REQUIRE(ParseStorage("storage=ROx storage=rwx xstorage=ro test=storage") == StorageMode::Auto);

    FeatherOS::BootOptions::SelectStorage(StorageMode::ReadOnly);
    REQUIRE(FeatherOS::BootOptions::SelectedStorage() == StorageMode::ReadOnly);
    FeatherOS::BootOptions::SelectStorage(StorageMode::Auto);
    REQUIRE(FeatherOS::BootOptions::SelectedStorage() == StorageMode::Auto);
}
