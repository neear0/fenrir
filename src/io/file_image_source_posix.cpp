#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <new>

#include "storage/file_image_source.h"

static_assert(sizeof(off_t) >= 8, "64-bit file offsets are needed, build with _FILE_OFFSET_BITS=64");

namespace {
	std::unexpected<storage::Error> from_errno(int error, const char* context) {
		using storage::Errc;
		switch (error) {
		case ENOENT:
		case ENOTDIR:
		case ENAMETOOLONG:
			return storage::fail(Errc::not_found, context, static_cast<std::uint32_t>(error));
		case EACCES:
		case EPERM:
			return storage::fail(Errc::access_denied, context, static_cast<std::uint32_t>(error));
		case ENOMEM:
			return storage::fail(Errc::out_of_memory, context, static_cast<std::uint32_t>(error));
		default:
			return storage::fail(Errc::io_error, context, static_cast<std::uint32_t>(error));
		}
	}

	struct t_fd { // closes it unless it's handed on
		int value = -1;
		~t_fd() { if (value >= 0) ::close(value); }
		int release() { const int v = value; value = -1; return v; }
	};
}

storage::Result<std::unique_ptr<storage::FileImageSource>> storage::FileImageSource::open(const std::filesystem::path& path) noexcept {
	if (path.empty())
		return fail(Errc::invalid_argument, "opening an image: the path is empty");

	t_fd fd{ ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY) };
	if (fd.value < 0)
		return from_errno(errno, "opening an image");

	struct stat info {};
	if (::fstat(fd.value, &info) != 0)
		return from_errno(errno, "opening an image: reading its attributes");
	if (!S_ISREG(info.st_mode)) // block devices too, they're for a separate device source
		return fail(Errc::unsupported, "opening an image: not a regular file");
	if (info.st_size < 0)
		return fail(Errc::io_error, "opening an image: reading its size");

	std::unique_ptr<FileImageSource> source(new (std::nothrow) FileImageSource());
	if (!source)
		return fail(Errc::out_of_memory, "opening an image");
	source->size_ = static_cast<std::uint64_t>(info.st_size);
	source->fd_ = fd.release();
	return source;
}

storage::FileImageSource::~FileImageSource() {
	if (fd_ >= 0)
		::close(fd_);
}

std::uint64_t storage::FileImageSource::size_bytes() const noexcept { return size_; }

storage::Result<std::size_t> storage::FileImageSource::read_at(std::uint64_t offset, std::span<std::byte> output) noexcept {
	if (offset > size_)
		return fail(Errc::out_of_range, "reading the image: the offset is past its end");
	const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), size_ - offset));

	std::size_t done = 0;
	while (done < want) {
		const std::size_t chunk = std::min<std::size_t>(want - done, std::size_t(1) << 30);
		const std::uint64_t at = offset + done; // <= size_, which came from a non-negative off_t
		const ssize_t got = ::pread(fd_, output.data() + done, chunk, static_cast<off_t>(at));
		if (got < 0) {
			if (errno == EINTR)
				continue;
			return from_errno(errno, "reading the image");
		}
		if (got == 0) // nothing stops another program from truncating it here
			return fail(Errc::io_error, "reading the image: it's shorter than when it was opened");
		done += static_cast<std::size_t>(got);
	}
	return done;
}

bool storage::is_device_path(const std::filesystem::path& path) noexcept {
	// the image source checks the file type after opening, this is for paths that aren't opened, like a destination
	const std::string& p = path.native();
	return p.starts_with("/dev/") || p.starts_with("/proc/") || p.starts_with("/sys/");
}
