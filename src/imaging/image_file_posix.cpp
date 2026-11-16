// imaging's files on posix. tests/check_no_write_apis.cmake lets src/imaging/ write, and only here: a new image file,
// an image a matching map was made with, and the map itself replaced whole

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <new>
#include <utility>

#include "image_file.h"
#include "storage/file_image_source.h"

namespace {
	using namespace storage;

	std::unexpected<Error> from_errno(int error, const char* context) {
		const auto code = static_cast<std::uint32_t>(error);
		switch (error) {
		case ENOENT:
		case ENOTDIR:
		case ENAMETOOLONG:
			return fail(Errc::not_found, context, code);
		case EEXIST:
			return fail(Errc::exists, context, code);
		case EACCES:
		case EPERM:
		case EROFS:
			return fail(Errc::access_denied, context, code);
		case ENOSPC:
		case EDQUOT:
		case EFBIG:
			return fail(Errc::no_space, context, code);
		case ENOMEM:
			return fail(Errc::out_of_memory, context, code);
		default:
			return fail(Errc::io_error, context, code);
		}
	}

	struct t_fd {
		int value = -1;
		~t_fd() { if (value >= 0) ::close(value); }
		int release() { return std::exchange(value, -1); }
	};

	Result<void> write_all(int fd, std::uint64_t offset, std::span<const std::byte> data, const char* context) {
		std::size_t done = 0;
		while (done < data.size()) {
			const std::size_t chunk = std::min<std::size_t>(data.size() - done, std::size_t(1) << 30);
			const ssize_t wrote = ::pwrite(fd, data.data() + done, chunk, static_cast<off_t>(offset + done));
			if (wrote < 0) {
				if (errno == EINTR)
					continue;
				return from_errno(errno, context);
			}
			if (wrote == 0)
				return fail(Errc::io_error, context);
			done += static_cast<std::size_t>(wrote);
		}
		return {};
	}
}

storage::Result<storage::imaging::ImageFile> storage::imaging::ImageFile::create_new(const std::filesystem::path& path, std::uint64_t size) noexcept {
	if (path.empty() || is_device_path(path))
		return fail(Errc::unsupported, "creating the image: not a file path");
	if (size > static_cast<std::uint64_t>(LLONG_MAX))
		return fail(Errc::overflow, "creating the image");
	// O_EXCL: never a file that's already there, nor a link to one
	t_fd fd{ ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOCTTY, 0644) };
	if (fd.value < 0)
		return from_errno(errno, "creating the image");
	if (::ftruncate(fd.value, static_cast<off_t>(size)) != 0) {
		const int error = errno;
		::close(fd.release());
		::unlink(path.c_str()); // the empty file just created, nothing else
		return from_errno(error, "creating the image: making it the source's size");
	}
	ImageFile file;
	file.size_ = size;
	file.fd_ = fd.release();
	return file;
}

storage::Result<storage::imaging::ImageFile> storage::imaging::ImageFile::open_existing(const std::filesystem::path& path) noexcept {
	if (path.empty() || is_device_path(path))
		return fail(Errc::unsupported, "opening the image: not a file path");
	t_fd fd{ ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW) };
	if (fd.value < 0)
		return from_errno(errno, "opening the image");
	struct stat info {};
	if (::fstat(fd.value, &info) != 0)
		return from_errno(errno, "opening the image");
	if (!S_ISREG(info.st_mode) || info.st_size < 0)
		return fail(Errc::unsupported, "opening the image: not a regular file");
	ImageFile file;
	file.size_ = static_cast<std::uint64_t>(info.st_size);
	file.fd_ = fd.release();
	return file;
}

storage::imaging::ImageFile::ImageFile(ImageFile&& other) noexcept : size_(other.size_), fd_(std::exchange(other.fd_, -1)) {}

storage::imaging::ImageFile& storage::imaging::ImageFile::operator=(ImageFile&& other) noexcept {
	if (this != &other) {
		if (fd_ >= 0)
			::close(fd_);
		size_ = other.size_;
		fd_ = std::exchange(other.fd_, -1);
	}
	return *this;
}

storage::imaging::ImageFile::~ImageFile() {
	if (fd_ >= 0)
		::close(fd_);
}

storage::Result<void> storage::imaging::ImageFile::write_at(std::uint64_t offset, std::span<const std::byte> data) noexcept {
	if (offset > size_ || data.size() > size_ - offset)
		return fail(Errc::out_of_range, "writing the image: past its end");
	return write_all(fd_, offset, data, "writing the image");
}

storage::Result<void> storage::imaging::ImageFile::read_at(std::uint64_t offset, std::span<std::byte> data) noexcept {
	if (offset > size_ || data.size() > size_ - offset)
		return fail(Errc::out_of_range, "reading the image: past its end");
	std::size_t done = 0;
	while (done < data.size()) {
		const ssize_t got = ::pread(fd_, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
		if (got < 0) {
			if (errno == EINTR)
				continue;
			return from_errno(errno, "reading the image");
		}
		if (got == 0)
			return fail(Errc::io_error, "reading the image: it's shorter than it was");
		done += static_cast<std::size_t>(got);
	}
	return {};
}

storage::Result<void> storage::imaging::ImageFile::flush() noexcept {
	if (::fsync(fd_) != 0)
		return from_errno(errno, "flushing the image");
	return {};
}

std::filesystem::path storage::imaging::pending_path(const std::filesystem::path& path) {
	std::filesystem::path pending = path;
	pending += ".new";
	return pending;
}

storage::Result<void> storage::imaging::save_text(const std::filesystem::path& path, std::string_view text, bool replace) noexcept {
	try {
		if (path.empty() || is_device_path(path))
			return fail(Errc::unsupported, "saving the map: not a file path");
		const std::filesystem::path pending = pending_path(path);
		{
			t_fd fd{ ::open(pending.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOCTTY, 0644) };
			if (fd.value < 0)
				return from_errno(errno, "saving the map");
			Result<void> wrote = write_all(fd.value, 0, std::as_bytes(std::span(text)), "saving the map");
			if (wrote && ::fsync(fd.value) != 0)
				wrote = from_errno(errno, "saving the map: flushing it");
			if (!wrote) {
				::close(fd.release());
				::unlink(pending.c_str());
				return wrote;
			}
		}
		// replace: only ever over the map the copy is going on from. otherwise link() fails when path is taken
		if (replace ? ::rename(pending.c_str(), path.c_str()) != 0 : ::link(pending.c_str(), path.c_str()) != 0) {
			const int error = errno;
			::unlink(pending.c_str());
			return from_errno(error, "saving the map");
		}
		if (!replace)
			::unlink(pending.c_str());
		return {};
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "saving the map");
	}
}

storage::Result<void> storage::imaging::remove_pending(const std::filesystem::path& path) noexcept {
	if (::unlink(path.c_str()) != 0)
		return from_errno(errno, "removing an unfinished map");
	return {};
}
