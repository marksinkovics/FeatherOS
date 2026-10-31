#ifndef FEATHEROS_LIBC_RESOLV_CONF_H
#define FEATHEROS_LIBC_RESOLV_CONF_H

#include <stddef.h>
#include <stdint.h>

// /etc/resolv.conf: "nameserver address[#port]" lines and "options timeout:n attempts:n". Lines
// starting with "#" or ";" are comments, and other keywords are ignored.
namespace FeatherOS::LibC::ResolvConf
{

// glibc's MAXNS, RES_MAXRETRANS and RES_MAXRETRY.
constexpr size_t MaximumNameServers = 3;
constexpr int MaximumTimeoutSeconds = 30;
constexpr int MaximumAttempts = 5;

struct NameServer
{
    // 4 or 16 bytes, in network order.
    size_t address_length;
    uint8_t address[16];
    uint16_t port;
};

struct Config
{
    size_t name_server_count;
    NameServer name_servers[MaximumNameServers];
    // Seconds to wait for each answer, and rounds through the name servers.
    int timeout_seconds;
    int attempts;
};

// Adds the text's name servers to config, past MaximumNameServers ignored, and sets the options
// it names, clamped to at least 1 and at most the maximums; malformed lines and values are
// skipped.
void Parse(const char* text, size_t length, Config* config);

} // namespace FeatherOS::LibC::ResolvConf

#endif
