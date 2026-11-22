#include "vfs.h"

#include "user/syscall_abi.h"

#include <stdint.h>

// The walk keeps two strings. walk->path is the physical path resolved so far, "/" or "/a/b",
// without links, "." or "..", so its textual parent is its real parent, across a mount point too:
// ".." drops its last component. Its first `checked` characters are known to hold no link; the
// components appended since are checked together (FlushLinks) at the end, before "..", and before
// a last component that is not followed, so a path without links costs one driver call per mount
// it crosses. walk->pending holds what is still to walk, right-aligned in its buffer, so a link's
// target goes in front of the rest without moving it.
//
// A link found is replaced by its target: an absolute one restarts at "/", a relative one goes on
// from the link's directory. The components after the link wait in pending behind the target, so
// a ".." among them leaves the target's directory, not the link's: POSIX's physical walk, where
// "link/.." is the parent of what link points to. Links are counted rather than recursed into, so
// the kernel stack use stays the same whatever the path holds.
namespace FeatherOS::VFS
{
namespace
{

constexpr uint32_t Capacity = Path::MaximumLength;
// pending's last byte is always its terminator.
constexpr uint32_t PendingEnd = Capacity - 1;

void CopyBytes(char* destination, const char* source, uint32_t length)
{
    for (uint32_t index = 0; index < length; ++index)
    {
        destination[index] = source[index];
    }
}

class Walker
{
public:
    explicit Walker(PathWalk* walk) : walk(walk) {}

    WalkResult Run(const char* path, Follow follow)
    {
        uint32_t length = 0;
        while (path[length] != '\0')
        {
            if (++length >= Capacity)
            {
                return WalkResult::NameTooLong;
            }
        }
        start = PendingEnd - length;
        CopyBytes(walk->pending + start, path, length);
        walk->pending[PendingEnd] = '\0';
        walk->path[0] = '/';
        walk->path[1] = '\0';
        path_length = 1;
        checked = 1;
        for (;;)
        {
            // start stays before the '/'s ahead of a name until the name is used, so text put back
            // in front of it stays separated from it.
            uint32_t name = start;
            while (walk->pending[name] == '/')
            {
                ++name;
            }
            uint32_t end = name;
            while (walk->pending[end] != '\0' && walk->pending[end] != '/')
            {
                ++end;
            }
            const char* text = walk->pending + name;
            const uint32_t name_length = end - name;
            const bool dot = name_length == 1 && text[0] == '.';
            const bool dot_dot = name_length == 2 && text[0] == '.' && text[1] == '.';
            // A trailing '/' makes even an unfollowed last component follow, as on Linux.
            const bool unfollowed_last =
                follow == Follow::NotLast && name_length > 0 && walk->pending[end] == '\0';
            if (dot)
            {
                start = end;
                continue;
            }
            if (name_length == 0 || dot_dot || unfollowed_last)
            {
                bool expanded = false;
                const WalkResult flushed = FlushLinks(&expanded);
                if (flushed != WalkResult::Done)
                {
                    return flushed;
                }
                if (expanded)
                {
                    continue;
                }
                if (name_length == 0)
                {
                    return WalkResult::Done;
                }
            }
            if (dot_dot)
            {
                const WalkResult left = LeaveDirectory();
                if (left != WalkResult::Done)
                {
                    return left;
                }
                start = end;
                continue;
            }
            const WalkResult appended = Append(text, name_length);
            start = end;
            if (appended != WalkResult::Done || unfollowed_last)
            {
                return appended;
            }
        }
    }

private:
    WalkResult Append(const char* text, uint32_t length)
    {
        const uint32_t separator = path_length > 1 ? 1 : 0;
        if (length > User::MaximumNameLength || path_length + separator + length >= Capacity)
        {
            return WalkResult::NameTooLong;
        }
        if (separator != 0)
        {
            walk->path[path_length++] = '/';
        }
        CopyBytes(walk->path + path_length, text, length);
        path_length += length;
        walk->path[path_length] = '\0';
        return WalkResult::Done;
    }

    void DropLastComponent()
    {
        uint32_t after_separator = path_length;
        while (after_separator > 0 && walk->path[after_separator - 1] != '/')
        {
            --after_separator;
        }
        path_length = after_separator > 1 ? after_separator - 1 : 1;
        walk->path[path_length] = '\0';
    }

    // "..": only an existing directory has a parent to go to.
    WalkResult LeaveDirectory()
    {
        if (path_length == 1)
        {
            return WalkResult::Done;
        }
        if (!IsDirectory(walk->path))
        {
            FileInfo info;
            return Stat(walk->path, &info) ? WalkResult::NotDirectory : WalkResult::NotFound;
        }
        DropLastComponent();
        checked = path_length;
        return WalkResult::Done;
    }

    // Checks the components appended since the last check. On a link, puts what followed it back
    // in front of pending and expands the link, setting *expanded.
    WalkResult FlushLinks(bool* expanded)
    {
        *expanded = false;
        if (checked >= path_length)
        {
            return WalkResult::Done;
        }
        const uint32_t link_end = FirstLink(walk->path, checked);
        if (link_end == 0)
        {
            checked = path_length;
            return WalkResult::Done;
        }
        // These came out of pending, so the room they need is free in front of it.
        const uint32_t rest = path_length - link_end;
        if (rest > start)
        {
            return WalkResult::NameTooLong;
        }
        start -= rest;
        CopyBytes(walk->pending + start, walk->path + link_end, rest);
        path_length = link_end;
        walk->path[path_length] = '\0';
        *expanded = true;
        return Expand();
    }

    WalkResult Expand()
    {
        if (++expansions > User::MaximumSymbolicLinks)
        {
            return WalkResult::Loop;
        }
        uint32_t length = 0;
        if (!ReadLink(walk->path, walk->target, Capacity, &length) || length == 0)
        {
            return WalkResult::NotFound;
        }
        if (length >= Capacity || length > start)
        {
            return WalkResult::NameTooLong;
        }
        start -= length;
        CopyBytes(walk->pending + start, walk->target, length);
        if (walk->target[0] == '/')
        {
            path_length = 1;
            walk->path[path_length] = '\0';
        }
        else
        {
            DropLastComponent();
        }
        checked = path_length;
        return WalkResult::Done;
    }

    PathWalk* walk;
    uint32_t start = 0;
    uint32_t path_length = 0;
    uint32_t checked = 0;
    uint32_t expansions = 0;
};

} // namespace

WalkResult ResolvePath(const char* path, Follow follow, PathWalk* walk)
{
    if (path == nullptr || walk == nullptr)
    {
        return WalkResult::NotFound;
    }
    if (path[0] != '/')
    {
        uint32_t length = 0;
        for (; path[length] != '\0'; ++length)
        {
            if (length + 1 >= Capacity)
            {
                return WalkResult::NameTooLong;
            }
            walk->path[length] = path[length];
        }
        walk->path[length] = '\0';
        return WalkResult::Done;
    }
    return Walker(walk).Run(path, follow);
}

} // namespace FeatherOS::VFS
