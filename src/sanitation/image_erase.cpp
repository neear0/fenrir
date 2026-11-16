// overwriting a test image: it opens the file through imaging's ImageFile::open_existing, the reviewed write path that
// never creates, truncates or replaces anything, so there's no write api in src/sanitation/ itself

#include <algorithm>
#include <format>
#include <memory>
#include <new>
#include <system_error>

#include "../imaging/image_file.h"
#include "storage/disks.h"
#include "storage/file_image_source.h"
#include "storage/image_erase.h"
#include "storage/simulated_drive.h"

namespace {
	using namespace storage;
	namespace fs = std::filesystem;
	constexpr std::size_t chunk = std::size_t(1) << 20;
}

storage::Result<storage::Target> storage::TestImageBackend::inspect() {
	if (physical_drive_number(path_) || is_device_path(path_))
		return fail(Errc::unsupported, "erasing: drives can't be erased in this build, only test image files");
	std::error_code error;
	const fs::file_status status = fs::symlink_status(path_, error);
	if (error || !fs::exists(status))
		return fail(Errc::not_found, "erasing: there's no file there");
	if (!fs::is_regular_file(status))
		return fail(Errc::unsupported, "erasing: only a regular file can be a test image (not a directory, link or device)");
	const std::uint64_t size = fs::file_size(path_, error);
	const std::optional<std::uint64_t> modified = file_modified(path_);
	if (error || !modified)
		return fail(Errc::io_error, "erasing: reading the file's size and time", static_cast<std::uint32_t>(error.value()));

	Target t;
	t.identity.media = Media::image_file;
	t.identity.path = fs::absolute(path_, error);
	t.identity.size = size;
	t.identity.modified = *modified;
	t.capabilities.methods = { Method::overwrite };
	t.capabilities.sector_size = size % 512 == 0 ? 512 : 1;
	return t;
}

storage::Result<storage::SanitizeReport> storage::TestImageBackend::execute(const Authorization& approved, const CancellationToken* cancel, ProgressSink* progress) {
	try {
		const SanitizePlan& plan = approved.plan();
		if (plan.target.identity.media != Media::image_file || plan.request.method != Method::overwrite)
			return fail(Errc::unsupported, "erasing: a test image can only be overwritten");
		Result<Target> now = inspect();
		if (!now)
			return std::unexpected(now.error());
		if (now->identity != plan.target.identity)
			return fail(Errc::mismatch, "erasing: the file isn't the one the plan was approved for (it changed, or another one is there), nothing was done");

		// another program writing it, or fenrir reading it as an image, makes this fail with busy: nothing is done then
		Result<imaging::ImageFile> file = imaging::ImageFile::open_existing(path_);
		if (!file)
			return std::unexpected(file.error());
		const std::uint64_t size = file->size();
		if (size != plan.target.identity.size)
			return fail(Errc::mismatch, "erasing: the file isn't the size it was planned at, nothing was done");

		SanitizeReport r;
		r.plan = plan;
		r.started = iso_now();
		r.events.push_back("checked the file again: it's the one the plan was approved for, unchanged since");
		auto end = [&](SanitizeStatus status) {
			r.status = status;
			r.finished = iso_now();
			return r;
		};

		std::unique_ptr<std::byte[]> buffer(new std::byte[chunk]);
		std::unique_ptr<std::byte[]> expected(new std::byte[chunk]);
		r.events.push_back(std::format("writing {} over all of it", plan.request.zeros ? "zeros" : "the pattern"));
		for (std::uint64_t pos = 0; pos < size;) {
			if (cancel && cancel->cancelled()) {
				r.events.push_back(std::format("stopped at byte {}, as asked: what's past it wasn't written", pos));
				(void)file->flush();
				return end(SanitizeStatus::interrupted);
			}
			const std::span<std::byte> part(buffer.get(), static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size - pos)));
			sanitation::pattern(part, pos, plan.request);
			if (Result<void> wrote = file->write_at(pos, part); !wrote) {
				r.errors.push_back(std::format("writing at byte {} failed: {} (os error {})", pos, to_string(wrote.error().code), wrote.error().os_error));
				return end(SanitizeStatus::failed);
			}
			pos += part.size();
			r.bytes_done = pos;
			if (progress)
				progress->progress(pos, plan.request.verify ? size * 2 : size);
		}
		if (Result<void> flushed = file->flush(); !flushed) {
			r.errors.push_back(std::format("flushing failed: {} (os error {})", to_string(flushed.error().code), flushed.error().os_error));
			return end(SanitizeStatus::failed);
		}
		r.events.push_back("all of it was written and flushed");

		if (!plan.request.verify) {
			r.verification.note = "not read back, as the plan said";
			return end(SanitizeStatus::completed);
		}
		Verification& v = r.verification;
		v.done = true;
		for (std::uint64_t pos = 0; pos < size;) {
			const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size - pos));
			const std::span<std::byte> part(buffer.get(), length);
			if (Result<void> read = file->read_at(pos, part); !read) {
				r.errors.push_back(std::format("reading back at byte {} failed", pos));
				v.note = "it couldn't all be read back: whether it worked can't be told";
				return end(SanitizeStatus::indeterminate);
			}
			sanitation::pattern(std::span(expected.get(), length), pos, plan.request);
			for (std::size_t at = 0; at < length; at += 512) {
				const std::size_t n = std::min<std::size_t>(512, length - at);
				if (!std::equal(part.begin() + static_cast<std::ptrdiff_t>(at), part.begin() + static_cast<std::ptrdiff_t>(at + n), expected.get() + at)) {
					if (v.mismatches++ == 0)
						v.first_mismatch = pos + at;
				}
			}
			pos += length;
			v.bytes_checked = pos;
			if (progress)
				progress->progress(size + pos, size * 2);
		}
		v.note = "every byte was read back through the os and compared with the pattern. the os may have answered from its cache, it says nothing about the drive underneath";
		if (v.mismatches != 0) {
			r.errors.push_back(std::format("{} sectors didn't read back as written, the first at byte {}", v.mismatches, v.first_mismatch));
			return end(SanitizeStatus::failed);
		}
		return end(SanitizeStatus::verified);
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "erasing");
	}
}
