#include "tcp_client.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

namespace FeatherOS::User
{

int Connect(const char* program, const char* host, const char* port, int family, int type)
{
    addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_socktype = type;
    addrinfo* results = nullptr;
    const int error = getaddrinfo(host, port, &hints, &results);
    if (error != 0)
    {
        fprintf(stderr, "%s: %s: %s\n", program, host, gai_strerror(error));
        return -1;
    }
    int connection = -1;
    int failure = 0;
    for (const addrinfo* result = results; result != nullptr && connection < 0;
         result = result->ai_next)
    {
        connection = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (connection >= 0 && connect(connection, result->ai_addr, result->ai_addrlen) != 0)
        {
            failure = errno;
            close(connection);
            connection = -1;
        }
    }
    freeaddrinfo(results);
    if (connection < 0)
    {
        fprintf(stderr, "%s: %s %s: %s\n", program, host, port, strerror(failure));
    }
    return connection;
}

bool SendAll(int connection, const char* data, size_t length)
{
    while (length > 0)
    {
        const ssize_t sent = send(connection, data, length, MSG_NOSIGNAL);
        if (sent <= 0)
        {
            return false;
        }
        data += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}

bool WriteAll(int descriptor, const char* data, size_t length)
{
    while (length > 0)
    {
        const ssize_t written = write(descriptor, data, length);
        if (written <= 0)
        {
            return false;
        }
        data += written;
        length -= static_cast<size_t>(written);
    }
    return true;
}

} // namespace FeatherOS::User
