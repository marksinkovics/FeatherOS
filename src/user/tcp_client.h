#ifndef FEATHEROS_USER_TCP_CLIENT_H
#define FEATHEROS_USER_TCP_CLIENT_H

#include <stddef.h>

// What nc and fetch share.
namespace FeatherOS::User
{

// Connects a socket of type (SOCK_STREAM or SOCK_DGRAM) to the first of host's addresses in
// family (AF_UNSPEC for any) that accepts. On failure, prints why after "program: " and returns
// -1.
int Connect(const char* program, const char* host, const char* port, int family, int type);

// Sends all of data, without SIGPIPE on a closed connection.
bool SendAll(int connection, const char* data, size_t length);
bool WriteAll(int descriptor, const char* data, size_t length);

} // namespace FeatherOS::User

#endif
