#ifndef FEATHEROS_FILESYSTEM_FILESYSTEM_H
#define FEATHEROS_FILESYSTEM_FILESYSTEM_H

#include <stdint.h>

// The interface between the VFS and a mounted filesystem. Paths reaching a driver are relative to
// its mount point, such as "docs/a.txt", with "" for its root.
namespace FeatherOS::Filesystem
{

struct DirectoryEntry
{
    // The long name if the entry has one, otherwise "NAME.EXT", and a terminator.
    char name[64];
    bool directory;
    uint32_t size;
};

struct FileInfo
{
    uint64_t size;
    bool device;
};

// An open file as its driver identifies it; the VFS never looks inside. The ID stays the same for
// every handle to the same file while it exists, so it keys the open-file rules.
struct Node
{
    uint64_t id;
    uint64_t payload;
};

struct Properties
{
    // Nothing on it can be created, written or removed.
    bool read_only;
    // Its open files are tracked, so a file is not removed or written by two handles at once.
    bool tracked;
    // Its files are devices: they have no size, accept any offset and never move it.
    bool devices;
};

// Operations a driver leaves out fail.
class Driver
{
public:
    constexpr explicit Driver(Properties properties) : properties(properties) {}
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    Driver(Driver&&) = delete;
    Driver& operator=(Driver&&) = delete;

    const Properties& Attributes() const
    {
        return properties;
    }

    virtual bool Open(const char* path, Node* node, uint64_t* size);
    virtual bool Stat(const char* path, FileInfo* info);
    virtual bool Read(const Node& node, uint64_t size, uint64_t offset, void* buffer,
                      uint64_t length, uint64_t* bytes_read);
    virtual void Close(const Node& node);
    virtual bool Create(const char* path, Node* node, uint64_t* size);
    // offset is at most *size. Updates *size; *bytes_written may be short when space runs out.
    virtual bool Write(Node* node, uint64_t* size, uint64_t offset, const void* buffer,
                       uint64_t length, uint64_t* bytes_written);
    virtual bool Truncate(Node* node, uint64_t* size, uint64_t new_size);
    virtual bool Remove(const char* path);
    virtual bool MakeDirectory(const char* path);
    virtual bool RemoveDirectory(const char* path);
    // Both paths are on this filesystem.
    virtual bool Rename(const char* from, const char* to);
    virtual bool IsDirectory(const char* path);
    virtual bool ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry);
    virtual bool IsReadOnly(const char* path);

protected:
    ~Driver() = default;

private:
    Properties properties;
};

} // namespace FeatherOS::Filesystem

#endif
