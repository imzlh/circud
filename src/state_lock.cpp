#include <circud/state_lock.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace circud {
namespace {

    std::runtime_error lock_error(const std::filesystem::path& path,
                                  std::string_view operation, int error) {
        return std::runtime_error(std::string(operation) + " " + path.string() +
                                  ": " + std::strerror(error));
    }

} // namespace

    StateLock::StateLock(const std::filesystem::path& state_dir) {
        std::filesystem::create_directories(state_dir);
        const auto path = state_dir / "circud.lock";
        descriptor_ = ::open(path.c_str(),
            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (descriptor_ < 0)
            throw lock_error(path, "cannot open state lock", errno);
        struct stat metadata{};
        const int stat_result = ::fstat(descriptor_, &metadata);
        if (stat_result != 0 || !S_ISREG(metadata.st_mode)) {
            const int error = stat_result != 0 ? errno : EINVAL;
            ::close(descriptor_);
            descriptor_ = -1;
            throw lock_error(path, "state lock is not a regular file", error);
        }
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            ::close(descriptor_);
            descriptor_ = -1;
            if (error == EWOULDBLOCK || error == EAGAIN) {
                throw std::runtime_error(
                    "state directory is already in use by another circud: " +
                    state_dir.string());
            }
            throw lock_error(path, "cannot acquire state lock", error);
        }

        const auto owner = std::to_string(static_cast<long long>(::getpid())) +
                           "\n";
        bool recorded = ::ftruncate(descriptor_, 0) == 0;
        std::size_t written = 0;
        while (recorded && written < owner.size()) {
            const auto count = ::pwrite(descriptor_, owner.data() + written,
                owner.size() - written, static_cast<off_t>(written));
            if (count > 0) {
                written += static_cast<std::size_t>(count);
            }
            else if (count < 0 && errno == EINTR) {
                continue;
            }
            else {
                if (count == 0) errno = EIO;
                recorded = false;
            }
        }
        if (!recorded) {
            const int error = errno;
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
            descriptor_ = -1;
            throw lock_error(path, "cannot record state lock owner", error);
        }
    }

    StateLock::~StateLock() {
        if (descriptor_ < 0) return;
        ::flock(descriptor_, LOCK_UN);
        ::close(descriptor_);
    }

} // namespace circud
