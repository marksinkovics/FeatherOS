#ifndef FEATHEROS_USER_FILE_COPY_H
#define FEATHEROS_USER_FILE_COPY_H

#include <stddef.h>

// What cp and mv share. Failures are printed as "program: path: error".
namespace FeatherOS::User
{

// Where source goes for a target: inside it, under source's own name, when target is a
// directory. False when the result is too long for a path.
bool ResolveTarget(const char* program, const char* source, const char* target, char* path,
                   size_t capacity);
// Replaces an existing file at target.
bool CopyFile(const char* program, const char* source, const char* target);

} // namespace FeatherOS::User

#endif
