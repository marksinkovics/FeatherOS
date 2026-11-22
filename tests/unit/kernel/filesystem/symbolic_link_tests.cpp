#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/filesystem/tmpfs.h"
#include "kernel/filesystem/vfs.h"

#include <memory>
#include <string>
#include <vector>

namespace
{

namespace User = FeatherOS::User;
using FeatherOS::VFS::Follow;
using FeatherOS::VFS::WalkResult;

// As the kernel does at boot.
[[maybe_unused]] const bool built_in_mounted = FeatherOS::VFS::MountBuiltIn();

WalkResult Walk(const std::string& path, Follow follow, std::string* resolved = nullptr)
{
    const auto walk = std::make_unique<FeatherOS::VFS::PathWalk>();
    const WalkResult result = FeatherOS::VFS::ResolvePath(path.c_str(), follow, walk.get());
    if (resolved != nullptr)
    {
        *resolved = walk->path;
    }
    return result;
}

std::string Resolve(const std::string& path, Follow follow = Follow::All)
{
    std::string resolved;
    REQUIRE(Walk(path, follow, &resolved) == WalkResult::Done);
    return resolved;
}

void WriteFile(const char* path, const std::string& contents)
{
    FeatherOS::Files::Table table{};
    const uint64_t descriptor =
        FeatherOS::Files::Open(&table, path, User::OpenWrite | User::OpenCreate);
    REQUIRE_FALSE(User::IsSyscallError(descriptor));
    REQUIRE(FeatherOS::Files::Write(&table, descriptor, contents.data(), contents.size()) ==
            contents.size());
    FeatherOS::Files::CloseAll(&table);
}

std::string ReadFile(const char* path)
{
    FeatherOS::Files::Table table{};
    const uint64_t descriptor = FeatherOS::Files::Open(&table, path, User::OpenRead);
    REQUIRE_FALSE(User::IsSyscallError(descriptor));
    std::string contents;
    char buffer[64];
    for (;;)
    {
        const uint64_t count = FeatherOS::Files::Read(&table, descriptor, buffer, sizeof(buffer));
        REQUIRE_FALSE(User::IsSyscallError(count));
        if (count == 0)
        {
            break;
        }
        contents.append(buffer, count);
    }
    FeatherOS::Files::CloseAll(&table);
    return contents;
}

uint64_t OpenResult(const char* path, uint64_t flags)
{
    FeatherOS::Files::Table table{};
    const uint64_t result = FeatherOS::Files::Open(&table, path, flags);
    FeatherOS::Files::CloseAll(&table);
    return result;
}

std::string ReadLink(const char* path)
{
    char target[User::MaximumPathLength];
    const uint64_t length = FeatherOS::Files::ReadLink(path, target, sizeof(target));
    REQUIRE_FALSE(User::IsSyscallError(length));
    return {target, length};
}

void Link(const char* target, const char* path)
{
    REQUIRE(FeatherOS::Files::SymbolicLink(target, path) == 0);
}

// /tmp/dir/file.txt, and /tmp/a/b/f.txt with /tmp/other beside it.
void MakeTree()
{
    FeatherOS::Tmpfs::Reset();
    REQUIRE(FeatherOS::Files::MakeDirectory("/tmp/dir") == 0);
    WriteFile("/tmp/dir/file.txt", "contents");
    REQUIRE(FeatherOS::Files::MakeDirectory("/tmp/a") == 0);
    REQUIRE(FeatherOS::Files::MakeDirectory("/tmp/a/b") == 0);
    WriteFile("/tmp/a/b/f.txt", "deep");
    REQUIRE(FeatherOS::Files::MakeDirectory("/tmp/other") == 0);
}

std::vector<std::string> List(const char* path)
{
    std::vector<std::string> names;
    User::DirectoryEntry entry{};
    for (uint64_t index = 0; FeatherOS::Files::ReadDirectory(path, index, &entry) == 0; ++index)
    {
        const bool link = entry.type == User::ModeSymbolicLink >> User::EntryTypeShift;
        names.push_back(std::string(entry.name) + (link ? "@" : ""));
    }
    return names;
}

} // namespace

TEST_CASE("symbolic links resolve absolute and relative targets, and chains of them")
{
    MakeTree();
    Link("/tmp/dir/file.txt", "/tmp/absolute");
    Link("dir/file.txt", "/tmp/relative");
    Link("relative", "/tmp/chain1");
    Link("/tmp/chain1", "/tmp/chain2");
    Link("../chain2", "/tmp/dir/chain3");

    REQUIRE(Resolve("/tmp/absolute") == "/tmp/dir/file.txt");
    REQUIRE(Resolve("/tmp/relative") == "/tmp/dir/file.txt");
    REQUIRE(Resolve("/tmp/dir/chain3") == "/tmp/dir/file.txt");
    REQUIRE(Resolve("//tmp/./dir//chain3") == "/tmp/dir/file.txt");
    REQUIRE(ReadFile("/tmp/dir/chain3") == "contents");
    REQUIRE(Resolve("/tmp/chain2", Follow::NotLast) == "/tmp/chain2");
    REQUIRE(Resolve("/tmp/dir/chain3", Follow::NotLast) == "/tmp/dir/chain3");
    // A trailing '/' follows even an unfollowed last link, which here names a file.
    REQUIRE(Resolve("/tmp/chain2/", Follow::NotLast) == "/tmp/dir/file.txt");
    REQUIRE(List("/tmp") == std::vector<std::string>{"dir", "a", "other", "absolute@",
                                                     "relative@", "chain1@", "chain2@"});
    // Programs run through links too.
    FeatherOS::Files::ProgramFile program;
    REQUIRE(program.Open("/tmp/chain2") == 0);
    REQUIRE(program.Size() == 8);
}

TEST_CASE("a link to a directory works mid-path, and .. after it leaves the target")
{
    MakeTree();
    Link("/tmp/a/b", "/tmp/other/to-b");
    Link("../a/b/../b", "/tmp/other/relative-b");

    REQUIRE(Resolve("/tmp/other/to-b/f.txt") == "/tmp/a/b/f.txt");
    REQUIRE(ReadFile("/tmp/other/to-b/f.txt") == "deep");
    REQUIRE(Resolve("/tmp/other/relative-b/f.txt") == "/tmp/a/b/f.txt");
    // The physical parent of the target, not the link's directory /tmp/other.
    REQUIRE(Resolve("/tmp/other/to-b/..") == "/tmp/a");
    REQUIRE(Resolve("/tmp/other/to-b/../b/./f.txt") == "/tmp/a/b/f.txt");
    REQUIRE(Resolve("/tmp/other/to-b/..", Follow::NotLast) == "/tmp/a");
    REQUIRE(List("/tmp/other/to-b") == std::vector<std::string>{"f.txt"});
    // ".." needs an existing directory to leave; the root is its own parent.
    REQUIRE(Walk("/tmp/missing/..", Follow::All) == WalkResult::NotFound);
    REQUIRE(Walk("/tmp/a/b/f.txt/..", Follow::All) == WalkResult::NotDirectory);
    REQUIRE(Resolve("/../tmp") == "/tmp");
    REQUIRE(Resolve("/tmp/../../etc/.") == "/etc");
    // Missing components elsewhere are left for the call to report.
    REQUIRE(Resolve("/tmp/missing/x") == "/tmp/missing/x");
    REQUIRE(OpenResult("/tmp/other/to-b/none", User::OpenRead) == User::SyscallErrorNotFound);
}

TEST_CASE("links cross mounts into /tmp and out of it")
{
    MakeTree();
    WriteFile("/etc/settings.conf", "etc text");
    Link("/tmp/dir", "/etc/into-tmp");
    Link("../etc/settings.conf", "/tmp/out");
    Link("/etc", "/tmp/etc-link");
    Link("/proc", "/tmp/proc-link");

    REQUIRE(Resolve("/etc/into-tmp/file.txt") == "/tmp/dir/file.txt");
    REQUIRE(ReadFile("/etc/into-tmp/file.txt") == "contents");
    // ".." in the target leaves /tmp's mount for the root, then /etc's mount takes over.
    REQUIRE(Resolve("/tmp/out") == "/etc/settings.conf");
    REQUIRE(ReadFile("/tmp/out") == "etc text");
    REQUIRE(Resolve("/tmp/etc-link/..") == "/");
    REQUIRE(Resolve("/tmp/etc-link/../tmp/dir") == "/tmp/dir");
    REQUIRE(Resolve("/tmp/etc-link/into-tmp/../a/b") == "/tmp/a/b");
    REQUIRE(Resolve("/tmp/proc-link/meminfo") == "/proc/meminfo");
    REQUIRE(FeatherOS::Files::Remove("/etc/into-tmp") == 0);
    REQUIRE(FeatherOS::Files::Remove("/etc/settings.conf") == 0);
}

TEST_CASE("loops fail with ELOOP after 40 links, and a chain of 40 still resolves")
{
    MakeTree();
    Link("loop-b", "/tmp/loop-a");
    Link("loop-a", "/tmp/loop-b");
    Link("self", "/tmp/self");
    Link("growing/x", "/tmp/growing");

    REQUIRE(Walk("/tmp/loop-a", Follow::All) == WalkResult::Loop);
    REQUIRE(Walk("/tmp/self/x", Follow::NotLast) == WalkResult::Loop);
    REQUIRE(Walk("/tmp/growing", Follow::All) == WalkResult::Loop);
    REQUIRE(OpenResult("/tmp/loop-a", User::OpenRead) == User::SyscallErrorLoop);
    User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/tmp/loop-a", &status) == User::SyscallErrorLoop);
    REQUIRE(FeatherOS::Files::LinkStat("/tmp/loop-a", &status) == 0);
    REQUIRE(status.type == User::FileType::SymbolicLink);
    REQUIRE(FeatherOS::Files::Remove("/tmp/loop-a") == 0);

    // link-k resolves through k + 1 links.
    Link("/tmp/dir/file.txt", "/tmp/link-0");
    for (int index = 1; index <= 40; ++index)
    {
        Link(("link-" + std::to_string(index - 1)).c_str(),
             ("/tmp/link-" + std::to_string(index)).c_str());
    }
    static_assert(User::MaximumSymbolicLinks == 40);
    REQUIRE(Resolve("/tmp/link-39") == "/tmp/dir/file.txt");
    REQUIRE(Walk("/tmp/link-40", Follow::All) == WalkResult::Loop);
    REQUIRE(Resolve("/tmp/link-40", Follow::NotLast) == "/tmp/link-40");
}

TEST_CASE("a dangling link opens nothing but describes itself, and O_CREAT makes its target")
{
    MakeTree();
    Link("/tmp/nowhere", "/tmp/dangling");

    REQUIRE(OpenResult("/tmp/dangling", User::OpenRead) == User::SyscallErrorNotFound);
    User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/tmp/dangling", &status) == User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::LinkStat("/tmp/dangling", &status) == 0);
    REQUIRE(status.mode == (User::ModeSymbolicLink | 0777));
    REQUIRE(status.size == 12);
    REQUIRE(ReadLink("/tmp/dangling") == "/tmp/nowhere");
    char resolved[User::MaximumPathLength];
    REQUIRE(FeatherOS::Files::ResolvePath("/tmp/dangling", resolved, sizeof(resolved)) ==
            User::SyscallErrorNotFound);

    WriteFile("/tmp/dangling", "made through the link");
    REQUIRE(ReadFile("/tmp/nowhere") == "made through the link");
    REQUIRE(FeatherOS::Files::Stat("/tmp/dangling", &status) == 0);
    REQUIRE(status.type == User::FileType::Regular);
}

TEST_CASE("O_NOFOLLOW refuses a link in the last component only")
{
    MakeTree();
    Link("/tmp/dir/file.txt", "/tmp/absolute");
    Link("/tmp/a/b", "/tmp/other/to-b");

    REQUIRE(OpenResult("/tmp/absolute", User::OpenRead | User::OpenNoFollow) ==
            User::SyscallErrorLoop);
    REQUIRE(OpenResult("/tmp/absolute", User::OpenWrite | User::OpenCreate | User::OpenNoFollow) ==
            User::SyscallErrorLoop);
    REQUIRE_FALSE(User::IsSyscallError(
        OpenResult("/tmp/dir/file.txt", User::OpenRead | User::OpenNoFollow)));
    REQUIRE_FALSE(User::IsSyscallError(
        OpenResult("/tmp/other/to-b/f.txt", User::OpenRead | User::OpenNoFollow)));
    REQUIRE(OpenResult("/tmp/absolute", User::OpenRead | (User::OpenNoFollow << 1)) ==
            User::SyscallErrorInvalidArgument);
}

TEST_CASE("lstat describes a link, stat what it names")
{
    MakeTree();
    Link("/tmp/dir/file.txt", "/tmp/absolute");
    Link("/tmp/a", "/tmp/to-a");

    User::FileStatus file{};
    User::FileStatus followed{};
    User::FileStatus link{};
    REQUIRE(FeatherOS::Files::Stat("/tmp/dir/file.txt", &file) == 0);
    REQUIRE(FeatherOS::Files::Stat("/tmp/absolute", &followed) == 0);
    REQUIRE(FeatherOS::Files::LinkStat("/tmp/absolute", &link) == 0);
    REQUIRE(followed.node == file.node);
    REQUIRE(followed.type == User::FileType::Regular);
    REQUIRE(followed.size == 8);
    REQUIRE(link.node != file.node);
    REQUIRE(link.device == file.device);
    REQUIRE(link.mode == (User::ModeSymbolicLink | 0777));
    REQUIRE(link.type == User::FileType::SymbolicLink);
    REQUIRE(link.size == sizeof("/tmp/dir/file.txt") - 1);
    REQUIRE_FALSE(link.read_only);
    REQUIRE(FeatherOS::Files::Stat("/tmp/to-a", &followed) == 0);
    REQUIRE(followed.type == User::FileType::Directory);
    REQUIRE(FeatherOS::Files::LinkStat("/tmp/to-a", &link) == 0);
    REQUIRE(link.type == User::FileType::SymbolicLink);
    // A link in the middle is followed by both.
    REQUIRE(FeatherOS::Files::LinkStat("/tmp/to-a/b/f.txt", &link) == 0);
    REQUIRE(link.type == User::FileType::Regular);
}

TEST_CASE("a path that passes PATH_MAX once a link is expanded fails with ENAMETOOLONG")
{
    MakeTree();
    const std::string name(200, 'n');
    const std::string target = "/tmp/" + name + "/" + name + "/" + name + "/" + name;
    REQUIRE(target.size() == 808);
    Link(target.c_str(), "/tmp/long");
    // Within PATH_MAX as passed, past it once resolved.
    const std::string tail(250, 't');
    const std::string path = "/tmp/long/" + tail;
    REQUIRE(path.size() < User::MaximumPathLength);
    REQUIRE(Walk(path, Follow::All) == WalkResult::NameTooLong);
    User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat(path.c_str(), &status) == User::SyscallErrorNameTooLong);
    REQUIRE(OpenResult(path.c_str(), User::OpenWrite | User::OpenCreate) ==
            User::SyscallErrorNameTooLong);
    // A shorter tail still fits.
    REQUIRE(Resolve("/tmp/long/" + std::string(200, 't')) == target + "/" + std::string(200, 't'));
    // The target and the rest of the path still to walk must fit together, too.
    std::string rest = "/tmp/long/../";
    for (int index = 0; index < 120; ++index)
    {
        rest += "a/../";
    }
    rest += "z";
    REQUIRE(rest.size() < User::MaximumPathLength);
    REQUIRE(Walk(rest, Follow::All) == WalkResult::NameTooLong);
    REQUIRE(Walk("/tmp/" + std::string(256, 'x') + "/..", Follow::All) == WalkResult::NameTooLong);
}

TEST_CASE("chdir through a link keeps the physical path")
{
    MakeTree();
    Link("/tmp/a/b", "/tmp/other/to-b");
    Link("/tmp/dir/file.txt", "/tmp/absolute");
    Link("/tmp/nowhere", "/tmp/dangling");

    char working_directory[User::MaximumPathLength] = "/";
    REQUIRE(FeatherOS::Files::ChangeDirectory("/tmp/other/to-b", working_directory) == 0);
    REQUIRE(std::string(working_directory) == "/tmp/a/b");
    REQUIRE(FeatherOS::Files::ChangeDirectory("/tmp/other/to-b/..", working_directory) == 0);
    REQUIRE(std::string(working_directory) == "/tmp/a");
    REQUIRE(FeatherOS::Files::ChangeDirectory("/tmp/absolute", working_directory) ==
            User::SyscallErrorNotDirectory);
    REQUIRE(FeatherOS::Files::ChangeDirectory("/tmp/dangling", working_directory) ==
            User::SyscallErrorNotFound);
    REQUIRE(std::string(working_directory) == "/tmp/a");
    REQUIRE(FeatherOS::Files::CheckDirectory("/tmp/other/to-b") == 0);
}

TEST_CASE("rm, rename, rmdir and mkdir act on a link itself")
{
    MakeTree();
    Link("/tmp/dir/file.txt", "/tmp/absolute");
    Link("dir/file.txt", "/tmp/relative");
    Link("/tmp/a/b", "/tmp/to-b");
    Link("/tmp/nowhere", "/tmp/dangling");

    REQUIRE(FeatherOS::Files::Remove("/tmp/relative") == 0);
    REQUIRE(ReadFile("/tmp/dir/file.txt") == "contents");
    User::FileStatus status{};
    REQUIRE(FeatherOS::Files::LinkStat("/tmp/relative", &status) == User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/tmp/to-b") == User::SyscallErrorNotDirectory);
    REQUIRE(FeatherOS::Files::Remove("/tmp/to-b") == 0);
    REQUIRE(FeatherOS::Files::CheckDirectory("/tmp/a/b") == 0);
    REQUIRE(FeatherOS::Files::MakeDirectory("/tmp/dangling") == User::SyscallErrorExists);
    REQUIRE(FeatherOS::Files::CheckDirectory("/tmp/nowhere") == User::SyscallErrorNotFound);

    REQUIRE(FeatherOS::Files::Rename("/tmp/absolute", "/tmp/renamed") == 0);
    REQUIRE(Resolve("/tmp/renamed") == "/tmp/dir/file.txt");
    REQUIRE(FeatherOS::Files::LinkStat("/tmp/absolute", &status) == User::SyscallErrorNotFound);
    // Renaming over a link replaces the link, not what it names.
    WriteFile("/tmp/new.txt", "new");
    REQUIRE(FeatherOS::Files::Rename("/tmp/new.txt", "/tmp/renamed") == 0);
    REQUIRE(FeatherOS::Files::LinkStat("/tmp/renamed", &status) == 0);
    REQUIRE(status.type == User::FileType::Regular);
    REQUIRE(ReadFile("/tmp/dir/file.txt") == "contents");
    REQUIRE(FeatherOS::Files::Rename("/tmp/dangling", "/tmp/dir/moved") == 0);
    REQUIRE(ReadLink("/tmp/dir/moved") == "/tmp/nowhere");
    REQUIRE(FeatherOS::Files::Rename("/tmp/a", "/tmp/dir/moved") == User::SyscallErrorNotDirectory);
}

TEST_CASE("symlink, link and readlink fail as Linux's do")
{
    MakeTree();
    Link("/tmp/dir/file.txt", "/tmp/absolute");

    REQUIRE(FeatherOS::Files::SymbolicLink("x", "/tmp/dir/file.txt") == User::SyscallErrorExists);
    REQUIRE(FeatherOS::Files::SymbolicLink("x", "/tmp/absolute") == User::SyscallErrorExists);
    REQUIRE(FeatherOS::Files::SymbolicLink("x", "/tmp/missing/link") ==
            User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::SymbolicLink("", "/tmp/empty") == User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::SymbolicLink("x", "/system/link") == User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::SymbolicLink("x", "/proc/link") == User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::SymbolicLink("x", "/dev/link") == User::SyscallErrorReadOnly);

    char target[4];
    REQUIRE(FeatherOS::Files::ReadLink("/tmp/absolute", target, sizeof(target)) == 4);
    REQUIRE(std::string(target, 4) == "/tmp");
    REQUIRE(FeatherOS::Files::ReadLink("/tmp/absolute", target, 0) ==
            User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::ReadLink("/tmp/dir/file.txt", target, sizeof(target)) ==
            User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::ReadLink("/tmp/none", target, sizeof(target)) ==
            User::SyscallErrorNotFound);

    // tmpfs holds no hard links: a node keeps its own name.
    REQUIRE(FeatherOS::Files::Link("/tmp/dir/file.txt", "/tmp/hard") ==
            User::SyscallErrorNotPermitted);
    REQUIRE(FeatherOS::Files::Link("/tmp/dir/file.txt", "/etc/hard") ==
            User::SyscallErrorCrossDevice);
    REQUIRE(FeatherOS::Files::Link("/tmp/dir", "/tmp/dir2") == User::SyscallErrorNotPermitted);
    REQUIRE(FeatherOS::Files::Link("/tmp/none", "/tmp/x") == User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Link("/tmp/dir/file.txt", "/tmp/absolute") ==
            User::SyscallErrorExists);
    REQUIRE(FeatherOS::Files::Link("/tmp/dir/file.txt", "/tmp/missing/x") ==
            User::SyscallErrorNotFound);
}

TEST_CASE("realpath's walk resolves links, dots and repeated slashes of an existing file")
{
    MakeTree();
    Link("/tmp/a/b", "/tmp/other/to-b");

    char resolved[User::MaximumPathLength];
    REQUIRE(FeatherOS::Files::ResolvePath("/tmp/other//to-b/../b/./f.txt", resolved,
                                          sizeof(resolved)) == sizeof("/tmp/a/b/f.txt") - 1);
    REQUIRE(std::string(resolved) == "/tmp/a/b/f.txt");
    REQUIRE(FeatherOS::Files::ResolvePath("/tmp/other/to-b", resolved, sizeof(resolved)) == 8);
    REQUIRE(FeatherOS::Files::ResolvePath("/", resolved, sizeof(resolved)) == 1);
    REQUIRE(std::string(resolved) == "/");
    REQUIRE(FeatherOS::Files::ResolvePath("/tmp/other/to-b/none", resolved, sizeof(resolved)) ==
            User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::ResolvePath("/tmp/other/to-b", resolved, 8) ==
            User::SyscallErrorRange);
}
