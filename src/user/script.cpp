#include "command_line.h"
#include "window_size.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

using namespace FeatherOS::User;

constexpr char Help[] =
    "usage: script [-a] [-q] [-c command] [file]\n"
    "Runs a shell, or one command with -c, on a new pseudo-terminal and saves what\n"
    "it prints to file, typescript by default; -a appends to it. The session ends\n"
    "when the shell or the command exits. -q leaves out the start and end messages.";
constexpr char Usage[] = "usage: script [-a] [-q] [-c command] [file]";

namespace
{

struct Options
{
    bool append = false;
    bool quiet = false;
    const char* command = nullptr;
    const char* file = "typescript";
};

bool ParseOptions(int argc, char** argv, Options* options)
{
    int index = 1;
    for (; index < argc && argv[index][0] == '-'; ++index)
    {
        if (strcmp(argv[index], "-a") == 0)
        {
            options->append = true;
        }
        else if (strcmp(argv[index], "-q") == 0)
        {
            options->quiet = true;
        }
        else if (strcmp(argv[index], "-c") == 0 && index + 1 < argc)
        {
            options->command = argv[++index];
        }
        else
        {
            return false;
        }
    }
    if (index + 1 < argc)
    {
        return false;
    }
    if (index < argc)
    {
        options->file = argv[index];
    }
    return true;
}

bool WriteAll(int descriptor, const char* bytes, size_t length)
{
    while (length > 0)
    {
        const ssize_t written = write(descriptor, bytes, length);
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        if (written <= 0)
        {
            return false;
        }
        bytes += written;
        length -= static_cast<size_t>(written);
    }
    return true;
}

void WriteTimestamp(int log, const char* event)
{
    const time_t now = time(nullptr);
    char text[80];
    char stamp[32];
    if (now == static_cast<time_t>(-1) ||
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC", gmtime(&now)) == 0)
    {
        snprintf(stamp, sizeof(stamp), "an unknown time");
    }
    const int length = snprintf(text, sizeof(text), "Script %s on %s\n", event, stamp);
    WriteAll(log, text, static_cast<size_t>(length));
}

void CopyWindowSize(int master)
{
    winsize size{};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0)
    {
        ioctl(master, TIOCSWINSZ, &size);
    }
}

// The child leads a session on the slave, so it takes the slave as its controlling terminal. The
// master and slave descriptors themselves are close-on-exec.
bool StartSession(const Options& options, int slave, pid_t* child)
{
    const char* shell = getenv("SHELL");
    shell = shell == nullptr || shell[0] == '\0' ? "shell" : shell;
    char* arguments[] = {const_cast<char*>(shell),
                         const_cast<char*>(options.command == nullptr ? nullptr : "-c"),
                         const_cast<char*>(options.command), nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    const int error = posix_spawnp(child, shell, &actions, &attributes, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (error != 0)
    {
        printf("script: %s: %s\n", shell, strerror(error));
    }
    return error == 0;
}

// Relays the keys from standard input to the master and the session's output from the master to
// standard output and the log, until the session ends. The master is non-blocking: a program
// that is busy writing reads no input, so waiting for room for its input could stop both.
// Returns false if the log could not be written.
bool Relay(int master, int log, pid_t child, int* status)
{
    char input[256];
    size_t input_length = 0;
    size_t input_sent = 0;
    bool input_open = true;
    bool exited = false;
    bool logged = true;
    for (;;)
    {
        if (WindowSizeChanged())
        {
            CopyWindowSize(master);
        }
        const bool pending = input_sent < input_length;
        pollfd entries[2] = {
            {.fd = master,
             .events = static_cast<short>(POLLIN | (pending ? POLLOUT : 0)),
             .revents = 0},
            {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
        };
        const nfds_t count = input_open && !pending ? 2 : 1;
        // Nothing announces the child's exit, so it is checked at least every 200 ms; once it
        // exited, what it left in the master is drained.
        const int ready = poll(entries, count, exited ? 0 : 200);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if ((entries[0].revents & (POLLIN | POLLHUP)) != 0)
        {
            char output[1024];
            const ssize_t received = read(master, output, sizeof(output));
            if (received > 0)
            {
                WriteAll(STDOUT_FILENO, output, static_cast<size_t>(received));
                logged = logged && WriteAll(log, output, static_cast<size_t>(received));
            }
            else if (received == 0 || (errno != EAGAIN && errno != EINTR))
            {
                break;
            }
        }
        if (pending && (entries[0].revents & POLLOUT) != 0)
        {
            const ssize_t sent = write(master, input + input_sent, input_length - input_sent);
            if (sent > 0)
            {
                input_sent += static_cast<size_t>(sent);
            }
        }
        if (count == 2 && (entries[1].revents & (POLLIN | POLLHUP)) != 0)
        {
            const ssize_t received = read(STDIN_FILENO, input, sizeof(input));
            if (received > 0)
            {
                input_length = static_cast<size_t>(received);
                input_sent = 0;
            }
            else if (received == 0 || errno != EINTR)
            {
                input_open = false;
            }
        }
        if (!exited)
        {
            exited = waitpid(child, status, WNOHANG) == child;
        }
        else if (ready == 0)
        {
            break;
        }
    }
    if (!exited)
    {
        *status = -1;
    }
    return logged;
}

} // namespace

int main(int argc, char** argv)
{
    if (HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    Options options;
    if (!ParseOptions(argc, argv, &options))
    {
        puts(Usage);
        return 1;
    }
    const int log = open(
        options.file, O_WRONLY | O_CREAT | O_CLOEXEC | (options.append ? O_APPEND : O_TRUNC), 0644);
    if (log < 0)
    {
        printf("script: %s: %s\n", options.file, strerror(errno));
        return 1;
    }
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    const char* name =
        master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0 ? ptsname(master) : nullptr;
    const int slave = name == nullptr ? -1 : open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0)
    {
        printf("script: cannot create a pseudo-terminal: %s\n", strerror(errno));
        return 1;
    }
    CopyWindowSize(master);
    WatchWindowSize();
    pid_t child = 0;
    if (!StartSession(options, slave, &child))
    {
        return 1;
    }
    close(slave);
    fcntl(master, F_SETFL, O_NONBLOCK);
    if (!options.quiet)
    {
        printf("Script started, output file is %s\n", options.file);
        fflush(stdout);
    }
    WriteTimestamp(log, "started");
    termios original{};
    const bool terminal = tcgetattr(STDIN_FILENO, &original) == 0;
    if (terminal)
    {
        // Ctrl+C and the other keys go to the session as bytes, for its own line discipline.
        termios raw = original;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    int status = 0;
    const bool logged = Relay(master, log, child, &status);
    if (terminal)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &original);
    }
    // Closing the master hangs up a session that outlived the end of its output.
    close(master);
    if (status == -1 && waitpid(child, &status, 0) != child)
    {
        status = 0;
    }
    WriteTimestamp(log, "done");
    close(log);
    if (!logged)
    {
        printf("script: %s: cannot write the session\n", options.file);
    }
    if (!options.quiet)
    {
        printf("Script done, output file is %s\n", options.file);
    }
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
}
