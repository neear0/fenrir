// the simulated drive: identity, capabilities and a sanitize that runs to a status, in memory, with faults to order

#include <algorithm>
#include <format>

#include "storage/simulated_drive.h"

namespace {
	using namespace storage;

	constexpr std::uint64_t simulated_size = 4u << 20;
	constexpr std::size_t samples = 64;   // sectors kept from before a firmware sanitize, to check they changed
	constexpr std::size_t chunk = 64 * 1024;

	std::uint64_t mix(std::uint64_t x) noexcept { // splitmix64
		x += 0x9E3779B97F4A7C15ull;
		x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
		x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
		return x ^ (x >> 31);
	}

	bool zeroing(Method m) { return m == Method::ata_secure_erase || m == Method::ata_sanitize_block || m == Method::nvme_sanitize_block || m == Method::nvme_sanitize_overwrite; }
	bool crypto(Method m) { return m == Method::ata_sanitize_crypto || m == Method::nvme_sanitize_crypto; }
}

void storage::sanitation::pattern(std::span<std::byte> out, std::uint64_t offset, const SanitizeRequest& request) noexcept {
	if (request.zeros) {
		std::ranges::fill(out, std::byte{ 0 });
		return;
	}
	std::uint64_t word_index = offset / 8, word = mix(request.seed ^ mix(word_index));
	for (std::size_t i = 0; i < out.size(); i++) {
		const std::uint64_t at = offset + i;
		if (at / 8 != word_index) {
			word_index = at / 8;
			word = mix(request.seed ^ mix(word_index));
		}
		out[i] = static_cast<std::byte>(word >> (8 * (at % 8)));
	}
}

std::vector<std::string_view> storage::SimulatedDrive::profiles() { return { "hdd", "sata-ssd", "nvme-ssd", "usb-flash" }; }

std::optional<storage::SimulatedDrive> storage::SimulatedDrive::make(std::string_view profile, SimulatedFaults faults) {
	Target t;
	t.identity.size = simulated_size;
	t.identity.path = std::string("sim:") + std::string(profile);
	if (profile == "hdd") {
		t.identity = { Media::hdd, "FENRIR SIMULATED HDD", "SIM-HDD-000001", t.identity.path, simulated_size, 0 };
		t.capabilities.methods = { Method::overwrite, Method::ata_secure_erase };
	} else if (profile == "sata-ssd") {
		t.identity = { Media::sata_ssd, "FENRIR SIMULATED SATA SSD", "SIM-SATA-000002", t.identity.path, simulated_size, 0 };
		t.capabilities.methods = { Method::overwrite, Method::ata_secure_erase, Method::ata_sanitize_block, Method::ata_sanitize_crypto };
	} else if (profile == "nvme-ssd") {
		t.identity = { Media::nvme_ssd, "FENRIR SIMULATED NVME SSD", "SIM-NVME-000003", t.identity.path, simulated_size, 0 };
		t.capabilities.methods = { Method::overwrite, Method::nvme_sanitize_block, Method::nvme_sanitize_crypto, Method::nvme_sanitize_overwrite };
	} else if (profile == "usb-flash") {
		t.identity = { Media::usb_flash, "FENRIR SIMULATED USB STICK", "SIM-USB-000004", t.identity.path, simulated_size, 0 };
		t.capabilities.methods = { Method::overwrite };
	} else
		return std::nullopt;
	std::vector<std::byte> data(simulated_size);
	sanitation::pattern(data, 0, { .zeros = false, .seed = 0xDA7A }); // "data": anything that isn't zeros or a pattern it'll be erased with
	return SimulatedDrive(std::move(t), std::move(data), faults);
}

storage::SimulatedDrive::SimulatedDrive(Target target, std::vector<std::byte> contents, SimulatedFaults faults)
	: target_(std::move(target)), contents_(std::move(contents)), faults_(faults) {
	target_.identity.size = contents_.size();
}

storage::Result<storage::Target> storage::SimulatedDrive::inspect() { return target_; }

void storage::SimulatedDrive::erase(Method method, std::uint64_t from, std::uint64_t to, std::uint64_t key) {
	const std::span<std::byte> part = std::span(contents_).subspan(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
	if (zeroing(method))
		std::ranges::fill(part, std::byte{ 0 });
	else if (crypto(method)) // a new key: what's there reads as something else entirely
		sanitation::pattern(part, from, { .zeros = false, .seed = mix(key) });
}

bool storage::SimulatedDrive::power_on() {
	if (!pending_)
		return false;
	erase(*pending_, 0, contents_.size(), ++key_);
	pending_.reset();
	return true;
}

storage::Result<storage::SanitizeReport> storage::SimulatedDrive::execute(const Authorization& approved, const CancellationToken* cancel, ProgressSink* progress) {
	const SanitizePlan& plan = approved.plan();
	Result<Target> now = inspect();
	if (!now)
		return std::unexpected(now.error());
	if (now->identity != plan.target.identity)
		return fail(Errc::mismatch, "erasing: the drive isn't the one the plan was approved for, nothing was done");

	SanitizeReport r;
	r.plan = plan;
	r.started = iso_now();
	r.events.push_back("checked the drive again: it's the one the plan was approved for");
	const std::uint64_t size = contents_.size();
	const std::uint32_t sector = std::max<std::uint32_t>(plan.target.capabilities.sector_size, 1);
	const Method m = plan.request.method;
	auto end = [&](SanitizeStatus status) {
		r.status = status;
		r.finished = iso_now();
		return r;
	};

	// sectors as they were, for checking a firmware sanitize changed them
	std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> before;
	if (m != Method::overwrite)
		for (std::size_t i = 0; i < samples; i++) {
			const std::uint64_t at = (size / sector) * i / samples * sector;
			before.emplace_back(at, std::vector<std::byte>(contents_.begin() + static_cast<std::ptrdiff_t>(at), contents_.begin() + static_cast<std::ptrdiff_t>(at + sector)));
		}

	if (m == Method::overwrite) {
		r.events.push_back(std::format("writing {} over every sector", plan.request.zeros ? "zeros" : "the pattern"));
		for (std::uint64_t pos = 0; pos < size; pos += chunk) {
			const double done = static_cast<double>(pos) / static_cast<double>(size);
			if (cancel && cancel->cancelled()) {
				r.events.push_back(std::format("stopped at byte {}, as asked: what's past it wasn't written", pos));
				return end(SanitizeStatus::interrupted);
			}
			if (faults_.fail_at && done >= *faults_.fail_at) {
				r.errors.push_back(std::format("writing at byte {} failed: the drive reported an error", pos));
				return end(SanitizeStatus::failed);
			}
			if (faults_.power_loss_at && done >= *faults_.power_loss_at) {
				r.errors.push_back(std::format("the drive lost power at byte {}: what's past it wasn't written", pos));
				return end(SanitizeStatus::interrupted);
			}
			const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size - pos));
			if (!faults_.lies)
				sanitation::pattern(std::span(contents_).subspan(static_cast<std::size_t>(pos), length), pos, plan.request);
			r.bytes_done = pos + length;
			if (progress)
				progress->progress(r.bytes_done, size);
		}
		r.events.push_back("every sector was written and flushed");
	} else {
		r.events.push_back(std::format("sent {}: the drive accepted it", to_string(m)));
		bool asked = false;
		for (int step = 0; step < 100; step++) {
			const double done = step / 100.0;
			if (cancel && cancel->cancelled() && !asked) {
				r.events.push_back("stopping was asked for, but a sanitize the drive has accepted can't be stopped: it went on");
				asked = true;
			}
			const std::uint64_t reached = size * static_cast<std::uint64_t>(step) / 100 / sector * sector;
			if (faults_.fail_at && done >= *faults_.fail_at) {
				if (!faults_.lies)
					erase(m, 0, reached, ++key_);
				r.errors.push_back("the drive's sanitize status says it failed");
				return end(SanitizeStatus::failed);
			}
			if (faults_.power_loss_at && done >= *faults_.power_loss_at) {
				if (!faults_.lies)
					erase(m, 0, reached, ++key_);
				pending_ = m;
				r.errors.push_back("the drive lost power while it ran: it goes on with it the next time it's powered, and only then can it be checked");
				return end(SanitizeStatus::interrupted);
			}
			if (progress)
				progress->progress(static_cast<std::uint64_t>(step), 100);
		}
		if (!faults_.lies)
			erase(m, 0, size, ++key_);
		r.bytes_done = size;
		r.events.push_back("the drive's sanitize status says it completed");
	}

	if (!plan.request.verify) {
		r.verification.note = "not read back, as the plan said: it completed as far as the drive says, nothing more is known";
		return end(SanitizeStatus::completed);
	}
	if (faults_.unreadable_after) {
		r.errors.push_back("reading it back failed");
		r.verification.note = "it couldn't be read back: whether it worked can't be told";
		return end(SanitizeStatus::indeterminate);
	}

	Verification& v = r.verification;
	v.done = true;
	auto mismatch = [&](std::uint64_t at) {
		if (v.mismatches++ == 0)
			v.first_mismatch = at;
	};
	if (m == Method::overwrite || zeroing(m)) {
		std::vector<std::byte> expected(sector);
		const SanitizeRequest zeros{ .zeros = true };
		for (std::uint64_t at = 0; at < size; at += sector) {
			sanitation::pattern(expected, at, m == Method::overwrite ? plan.request : zeros);
			if (!std::equal(expected.begin(), expected.end(), contents_.begin() + static_cast<std::ptrdiff_t>(at)))
				mismatch(at);
			v.bytes_checked += sector;
		}
		v.note = m == Method::overwrite ? "every sector was read back and compared with the pattern"
			: "every sector was read back and compared with zeros (what this simulated drive's erase leaves; a real one may leave ones or a vendor pattern)";
	} else {
		for (const auto& [at, old] : before) {
			if (std::equal(old.begin(), old.end(), contents_.begin() + static_cast<std::ptrdiff_t>(at)))
				mismatch(at);
			v.bytes_checked += old.size();
		}
		v.note = std::format("{} sectors spread over the drive were read before and after: none should hold what it held. that can't show the old key is gone", before.size());
	}
	if (v.mismatches != 0) {
		r.errors.push_back(std::format("{} sectors didn't read back as an erased drive would, the first at byte {}", v.mismatches, v.first_mismatch));
		return end(SanitizeStatus::failed);
	}
	return end(SanitizeStatus::verified);
}
