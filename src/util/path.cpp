#include <sstream>
#include <algorithm>

#include "path.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <libgen.h>
#include <linux/limits.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <mntent.h>
}

#ifndef FALLOC_FL_COLLAPSE_RANGE
#define FALLOC_FL_COLLAPSE_RANGE        0x08
#endif

struct FileHandle {
    struct file_handle head;
    char data[MAX_HANDLE_SZ];
    FileHandle() {
        head.handle_bytes = MAX_HANDLE_SZ;
    }
};

std::string TPath::DirNameStr() const {
    char *dup = strdup(Path.c_str());
    PORTO_ASSERT(dup != nullptr);

    char *p = dirname(dup);
    std::string out(p);
    free(dup);

    return out;
}

TPath TPath::DirName() const {
    std::string s = DirNameStr();
    return TPath(s);
}

std::string TPath::BaseName() const {
    char *dup = strdup(Path.c_str());
    PORTO_ASSERT(dup != nullptr);
    char *p = basename(dup);
    std::string out(p);
    free(dup);

    return out;
}

TError TPath::StatStrict(struct stat &st) const {
    if (lstat(Path.c_str(), &st))
        return TError(EError::Unknown, errno, "lstat " + Path);
    return TError::Success();
}

TError TPath::StatFollow(struct stat &st) const {
    if (stat(Path.c_str(), &st))
        return TError(EError::Unknown, errno, "stat " + Path);
    return TError::Success();
}

bool TPath::IsRegularStrict() const {
    struct stat st;
    return !lstat(c_str(), &st) && S_ISREG(st.st_mode);
}

bool TPath::IsRegularFollow() const {
    struct stat st;
    return !stat(c_str(), &st) && S_ISREG(st.st_mode);
}

bool TPath::IsDirectoryStrict() const {
    struct stat st;
    return !lstat(c_str(), &st) && S_ISDIR(st.st_mode);
}

bool TPath::IsDirectoryFollow() const {
    struct stat st;
    return !stat(c_str(), &st) && S_ISDIR(st.st_mode);
}

bool TPath::IsSameInode(const TPath &other) const {
    struct stat a, b;
    if (stat(c_str(), &a) || stat(other.c_str(), &b))
        return false;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

unsigned int TPath::GetDev() const {
    struct stat st;

    if (stat(Path.c_str(), &st))
        return 0;

    return st.st_dev;
}

unsigned int TPath::GetBlockDev() const {
    struct stat st;

    if (stat(Path.c_str(), &st) || !S_ISBLK(st.st_mode))
        return 0;

    return st.st_rdev;
}

bool TPath::Exists() const {
    return access(Path.c_str(), F_OK) == 0;
}

bool TPath::HasAccess(const TCred &cred, enum Access mask) const {
    struct stat st;
    int mode;

    if (!cred.Uid && !access(c_str(), mask & TPath::RWX))
        return true;

    if (stat(c_str(), &st)) {
        if (!(mask & TPath::P) || errno != ENOENT)
            return false;
        TPath dir = DirName();
        while (stat(dir.c_str(), &st)) {
            if (errno != ENOENT)
                return false;
            if (dir.Path.size() <= 1)
                return false;
            dir = dir.DirName();
        }
    }

    if ((mask & TPath::U) && cred.Uid == st.st_uid)
        return true;

    if (cred.Uid == st.st_uid)
        mode = st.st_mode >> 6;
    else if (cred.IsMemberOf(st.st_gid))
        mode = st.st_mode >> 3;
    else
        mode = st.st_mode;

    return (mode & mask & TPath::RWX) == (mask & TPath::RWX);
}

std::string TPath::ToString() const {
    return Path;
}

TPath TPath::AddComponent(const TPath &component) const {
    if (component.IsAbsolute()) {
        if (IsRoot())
            return TPath(component.Path);
        if (component.IsRoot())
            return TPath(Path);
        return TPath(Path + component.Path);
    }
    if (IsRoot())
        return TPath("/" + component.Path);
    return TPath(Path + "/" + component.Path);
}

TError TPath::Chdir() const {
    if (chdir(Path.c_str()) < 0)
        return TError(EError::InvalidValue, errno, "chdir(" + Path + ")");

    return TError::Success();
}

TError TPath::Chroot() const {
    L_ACT() << "chroot " << Path << std::endl;

    if (chroot(Path.c_str()) < 0)
        return TError(EError::Unknown, errno, "chroot(" + Path + ")");

    return TError::Success();
}

// https://github.com/lxc/lxc/commit/2d489f9e87fa0cccd8a1762680a43eeff2fe1b6e
TError TPath::PivotRoot() const {
    TFile oldroot, newroot;
    TError error;

    L_ACT() << "pivot root " << Path << std::endl;

    error = oldroot.OpenDir("/");
    if (error)
        return error;

    error = newroot.OpenDir(*this);
    if (error)
        return error;

    /* old and new root must be at different mounts */
    if (oldroot.GetMountId() == newroot.GetMountId()) {
        error = BindAll(*this);
        if (error)
            return error;
        error = newroot.OpenDir(*this);
        if (error)
            return error;
    }

    if (fchdir(newroot.Fd))
        return TError(EError::Unknown, errno, "fchdir(newroot)");

    if (syscall(__NR_pivot_root, ".", "."))
        return TError(EError::Unknown, errno, "pivot_root()");

    if (fchdir(oldroot.Fd) < 0)
        return TError(EError::Unknown, errno, "fchdir(oldroot)");

    if (umount2(".", MNT_DETACH) < 0)
        return TError(EError::Unknown, errno, "umount2(.)");

    if (fchdir(newroot.Fd) < 0)
        return TError(EError::Unknown, errno, "fchdir(newroot) reenter");

    return TError::Success();
}

TError TPath::Chown(uid_t uid, gid_t gid) const {
    if (chown(Path.c_str(), uid, gid))
        return TError(EError::Unknown, errno, "chown(" + Path + ", " +
                        UserName(uid) + ", " + GroupName(gid) + ")");
    return TError::Success();
}

TError TPath::Chmod(const int mode) const {
    int ret = chmod(Path.c_str(), mode);
    if (ret)
        return TError(EError::Unknown, errno, "chmod(" + Path + ", " + StringFormat("%#o", mode) + ")");

    return TError::Success();
}

TError TPath::ReadLink(TPath &value) const {
    char buf[PATH_MAX];
    ssize_t len;

    len = readlink(Path.c_str(), buf, sizeof(buf) - 1);
    if (len < 0)
        return TError(EError::Unknown, errno, "readlink(" + Path + ")");

    buf[len] = '\0';

    value = TPath(buf);
    return TError::Success();
}

TError TPath::Symlink(const TPath &target) const {
    int ret = symlink(target.c_str(), Path.c_str());
    if (ret)
        return TError(EError::Unknown, errno, "symlink(" + target.ToString() + ", " + Path + ")");
    return TError::Success();
}

TError TPath::Mknod(unsigned int mode, unsigned int dev) const {
    int ret = mknod(Path.c_str(), mode, dev);
    if (ret)
        return TError(EError::Unknown, errno, "mknod(" + Path + ", " +
                StringFormat("%#o", mode) + ", " + StringFormat("%#x", dev) + ")");
    return TError::Success();
}

TError TPath::Mkfile(unsigned int mode) const {
    return Mknod(S_IFREG | (mode & 0777), 0);
}

TPath TPath::NormalPath() const {
    std::stringstream ss(Path);
    std::string component, path;

    if (IsEmpty())
        return TPath();

    if (IsAbsolute())
        path = "/";

    while (std::getline(ss, component, '/')) {

        if (component == "" || component == ".")
            continue;

        if (component == "..") {
            auto last = path.rfind('/');

            if (last == std::string::npos) {
                /* a/.. */
                if (!path.empty() && path != "..") {
                    path = "";
                    continue;
                }
            } else if (path.compare(last + 1, std::string::npos, "..") != 0) {
                if (last == 0)
                    path.erase(last + 1);   /* /.. or /a/.. */
                else
                    path.erase(last);       /* a/b/.. */
                continue;
            }
        }

        if (!path.empty() && path != "/")
            path += "/";
        path += component;
    }

    if (path.empty())
        path = ".";

    return TPath(path);
}

TPath TPath::AbsolutePath() const {
    char cwd[PATH_MAX];

    if (IsAbsolute() || IsEmpty())
        return TPath(Path);

    if (!getcwd(cwd, sizeof(cwd)))
        return TPath();

    return TPath(std::string(cwd) + "/" + Path);
}

TPath TPath::RealPath() const {
    char *p = realpath(Path.c_str(), NULL);
    if (!p)
        return Path;

    TPath path(p);

    free(p);
    return path;
}

/*
 * Returns relative or absolute path inside this or
 * empty path if argument path is not inside:
 *
 * "/root".InnerPath("/root/foo", true) -> "/foo"
 * "/root".InnerPath("/foo", true) -> ""
 */
TPath TPath::InnerPath(const TPath &path, bool absolute) const {

    unsigned len = Path.length();

    /* check prefix */
    if (!len || path.Path.compare(0, len, Path) != 0)
        return TPath();

    /* exact match */
    if (path.Path.length() == len) {
        if (absolute)
            return TPath("/");
        else
            return TPath(".");
    }

    /* prefix "/" act as "" */
    if (len == 1 && Path[0] == '/')
        len = 0;

    /* '/' must follow prefix */
    if (path.Path[len] != '/')
        return TPath();

    /* cut prefix */
    if (absolute)
        return TPath(path.Path.substr(len));
    else
        return TPath(path.Path.substr(len + 1));
}

bool TPath::IsInside(const std::vector<TPath> paths) const {
    for (auto &path: paths) {
        if (!path.InnerPath(*this).IsEmpty())
            return true;
    }
    return false;
}

TError TPath::StatFS(TStatFS &result) const {
    struct statvfs st;

    int ret = statvfs(Path.c_str(), &st);
    if (ret)
        return TError(EError::Unknown, errno, "statvfs(" + Path + ")");

    result.SpaceUsage = (uint64_t)(st.f_blocks - st.f_bfree) * st.f_bsize;
    result.SpaceAvail = (uint64_t)(st.f_bavail) * st.f_bsize;
    result.InodeUsage = st.f_files - st.f_ffree;
    result.InodeAvail = st.f_favail;
    return TError::Success();
}

TError TPath::Unlink() const {
    if (unlink(c_str()))
        return TError(EError::Unknown, errno, "unlink(" + Path + ")");
    return TError::Success();
}

TError TPath::Rename(const TPath &dest) const {
    if (rename(c_str(), dest.c_str()))
        return TError(EError::Unknown, errno, "rename(" + Path + ", " + dest.Path + ")");
    return TError::Success();
}

TError TPath::Mkdir(unsigned int mode) const {
    if (mkdir(Path.c_str(), mode) < 0)
        return TError(errno == ENOSPC ? EError::NoSpace :
                                        EError::Unknown,
                      errno, "mkdir(" + Path + ", " + StringFormat("%#o", mode) + ")");
    return TError::Success();
}

TError TPath::MkdirAll(unsigned int mode) const {
    std::vector<TPath> paths;
    TPath path(Path);
    TError error;

    while (!path.Exists()) {
        paths.push_back(path);
        path = path.DirName();
    }

    if (!path.IsDirectoryFollow())
        return TError(EError::Unknown, "Not a directory: " + path.ToString());

    for (auto path = paths.rbegin(); path != paths.rend(); path++) {
        error = path->Mkdir(mode);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TPath::MkdirTmp(const TPath &parent, const std::string &prefix, unsigned int mode) {
    Path = (parent / (prefix + "XXXXXX")).Path;
    if (!mkdtemp(&Path[0]))
        return TError(EError::Unknown, errno, "mkdtemp(" + Path + ")");
    if (mode != 0700)
        return Chmod(mode);
    return TError::Success();
}

TError TPath::CreateAll(unsigned int mode) const {
    if (!Exists()) {
        TPath dir = DirName();
        TError error;

        if (!dir.Exists()) {
            error = dir.MkdirAll(0755);
            if (error)
                return error;
        }

        /* This fails for broken symlinks */
        return Mkfile(mode);
    } else if (IsDirectoryFollow())
        return TError(EError::Unknown, "Is a directory: " + Path);

    return TError::Success();
}

TError TPath::Rmdir() const {
    if (rmdir(Path.c_str()) < 0)
        return TError(EError::Unknown, errno, "rmdir(" + Path + ")");
    return TError::Success();
}

/*
 * Removes everything in the directory but not directory itself.
 * Works only on one filesystem and aborts if sees mountpint.
 */
TError TPath::ClearDirectory() const {
    int top_fd, dir_fd, sub_fd;
    DIR *top = NULL, *dir;
    struct dirent *de;
    struct stat top_st, st;
    TError error = TError::Success();

    L_ACT() << "clear directory " << Path << std::endl;

    top_fd = open(Path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                O_NOFOLLOW | O_NOATIME);
    if (top_fd < 0)
        return TError(EError::Unknown, errno, "ClearDirectory open(" + Path + ")");

    if (fstat(top_fd, &top_st)) {
        close(top_fd);
        return TError(EError::Unknown, errno, "ClearDirectory fstat(" + Path + ")");
    }

    dir_fd = top_fd;

deeper:
    dir = fdopendir(dir_fd);
    if (dir == NULL) {
        close(dir_fd);
        if (dir_fd != top_fd)
            closedir(top);
        return TError(EError::Unknown, errno, "ClearDirectory fdopendir(" + Path + "/.../)");
    }

restart:
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (fstatat(dir_fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT)
                continue;
            error = TError(EError::Unknown, errno, "ClearDirectory fstatat(" +
                                          Path + "/.../" + de->d_name + ")");
            break;
        }

        if (st.st_dev != top_st.st_dev) {
            error = TError(EError::Unknown, EXDEV, "ClearDirectory found mountpoint in " + Path);
            break;
        }

        if (Verbose)
            L_ACT() << "clear directory: unlink " << de->d_name << std::endl;
        if (!unlinkat(dir_fd, de->d_name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0))
            continue;

        if (errno == ENOENT)
            continue;

        if (errno == EPERM || errno == EACCES) {
            sub_fd = openat(dir_fd, de->d_name, O_RDONLY | O_CLOEXEC |
                                    O_NOFOLLOW | O_NOCTTY | O_NONBLOCK);
            if (sub_fd >= 0) {
                error = TFile::Chattr(sub_fd, 0, FS_APPEND_FL | FS_IMMUTABLE_FL);
                close(sub_fd);
                if (error)
                    L_ERR() << "Cannot change "  << de->d_name <<
                        " attributes: " << error << std::endl;
            }
            error = TFile::Chattr(dir_fd, 0, FS_APPEND_FL | FS_IMMUTABLE_FL);
            if (error)
                L_ERR() << "Cannot change directory attributes: " << error << std::endl;

            if (!unlinkat(dir_fd, de->d_name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0))
                continue;
        }

        if (!S_ISDIR(st.st_mode) || (errno != ENOTEMPTY && errno != EEXIST)) {
            error = TError(EError::Unknown, errno, "ClearDirectory unlinkat(" +
                           Path + "/.../" + de->d_name + ")");
            break;
        }

        sub_fd = openat(dir_fd, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                            O_NOFOLLOW | O_NOATIME);
        if (sub_fd >= 0) {
            if (dir_fd != top_fd)
                closedir(dir); /* closes dir_fd */
            else
                top = dir;
            dir_fd = sub_fd;
            if (Verbose)
                L_ACT() << "clear directory: enter " << de->d_name << std::endl;
            goto deeper;
        }
        if (errno == ENOENT)
            continue;

        error = TError(EError::Unknown, errno, "ClearDirectory openat(" +
                       Path + "/.../" + de->d_name + ")");
        break;
    }

    closedir(dir); /* closes dir_fd */

    if (dir_fd != top_fd) {
        if (!error) {
            rewinddir(top);
            dir = top;
            dir_fd = top_fd;
            if (Verbose)
                L_ACT() << "clear directory: restart " << Path << std::endl;
            goto restart; /* Restart from top directory */
        }
        closedir(top); /* closes top_fd */
    }

    return error;
}

TError TPath::RemoveAll() const {
    if (IsDirectoryStrict()) {
        TError error = ClearDirectory();
        if (error)
            return error;
        return Rmdir();
    }
    return Unlink();
}

TError TPath::ReadDirectory(std::vector<std::string> &result) const {
    struct dirent *de;
    DIR *dir;

    result.clear();
    dir = opendir(c_str());
    if (!dir)
        return TError(EError::Unknown, errno, "Cannot open directory " + Path);

    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
            result.push_back(std::string(de->d_name));
    }
    closedir(dir);
    return TError::Success();
}

TError TPath::ListSubdirs(std::vector<std::string> &result) const {
    struct dirent *de;
    DIR *dir;

    result.clear();
    dir = opendir(c_str());
    if (!dir)
        return TError(EError::Unknown, errno, "Cannot open directory " + Path);

    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") &&
            (de->d_type == DT_DIR ||
             (de->d_type == DT_UNKNOWN &&
              (*this / std::string(de->d_name)).IsDirectoryStrict())))
            result.push_back(std::string(de->d_name));
    }
    closedir(dir);
    return TError::Success();
}

int64_t TPath::SinceModificationMs() const {
    struct stat st;

    if (lstat(Path.c_str(), &st))
        return -1;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 -
           (int64_t)st.st_mtim.tv_sec * 1000 - st.st_mtim.tv_nsec / 1000000;
}

TError TPath::SetXAttr(const std::string name, const std::string value) const {
    if (syscall(SYS_setxattr, Path.c_str(), name.c_str(),
                value.c_str(), value.length(), 0))
        return TError(EError::Unknown, errno,
                "setxattr(" + Path + ", " + name + ")");
    return TError::Success();
}

TError TPath::Truncate(off_t size) const {
    if (truncate(c_str(), size))
        return TError(EError::Unknown, errno, "truncate(" + Path + ")");
    return TError::Success();
}

TError TPath::RotateLog(off_t max_disk_usage, off_t &loss) const {
    struct stat st;
    off_t hole_len;
    TError error;

    int fd = open(c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path + ")");

    if (fstat(fd, &st))
        return TError(EError::Unknown, errno, "fstat(" + Path + ")");

    if (!S_ISREG(st.st_mode) || (off_t)st.st_blocks * 512 <= max_disk_usage) {
        loss = 0;
        close(fd);
        return TError::Success();
    }

    /* Keep half of allowed size or trucate to zero */
    hole_len = st.st_size - max_disk_usage / 2;
    hole_len -= hole_len % st.st_blksize;
    loss = hole_len;

    if (fallocate(fd, FALLOC_FL_COLLAPSE_RANGE, 0, hole_len)) {
        loss = st.st_size;
        if (ftruncate(fd, 0))
            error = TError(EError::Unknown, errno, "truncate(" + Path + ")");
    }

    close(fd);
    return error;
}

TError TPath::Chattr(unsigned add_flags, unsigned del_flags) const {
    TError error;
    TFile file;

    error = file.Open(*this, O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                             O_NOCTTY | O_NONBLOCK);
    if (error)
        return error;
    error = TFile::Chattr(file.Fd, add_flags, del_flags);
    if (error)
        return TError(error, Path);
    return TError::Success();
}

#ifndef MS_LAZYTIME
# define MS_LAZYTIME    (1<<25)
#endif

const TFlagsNames TPath::MountFlags = {
    { MS_RDONLY,        "ro" },
    { MS_NOSUID,        "nosuid" },
    { MS_NODEV,         "nodev" },
    { MS_NOEXEC,        "noexec" },
    { MS_SYNCHRONOUS,   "sync" },
    { MS_REMOUNT,       "remount" },
    { MS_MANDLOCK,      "mand" },
    { MS_DIRSYNC,       "dirsync" },
    { MS_NOATIME,       "noatime" },
    { MS_NODIRATIME,    "nodiratime" },
    { MS_BIND,          "bind" },
    { MS_MOVE,          "move" },
    { MS_REC,           "rec" },
    { MS_SILENT,        "silent" },
    { MS_POSIXACL,      "acl" },
    { MS_UNBINDABLE,    "unbindable" },
    { MS_PRIVATE,       "private" },
    { MS_SLAVE,         "slave" },
    { MS_SHARED,        "shared" },
    { MS_RELATIME,      "relatime" },
    { MS_I_VERSION,     "iversion" },
    { MS_STRICTATIME,   "strictatime" },
    { MS_LAZYTIME,      "lazyatime" },
};

const TFlagsNames TPath::UmountFlags = {
    { MNT_FORCE,        "force" },
    { MNT_DETACH,       "detach" },
    { MNT_EXPIRE,       "expire" },
    { UMOUNT_NOFOLLOW,  "nofollow" },
};

std::string TPath::MountFlagsToString(unsigned long flags) {
    return StringFormatFlags(flags, MountFlags);
}

std::string TPath::UmountFlagsToString(unsigned long flags) {
    return StringFormatFlags(flags, UmountFlags);
}

TError TPath::Mount(const TPath &source, const std::string &type, unsigned long flags,
                    const std::vector<std::string> &options) const {
    std::string data = MergeEscapeStrings(options, ',');

    if (data.length() >= 4096)
        return TError(EError::Unknown, E2BIG, "mount option too big: " +
                      std::to_string(data.length()));

    L_ACT() << "mount -t " << type << " " << source << " " << Path
            << " -o " << data <<  " " << MountFlagsToString(flags) << std::endl;
    if (mount(source.c_str(), Path.c_str(), type.c_str(), flags, data.c_str()))
        return TError(EError::Unknown, errno, "mount(" + source.ToString() + ", " +
                      Path + ", " + type + ", " + MountFlagsToString(flags) + ", " + data + ")");
    return TError::Success();
}

TError TPath::Bind(const TPath &source) const {
    L_ACT() << "bind mount " << Path << " " << source << " " << std::endl;
    if (mount(source.c_str(), Path.c_str(), NULL, MS_BIND, NULL))
        return TError(EError::Unknown, errno, "mount(" + source.ToString() +
                ", " + Path + ", , MS_BIND, )");
    return TError::Success();
}

TError TPath::BindAll(const TPath &source) const {
    L_ACT() << "bind mount all " << Path << " " << source << " " << std::endl;
    if (mount(source.c_str(), Path.c_str(), NULL, MS_BIND | MS_REC, NULL))
        return TError(EError::Unknown, errno, "mount(" + source.ToString() +
                ", " + Path + ", , MS_BIND | MS_REC, )");
    return TError::Success();
}

TError TPath::Remount(unsigned long flags) const {
    L_ACT() << "remount " << Path << " "
            << MountFlagsToString(flags) << std::endl;
    if (mount(NULL, Path.c_str(), NULL, flags, NULL))
        return TError(EError::Unknown, errno, "mount(NULL, " + Path +
                      ", NULL, " + MountFlagsToString(flags) + ", NULL)");
    return TError::Success();
}

TError TPath::BindRemount(const TPath &source, unsigned long flags) const {
    TError error = Bind(source);
    if (!error)
        error = Remount(MS_REMOUNT | MS_BIND | flags);
    return error;
}

TError TPath::Umount(unsigned long flags) const {
    L_ACT() << "umount " << Path << " "
            << UmountFlagsToString(flags) << std::endl;
    if (umount2(Path.c_str(), flags))
        return TError(EError::Unknown, errno, "umount2(" + Path + ", " +
                                        UmountFlagsToString(flags) +  ")");
    return TError::Success();
}

TError TPath::UmountAll() const {
    L_ACT() << "umount all " << Path << std::endl;
    while (1) {
        if (umount2(c_str(), UMOUNT_NOFOLLOW)) {
            if (errno == EINVAL)
                return TError::Success(); /* not a mountpoint */
            if (errno == EBUSY)
                umount2(c_str(), UMOUNT_NOFOLLOW | MNT_DETACH);
            else
                return TError(EError::Unknown, errno, "umount2(" + Path + ")");
        }
    }
}

TError TPath::ReadAll(std::string &text, size_t max) const {
    TError error;
    TFile file;

    error = file.OpenRead(*this);
    if (error)
        return error;

    error = file.ReadAll(text, max);
    if (error)
        return TError(error, Path);

    return TError::Success();
}

TError TPath::WriteAll(const std::string &text) const {
    TError error;
    TFile file;

    error = file.OpenTrunc(*this);
    if (error)
        return error;

    error = file.WriteAll(text);
    if (error)
        return TError(error, Path);

    return TError::Success();
}

TError TPath::WritePrivate(const std::string &text) const {
    TError error;
    TFile temp;

    if (!Exists()) {
        error = Mkfile(0644);
        if (error)
            return error;
    } else if (!IsRegularStrict())
        return TError(EError::InvalidValue, "non-regular file " + Path);

    error = temp.CreateTemp("/run");
    if (error)
        return error;

    if (fchmod(temp.Fd, 0644))
        return TError(EError::Unknown, errno, "fchmod");

    error = temp.WriteAll(text);
    if (error)
        return TError(error, Path);

    error = UmountAll();
    if (!error)
        error = Bind(temp.ProcPath());
    return error;
}

TError TPath::ReadLines(std::vector<std::string> &lines, size_t max) const {
    char *line = nullptr;
    size_t line_len = 0;
    size_t size = 0;
    ssize_t len;
    struct stat st;
    FILE *file;
    TError error;

    file = fopen(Path.c_str(), "r");
    if (!file)
        return TError(EError::Unknown, errno, "Cannot open for read: " + Path);

    if (fstat(fileno(file), &st) < 0) {
        error = TError(EError::Unknown, errno, "stat(" + Path + ")");
        goto out;
    }

    if (st.st_size > (off_t)max) {
        error = TError(EError::Unknown, "File too large: " + Path);
        goto out;
    }

    while ((len = getline(&line, &line_len, file)) >= 0) {
        size += len;
        if (size > max) {
            error = TError(EError::Unknown, "File too large: " + Path);
            goto out;
        }
        lines.push_back(std::string(line, len - 1));
    }

out:
    fclose(file);
    free(line);

    return error;
}

TError TPath::ReadInt(int &value) const {
    std::string text;
    TError error = ReadAll(text);
    if (!error)
        error = StringToInt(text, value);
    return error;
}

TError TPath::FindMount(TMount &mount) const {
    std::string mounts = "/proc/self/mounts";
    struct mntent* mnt, mntbuf;
    char buf[4096];
    FILE *file;

    auto device = GetDev();
    if (!device)
        return TError(EError::Unknown, "device not found: " + Path);

    file = setmntent(mounts.c_str(), "r");
    if (!file)
        return TError(EError::Unknown, errno, "setmntent " + mounts );

    TPath normal = NormalPath();
    bool found = false;
    while ((mnt = getmntent_r(file, &mntbuf, buf, sizeof buf))) {
        TPath source(mnt->mnt_fsname);
        TPath target(mnt->mnt_dir);

        if (!target.InnerPath(normal).IsEmpty() &&
                (target.GetDev() == device || source.GetBlockDev() == device)) {
            mount.Source = source;
            mount.Target = target;
            mount.Type = mnt->mnt_type;
            mount.Options = mnt->mnt_opts;
            found = true;
            /* get last matching mountpoint */
        }
    }
    endmntent(file);

    if (!found)
        return TError(EError::Unknown, "mountpoint not found: " + Path);

    return TError::Success();
}

TError TPath::ListAllMounts(std::list<TMount> &list) {
    std::string mounts = "/proc/self/mounts";
    struct mntent* mnt, mntbuf;
    char buf[4096];
    FILE* file;

    file = setmntent(mounts.c_str(), "r");
    if (!file)
        return TError(EError::Unknown, errno, "setmntent(" + mounts + ")");
    while ((mnt = getmntent_r(file, &mntbuf, buf, sizeof buf)))
        list.emplace_back(TMount{mnt->mnt_fsname, mnt->mnt_dir,
                                 mnt->mnt_type, mnt->mnt_opts});
    endmntent(file);
    return TError::Success();
}

bool TMount::HasOption(const std::string &option) const {
    std::string options = "," + Options + ",";
    std::string mask = "," + option + ",";
    return options.find(mask) != std::string::npos;
}

TError TFile::Open(const TPath &path, int flags) {
    if (Fd >= 0)
        close(Fd);
    SetFd = open(path.c_str(), flags);
    if (Fd < 0)
        return TError(EError::Unknown, errno, "Cannot open " + path.ToString());
    return TError::Success();
}

TError TFile::OpenRead(const TPath &path) {
    return Open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
}

TError TFile::OpenWrite(const TPath &path) {
    return Open(path, O_WRONLY | O_CLOEXEC | O_NOCTTY);
}

TError TFile::OpenReadWrite(const TPath &path) {
    return Open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
}

TError TFile::OpenAppend(const TPath &path) {
    return Open(path, O_WRONLY | O_CLOEXEC | O_APPEND | O_NOCTTY);
}

TError TFile::OpenTrunc(const TPath &path) {
    return Open(path, O_WRONLY | O_CLOEXEC | O_TRUNC | O_NOCTTY);
}

TError TFile::OpenDir(const TPath &path) {
    return Open(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOCTTY);
}

#ifndef O_TMPFILE
#define O_TMPFILE (O_DIRECTORY | 020000000)
#endif

TError TFile::CreateTemp(const TPath &path) {
    if (Fd >= 0)
        close(Fd);
    SetFd = open(path.c_str(), O_RDWR | O_TMPFILE | O_CLOEXEC, 0600);
    if (Fd < 0) {
        std::string temp = path.ToString() + "/porto.XXXXXX";
        SetFd = mkostemp(&temp[0], O_CLOEXEC);
        if (Fd < 0)
            return TError(EError::Unknown, errno, "Cannot create temporary " + temp);
        if (unlink(temp.c_str()))
            return TError(EError::Unknown, errno, "Cannot unlink " + temp);
    }
    return TError::Success();
}

TError TFile::CreateNew(const TPath &path, int mode) {
    if (Fd >= 0)
        close(Fd);
    SetFd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (Fd < 0)
        return TError(EError::Unknown, errno, "Cannot create " + path.ToString());
    return TError::Success();
}

void TFile::Close(void) {
    if (Fd >= 0)
        close(Fd);
    SetFd = -1;
}

void TFile::CloseAll(std::vector<int> except) {
    int max = getdtablesize();
    for (int fd = 0; fd < max; fd++) {
        if (std::find(except.begin(), except.end(), fd) == except.end())
            close(fd);
    }
}

TPath TFile::RealPath(void) const {
    TPath path;
    if (Fd >= 0)
        (void)ProcPath().ReadLink(path);
    return path;
}

TPath TFile::ProcPath(void) const {
    if (Fd < 0)
        return TPath();
    return TPath("/proc/self/fd/" + std::to_string(Fd));
}

TError TFile::ReadAll(std::string &text, size_t max) const {
    struct stat st;
    if (fstat(Fd, &st) < 0)
        return TError(EError::Unknown, errno, "fstat");

    if (st.st_size > (off_t)max)
        return TError(EError::Unknown, "File too large: " + std::to_string(st.st_size));

    size_t size = st.st_size;
    if (st.st_size < 4096)
        size = 4096;
    text.resize(size);

    size_t off = 0;
    ssize_t ret;
    do {
        if (size - off < 1024) {
            size += 16384;
            if (size > max)
                return TError(EError::Unknown, "File too large: " + std::to_string(size));
            text.resize(size);
        }
        ret = read(Fd, &text[off], size - off);
        if (ret < 0)
            return TError(EError::Unknown, errno, "read");
        off += ret;
    } while (ret > 0);

    text.resize(off);

    return TError::Success();
}

TError TFile::WriteAll(const std::string &text) const {
    size_t len = text.length(), off = 0;
    do {
        ssize_t ret = write(Fd, &text[off], len - off);
        if (ret < 0)
            return TError(EError::Unknown, errno, "write");
        off += ret;
    } while (off < len);

    return TError::Success();
}

TError TFile::Chattr(int fd, unsigned add_flags, unsigned del_flags) {
    unsigned old_flags, new_flags;

    if (ioctl(fd, FS_IOC_GETFLAGS, &old_flags))
        return TError(EError::Unknown, errno, "ioctlFS_IOC_GETFLAGS)");

    new_flags = (old_flags & ~del_flags) | add_flags;
    if ((new_flags != old_flags) && ioctl(fd, FS_IOC_SETFLAGS, &new_flags))
        return TError(EError::Unknown, errno, "ioctl(FS_IOC_SETFLAGS)");

    return TError::Success();
}

int TFile::GetMountId(void) const {
    FileHandle fh;
    int mnt;
    if (name_to_handle_at(Fd, "", &fh.head, &mnt, AT_EMPTY_PATH))
        return -1;
    return mnt;
}
