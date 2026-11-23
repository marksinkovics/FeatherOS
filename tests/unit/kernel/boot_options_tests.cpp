#include <catch2/catch_test_macros.hpp>

#include "kernel/boot_options.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/kernel_test_mode.h"

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
    REQUIRE(ParseRoot("root=vda16").partition == 16);
    REQUIRE(ParseRoot("root=vda9999").request == RootRequest::Partition);
}

TEST_CASE("root= can sit among other options")
{
    REQUIRE(ParseRoot("  quiet   root=vda3  test=shell ").partition == 3);
    REQUIRE(ParseRoot("test=storage root=vda2").partition == 2);
    // The last one wins.
    REQUIRE(ParseRoot("root=vda1 root=vda5").partition == 5);
    REQUIRE(ParseRoot("root=vda1 root=sda1").request == RootRequest::Malformed);
    // Words that only end or start like it are other options.
    REQUIRE(ParseRoot("chroot=vda4 xroot=vda4 rootfs=vda4").request == RootRequest::Absent);

    FeatherOS::KernelTest::Mode mode = FeatherOS::KernelTest::Mode::None;
    REQUIRE(FeatherOS::KernelTest::ParseCommandLine("root=vda4 test=storage", &mode));
    REQUIRE(mode == FeatherOS::KernelTest::Mode::Storage);
}

TEST_CASE("a root= that is not a vda partition is malformed")
{
    for (const char* text : {"root=", "root=vda", "root=vda0", "root=vda01", "root=vda4x",
                             "root=vda-1", "root=vda10000", "root=vda99999999999999999999",
                             "root=sda1", "root=/dev/vda1", "root=VDA1", "root=vd"})
    {
        INFO(text);
        const auto root = ParseRoot(text);
        REQUIRE(root.request == RootRequest::Malformed);
        REQUIRE(root.partition == 0);
    }
}

TEST_CASE("boot options selection")
{
    FeatherOS::BootOptions::SelectRoot({RootRequest::Partition, 4});
    REQUIRE(FeatherOS::BootOptions::SelectedRoot().partition == 4);
    FeatherOS::BootOptions::SelectRoot({RootRequest::Absent, 0});
    REQUIRE(FeatherOS::BootOptions::SelectedRoot().request == RootRequest::Absent);
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
