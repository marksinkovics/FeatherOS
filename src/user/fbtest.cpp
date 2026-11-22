#include "command_line.h"

#include <errno.h>
#include <fcntl.h>
#include <featheros/framebuffer.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

#if defined(__x86_64__)
constexpr uintptr_t KernelAddress = 0xFFFF800000000000;
#elif defined(__aarch64__)
constexpr uintptr_t KernelAddress = 0xFFFF000000000000;
#endif

constexpr int ReportDescriptor = 3;
constexpr uint32_t ScreenWidth = 1024;
constexpr uint32_t ScreenHeight = 768;
constexpr char Help[] = "usage: fbtest\n"
                        "Checks /dev/fb0 on the console it runs on: acquiring and releasing, the\n"
                        "errors, reading events, and release on close, exit, kill and in the\n"
                        "background. The screen is blank while it runs.";

[[noreturn]] void Fail(const char* name)
{
    printf("fbtest: failed %s (errno %d)\n", name, errno);
    exit(1);
}

void Check(bool passed, const char* name)
{
    if (!passed)
    {
        Fail(name);
    }
}

void CheckFails(int result, int error, const char* name)
{
    const int saved = errno;
    if (result != -1 || saved != error)
    {
        errno = saved;
        Fail(name);
    }
}

int Present(int descriptor, const uint32_t* pixels, uint32_t stride, int32_t x, int32_t y,
            uint32_t width, uint32_t height, uint32_t flags = 0)
{
    const fb_present request = {pixels, stride, x, y, width, height, flags};
    return ioctl(descriptor, FB_PRESENT, &request);
}

int OpenFramebuffer(int flags = 0)
{
    return open("/dev/fb0", O_RDWR | flags);
}

void Pause()
{
    usleep(100000);
}

void OnSignal(int) {}

// The child reports one character on its descriptor 3.
void Report(char result)
{
    write(ReportDescriptor, &result, 1);
}

int RunChild(const char* mode)
{
    if (strcmp(mode, "pty") == 0)
    {
        const int descriptor = OpenFramebuffer();
        const int result = descriptor < 0 ? 0 : ioctl(descriptor, FB_ACQUIRE);
        if (result != -1 || errno != ENOTTY)
        {
            printf("fbtest pty: failed (errno %d)\n", errno);
            return 1;
        }
        puts("fbtest pty: ok");
        return 0;
    }
    if (strcmp(mode, "ignore") == 0)
    {
        signal(SIGTTOU, SIG_IGN);
    }
    if (strcmp(mode, "eintr") == 0)
    {
        struct sigaction action = {};
        action.sa_handler = OnSignal;
        sigaction(SIGUSR1, &action, nullptr);
    }
    const int descriptor = OpenFramebuffer(strcmp(mode, "nonblockopen") == 0 ? O_NONBLOCK : 0);
    if (descriptor < 0)
    {
        Report('x');
        return 1;
    }
    const int result = ioctl(descriptor, FB_ACQUIRE);
    if (strcmp(mode, "enotty") == 0)
    {
        Report(result == -1 && errno == ENOTTY ? 'n' : 'x');
    }
    else if (strcmp(mode, "busy") == 0)
    {
        Report(result == -1 && errno == EBUSY ? 'b' : 'x');
    }
    else if (strcmp(mode, "hold") == 0)
    {
        Report(result == 0 ? 'h' : 'x');
        for (;;)
        {
            sleep(1);
        }
    }
    else if (strcmp(mode, "eintr") == 0)
    {
        fb_event event;
        const bool drained = result == 0 && read(descriptor, &event, sizeof(event)) == 8;
        Report(drained ? 'r' : 'x');
        const ssize_t blocked = read(descriptor, &event, sizeof(event));
        Report(blocked == -1 && errno == EINTR ? 'i' : 'x');
    }
    else if (strcmp(mode, "nonblockopen") == 0)
    {
        fb_event event = {};
        const bool first = result == 0 && read(descriptor, &event, sizeof(event)) == 8 &&
                           event.kind == FB_EVENT_SHOWN && event.code == 0;
        const bool second = read(descriptor, &event, sizeof(event)) == -1 && errno == EAGAIN;
        Report(first && second ? 'o' : 'x');
    }
    else if (strcmp(mode, "exit") == 0 || strcmp(mode, "ignore") == 0 || strcmp(mode, "stop") == 0)
    {
        Report(result == 0 ? 'a' : 'x');
    }
    return 0;
}

struct Child
{
    pid_t pid = 0;
    int report = -1;
};

bool Start(const char* mode, short flags, Child* child)
{
    int ends[2];
    if (pipe(ends) != 0)
    {
        return false;
    }
    fcntl(ends[0], F_SETFD, FD_CLOEXEC);
    fcntl(ends[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, ends[1], ReportDescriptor);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, flags);
    posix_spawnattr_setpgroup(&attributes, 0);
    char program[] = "fbtest";
    char name[16];
    strncpy(name, mode, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char* arguments[] = {program, name, nullptr};
    const int error = posix_spawn(&child->pid, "fbtest", &actions, &attributes, arguments, environ);
    close(ends[1]);
    if (error != 0)
    {
        close(ends[0]);
        return false;
    }
    child->report = ends[0];
    return true;
}

char ReadReport(const Child& child)
{
    char result = 0;
    return read(child.report, &result, 1) == 1 ? result : '\0';
}

bool Reap(Child* child)
{
    int status = 0;
    const bool reaped = waitpid(child->pid, &status, 0) == child->pid;
    close(child->report);
    child->report = -1;
    return reaped;
}

void CheckOpenAndInfo(int descriptor)
{
    Check(descriptor >= 0, "opening /dev/fb0");
    Check((fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0, "close-on-exec on /dev/fb0");
    fb_info info = {1, 1, 1, 1};
    Check(ioctl(descriptor, FB_GET_INFO, &info) == 0, "FB_GET_INFO");
    Check(info.width == ScreenWidth && info.height == ScreenHeight, "the screen size");
    Check(info.console == 1 && info.dropped_events == 0, "the console number");
    CheckFails(ioctl(descriptor, FB_GET_INFO, reinterpret_cast<void*>(8)), EFAULT,
               "FB_GET_INFO with a bad pointer");
    CheckFails(ioctl(descriptor, 0x4699UL), EINVAL, "an unknown request");
}

void CheckBeforeAcquire(int descriptor)
{
    static uint32_t pixel = 0;
    CheckFails(Present(descriptor, &pixel, 1, 0, 0, 1, 1), EINVAL, "a present before acquiring");
    CheckFails(ioctl(descriptor, FB_RELEASE), EINVAL, "a release before acquiring");
    char byte;
    CheckFails(read(descriptor, &byte, 1) < 0 ? -1 : 0, EINVAL, "a read");
    pollfd entry = {descriptor, POLLIN, 0};
    Check(poll(&entry, 1, 0) == 1 && (entry.revents & POLLERR) != 0, "poll before acquiring");
    CheckFails(write(descriptor, &byte, 1) < 0 ? -1 : 0, EINVAL, "a write");
    CheckFails(lseek(descriptor, 0, SEEK_SET) < 0 ? -1 : 0, ESPIPE, "a seek");
    CheckFails(ioctl(STDIN_FILENO, FB_ACQUIRE), ENOTTY, "FB_ACQUIRE on the console");
    CheckFails(ioctl(STDIN_FILENO, FB_GET_INFO, nullptr), ENOTTY, "FB_GET_INFO on the console");
    const int file = open("/dev/null", O_RDWR);
    CheckFails(ioctl(file, FB_ACQUIRE), ENOTTY, "FB_ACQUIRE on /dev/null");
    close(file);
    CheckFails(ioctl(99, FB_ACQUIRE), EBADF, "FB_ACQUIRE on a closed descriptor");
}

void CheckNoTerminal()
{
    Child child;
    Check(Start("enotty", POSIX_SPAWN_SETSID, &child), "starting a session leader");
    Check(ReadReport(child) == 'n', "FB_ACQUIRE without a controlling terminal");
    Check(Reap(&child), "reaping the session leader");
}

void CheckAcquireAndBusy(int descriptor)
{
    Check(ioctl(descriptor, FB_ACQUIRE) == 0, "FB_ACQUIRE");
    Check(ioctl(descriptor, FB_ACQUIRE) == 0, "FB_ACQUIRE again");
    const int second = OpenFramebuffer();
    Check(second >= 0, "a second open");
    CheckFails(ioctl(second, FB_ACQUIRE), EBUSY, "FB_ACQUIRE from a second open file");
    close(second);
    Child child;
    Check(Start("busy", POSIX_SPAWN_BACKGROUND, &child), "starting a second owner");
    Check(ReadReport(child) == 'b', "FB_ACQUIRE from another process");
    Check(Reap(&child), "reaping the second owner");
}

void CheckEvents(int descriptor)
{
    pollfd entry = {descriptor, POLLIN, 0};
    Check(poll(&entry, 1, 0) == 1 && (entry.revents & POLLIN) != 0, "poll with the state event");
    fb_event event = {};
    Check(read(descriptor, &event, sizeof(event)) == 8, "reading the state event");
    Check(event.kind == FB_EVENT_SHOWN && event.code == 0 && event.modifiers == 0,
          "the first event");
    entry = {descriptor, POLLIN, 0};
    Check(poll(&entry, 1, 30) == 0, "poll with no events");

    fb_event pair[2];
    CheckFails(read(descriptor, pair, 4) < 0 ? -1 : 0, EINVAL, "a read of 4 bytes");
    Check(read(descriptor, pair, 0) == 0, "a read of 0 bytes");
    const int flags = fcntl(descriptor, F_GETFL);
    Check(fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0, "setting O_NONBLOCK");
    CheckFails(read(descriptor, pair, sizeof(pair)) < 0 ? -1 : 0, EAGAIN, "a non-blocking read");
    Check(fcntl(descriptor, F_SETFL, flags) == 0, "clearing O_NONBLOCK");
    const int second = OpenFramebuffer(O_NONBLOCK);
    Check(second >= 0 && (fcntl(second, F_GETFL) & O_NONBLOCK) != 0, "O_NONBLOCK from open");
    close(second);
    fb_info info = {1, 1, 1, 1};
    Check(ioctl(descriptor, FB_GET_INFO, &info) == 0 && info.dropped_events == 0,
          "dropped events after acquiring");
}

void CheckPresentErrors(int descriptor)
{
    static uint32_t small[100 * 100];
    CheckFails(Present(descriptor, reinterpret_cast<const uint32_t*>(8), 16, 0, 0, 16, 16), EFAULT,
               "pixels at address 8");
    CheckFails(
        Present(descriptor, reinterpret_cast<const uint32_t*>(KernelAddress), 16, 0, 0, 16, 16),
        EFAULT, "pixels in kernel memory");
    CheckFails(
        Present(descriptor, reinterpret_cast<const uint32_t*>(UINTPTR_MAX - 8), 16, 0, 0, 16, 16),
        EFAULT, "pixels that wrap");
    CheckFails(Present(descriptor, small, 10, 0, 0, 20, 5), EINVAL, "a stride below the width");
    CheckFails(Present(descriptor, small, 100, 0, 0, 10, 10, 1), EINVAL, "present flags");
    CheckFails(Present(descriptor, small, 20000, 0, 0, 20000, 1), EINVAL, "a width of 20000");
    Check(Present(descriptor, nullptr, 0, 0, 0, 0, 0) == 0, "an empty rectangle");
    Check(Present(descriptor, small, 100, -5000, 0, 100, 100) == 0,
          "a rectangle left of the screen");
    Check(Present(descriptor, small, 100, 0, 0, 100, 100) == 0, "a small rectangle");
    Check(Present(descriptor, small, 100, -50, -50, 100, 100) == 0, "a rectangle on the corner");
    Check(Present(descriptor, small, 100, 1000, 760, 100, 100) == 0, "a rectangle past the edge");

    auto* screen = static_cast<uint32_t*>(
        malloc(static_cast<size_t>(ScreenWidth) * ScreenHeight * sizeof(uint32_t)));
    Check(screen != nullptr, "allocating a screen");
    for (uint32_t index = 0; index < ScreenWidth * ScreenHeight; ++index)
    {
        screen[index] = 0x00112233 + index;
    }
    Check(Present(descriptor, screen, ScreenWidth, 0, 0, ScreenWidth, ScreenHeight) == 0,
          "a full-screen present");
    free(screen);
}

void CheckReleaseOnClose(int descriptor)
{
    static uint32_t pixels[64];
    const int duplicate = dup(descriptor);
    Check(duplicate >= 0, "dup");
    close(descriptor);
    Check(Present(duplicate, pixels, 8, 0, 0, 8, 8) == 0, "a present on the duplicate");
    const int second = OpenFramebuffer();
    CheckFails(ioctl(second, FB_ACQUIRE), EBUSY, "FB_ACQUIRE while a duplicate lives");
    close(duplicate);
    Check(ioctl(second, FB_ACQUIRE) == 0, "FB_ACQUIRE after the last close");
    Check(ioctl(second, FB_RELEASE) == 0, "FB_RELEASE");
    CheckFails(ioctl(second, FB_RELEASE), EINVAL, "a second FB_RELEASE");
    CheckFails(Present(second, pixels, 8, 0, 0, 8, 8), EINVAL, "a present after releasing");
    close(second);
}

void* BlockedReader(void* argument)
{
    const int descriptor = *static_cast<int*>(argument);
    fb_event event;
    const ssize_t result = read(descriptor, &event, sizeof(event));
    return reinterpret_cast<void*>(result == -1 ? static_cast<intptr_t>(errno) : 0);
}

void CheckReleaseWakesReader()
{
    int descriptor = OpenFramebuffer();
    Check(ioctl(descriptor, FB_ACQUIRE) == 0, "FB_ACQUIRE for a blocked reader");
    fb_event event;
    Check(read(descriptor, &event, sizeof(event)) == 8, "draining the state event");
    pthread_t thread;
    Check(pthread_create(&thread, nullptr, BlockedReader, &descriptor) == 0,
          "starting a reader thread");
    Pause();
    Check(ioctl(descriptor, FB_RELEASE) == 0, "FB_RELEASE under a blocked reader");
    void* result = nullptr;
    Check(pthread_join(thread, &result) == 0, "joining the reader thread");
    Check(reinterpret_cast<intptr_t>(result) == EINVAL, "a blocked read failing after the release");
    close(descriptor);
}

void CheckEventChildren()
{
    Child child;
    Check(Start("eintr", POSIX_SPAWN_BACKGROUND, &child), "starting the reader child");
    Check(ReadReport(child) == 'r', "the reader child draining its events");
    Pause();
    Check(kill(child.pid, SIGUSR1) == 0, "signalling the reader child");
    Check(ReadReport(child) == 'i', "a blocked read failing with EINTR");
    Check(Reap(&child), "reaping the reader child");

    Check(Start("nonblockopen", POSIX_SPAWN_BACKGROUND, &child), "starting the non-blocking child");
    Check(ReadReport(child) == 'o', "a non-blocking open reading its first event and EAGAIN");
    Check(Reap(&child), "reaping the non-blocking child");
}

void CheckReleaseAfterChild(const char* mode, short flags, char reported, const char* name,
                            bool kill_it)
{
    const int descriptor = OpenFramebuffer();
    Check(descriptor >= 0, name);
    Child child;
    Check(Start(mode, flags, &child), name);
    Check(ReadReport(child) == reported, name);
    if (kill_it)
    {
        CheckFails(ioctl(descriptor, FB_ACQUIRE), EBUSY, name);
        Check(kill(child.pid, SIGKILL) == 0, name);
    }
    Check(Reap(&child), name);
    Check(ioctl(descriptor, FB_ACQUIRE) == 0, name);
    Check(ioctl(descriptor, FB_RELEASE) == 0, name);
    close(descriptor);
}

void CheckBackground()
{
    const int descriptor = OpenFramebuffer();
    Child child;
    Check(Start("stop", POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_BACKGROUND, &child),
          "starting a background child");
    int status = 0;
    Check(waitpid(child.pid, &status, WUNTRACED) == child.pid && WIFSTOPPED(status) &&
              WSTOPSIG(status) == SIGTTOU,
          "SIGTTOU stopping a background FB_ACQUIRE");
    Check(kill(child.pid, SIGKILL) == 0, "killing the stopped child");
    Check(Reap(&child), "reaping the stopped child");
    Check(ioctl(descriptor, FB_ACQUIRE) == 0, "FB_ACQUIRE after the stopped child");
    Check(ioctl(descriptor, FB_RELEASE) == 0, "FB_RELEASE after the stopped child");

    Check(Start("ignore", POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_BACKGROUND, &child),
          "starting a background child that ignores SIGTTOU");
    Check(ReadReport(child) == 'a', "FB_ACQUIRE with SIGTTOU ignored");
    Check(Reap(&child), "reaping the child that ignored SIGTTOU");
    Check(ioctl(descriptor, FB_ACQUIRE) == 0, "FB_ACQUIRE after the child that ignored SIGTTOU");
    Check(ioctl(descriptor, FB_RELEASE) == 0, "FB_RELEASE at the end");
    close(descriptor);
}

} // namespace

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (argc == 2)
    {
        return RunChild(argv[1]);
    }
    if (argc != 1)
    {
        puts("usage: fbtest");
        return 1;
    }
    const int descriptor = OpenFramebuffer();
    CheckOpenAndInfo(descriptor);
    CheckBeforeAcquire(descriptor);
    CheckNoTerminal();
    CheckAcquireAndBusy(descriptor);
    CheckEvents(descriptor);
    CheckPresentErrors(descriptor);
    CheckReleaseOnClose(descriptor);
    CheckReleaseWakesReader();
    CheckEventChildren();
    CheckReleaseAfterChild("hold", POSIX_SPAWN_BACKGROUND, 'h', "release when a child is killed",
                           true);
    CheckReleaseAfterChild("exit", POSIX_SPAWN_BACKGROUND, 'a', "release when a child exits",
                           false);
    CheckBackground();
    puts("fbtest: ok");
    return 0;
}
