// physical drives elsewhere than windows: not yet. nothing is listed or opened, and since nothing can read a drive,
// nothing needs to check a destination against one (it fails closed anyway)

#include "storage/disks.h"

std::optional<std::uint32_t> storage::physical_drive_number(const std::filesystem::path&) noexcept { return std::nullopt; }

storage::Result<std::vector<storage::DiskInfo>> storage::list_disks() noexcept {
	return fail(Errc::unsupported, "listing the drives: only on windows so far");
}

struct storage::DiskSource::t_impl {};

storage::Result<std::unique_ptr<storage::DiskSource>> storage::DiskSource::open(std::uint32_t, const DiskInfo*) noexcept {
	return fail(Errc::unsupported, "opening a drive: only on windows so far");
}

storage::DiskSource::~DiskSource() = default;

storage::Result<std::size_t> storage::DiskSource::read_at(std::uint64_t, std::span<std::byte>) noexcept {
	return fail(Errc::unsupported, "reading a drive: only on windows so far");
}

storage::Result<std::vector<std::uint32_t>> storage::disks_of(const std::filesystem::path&) noexcept {
	return fail(Errc::unsupported, "finding which drive a path is on: only on windows so far");
}

storage::Result<void> storage::check_destination(const std::filesystem::path&, std::uint32_t) noexcept {
	return fail(Errc::unsupported, "checking the destination: only on windows so far");
}
