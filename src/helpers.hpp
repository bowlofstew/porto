#include <cgroup.hpp>
#include <string>
#include <vector>
#include "util/path.hpp"

TError RunCommand(const std::vector<std::string> &command, const TPath &cwd);
TError PackTarball(const TPath &tar, const TPath &path);
TError UnpackTarball(const TPath &tar, const TPath &path);
TError CopyRecursive(const TPath &src, const TPath &dst);
TError ResizeLoopDev(int loopNr, const TPath &image, off_t current, off_t target);
