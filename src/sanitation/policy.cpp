// the decision matrix and the plan: what a method can be called on a medium, what can't be promised, what has to be
// typed to approve it. nothing here touches a target

#include <algorithm>
#include <chrono>
#include <format>

#include "storage/sanitize.h"

namespace {
	using namespace storage;

	bool is_ata(Method m) { return m == Method::ata_secure_erase || m == Method::ata_sanitize_block || m == Method::ata_sanitize_crypto; }
	bool is_crypto(Method m) { return m == Method::ata_sanitize_crypto || m == Method::nvme_sanitize_crypto; }
	bool is_firmware(Method m) { return m != Method::overwrite; }

	std::string human(std::uint64_t bytes) {
		const char* units[] = { "bytes", "KiB", "MiB", "GiB", "TiB", "PiB" };
		double v = static_cast<double>(bytes);
		int u = 0;
		while (v >= 1024.0 && u < 5) {
			v /= 1024.0;
			u++;
		}
		return u == 0 ? std::format("{} bytes", bytes) : std::format("{:.2f} {}", v, units[u]);
	}

	std::string name_of(const TargetIdentity& id) {
		if (id.media == Media::image_file) {
			const std::u8string name = id.path.filename().u8string();
			return std::string(reinterpret_cast<const char*>(name.data()), name.size());
		}
		return id.model.empty() ? "the drive" : id.model;
	}

	// a drive: its serial's last 6 characters, a test image: its file name. never something that's on screen as a default
	std::string confirmation_for(const TargetIdentity& id) {
		if (id.media == Media::image_file)
			return "ERASE " + name_of(id);
		const std::string& key = id.serial.empty() ? id.model : id.serial;
		return "ERASE " + (key.size() > 6 ? key.substr(key.size() - 6) : key);
	}
}

std::string_view storage::to_string(Media media) noexcept {
	switch (media) {
	case Media::hdd: return "hard disk";
	case Media::sata_ssd: return "SATA SSD";
	case Media::nvme_ssd: return "NVMe SSD";
	case Media::usb_flash: return "USB flash";
	case Media::unknown: return "unknown";
	case Media::image_file: return "test image file";
	}
	return "?";
}

std::string_view storage::to_string(Method method) noexcept {
	switch (method) {
	case Method::overwrite: return "overwrite";
	case Method::ata_secure_erase: return "ata-secure-erase";
	case Method::ata_sanitize_block: return "ata-sanitize-block";
	case Method::ata_sanitize_crypto: return "ata-sanitize-crypto";
	case Method::nvme_sanitize_block: return "nvme-sanitize-block";
	case Method::nvme_sanitize_crypto: return "nvme-sanitize-crypto";
	case Method::nvme_sanitize_overwrite: return "nvme-sanitize-overwrite";
	}
	return "?";
}

std::optional<storage::Method> storage::method_from(std::string_view name) noexcept {
	for (Method m : { Method::overwrite, Method::ata_secure_erase, Method::ata_sanitize_block, Method::ata_sanitize_crypto, Method::nvme_sanitize_block,
			 Method::nvme_sanitize_crypto, Method::nvme_sanitize_overwrite })
		if (to_string(m) == name)
			return m;
	return std::nullopt;
}

std::string_view storage::to_string(Category category) noexcept {
	switch (category) {
	case Category::none: return "none";
	case Category::limited_clear: return "limited clear";
	case Category::clear: return "clear";
	case Category::purge: return "purge";
	}
	return "?";
}

std::string_view storage::to_string(SanitizeStatus status) noexcept {
	switch (status) {
	case SanitizeStatus::completed: return "completed";
	case SanitizeStatus::verified: return "verified";
	case SanitizeStatus::validated: return "validated";
	case SanitizeStatus::failed: return "failed";
	case SanitizeStatus::incomplete: return "incomplete";
	case SanitizeStatus::unsupported: return "unsupported";
	case SanitizeStatus::interrupted: return "interrupted";
	case SanitizeStatus::indeterminate: return "indeterminate";
	}
	return "?";
}

// conservative on purpose: where implementations vary, the lower category
storage::Category storage::category_of(Method method, Media media) noexcept {
	switch (method) {
	case Method::overwrite:
		switch (media) {
		case Media::hdd:
		case Media::sata_ssd:
		case Media::nvme_ssd: return Category::clear;
		case Media::usb_flash:
		case Media::unknown: return Category::limited_clear;
		case Media::image_file: return Category::none;
		}
		return Category::none;
	case Method::ata_secure_erase:
		return media == Media::hdd ? Category::purge : media == Media::sata_ssd ? Category::clear : Category::none;
	case Method::ata_sanitize_block:
		return media == Media::sata_ssd ? Category::purge : Category::none;
	case Method::ata_sanitize_crypto:
		return media == Media::sata_ssd || media == Media::hdd ? Category::purge : Category::none;
	case Method::nvme_sanitize_block:
	case Method::nvme_sanitize_crypto:
	case Method::nvme_sanitize_overwrite:
		return media == Media::nvme_ssd ? Category::purge : Category::none;
	}
	return Category::none;
}

std::variant<storage::SanitizePlan, storage::PlanRefusal> storage::plan_sanitize(const Target& target, const SanitizeRequest& request) {
	const TargetIdentity& id = target.identity;
	const Capabilities& caps = target.capabilities;
	const Method m = request.method;
	PlanRefusal refusal;

	if (id.size == 0)
		refusal.reasons.push_back("it has no size: there's nothing to erase, or it couldn't be read");
	if (caps.sector_size == 0 || id.size % caps.sector_size != 0)
		refusal.reasons.push_back("its size isn't a whole number of its sectors");
	if (std::ranges::find(caps.methods, m) == caps.methods.end())
		refusal.reasons.push_back(std::format("it doesn't support {}. nothing else is tried instead: pick a method it supports", to_string(m)));
	else if (category_of(m, id.media) == Category::none && id.media != Media::image_file)
		refusal.reasons.push_back(std::format("{} isn't a sanitization method for a {}", to_string(m), to_string(id.media)));
	if (id.media == Media::image_file && m != Method::overwrite)
		refusal.reasons.push_back("a test image file can only be overwritten");
	for (const std::string& blocker : caps.blockers)
		refusal.reasons.push_back(blocker);
	if (caps.system)
		refusal.reasons.push_back("windows, its boot files or a page file are on it: that needs a separate offline workflow, which fenrir doesn't have");
	if (caps.mounted)
		refusal.reasons.push_back("a volume on it is mounted: take it offline first, so nothing is writing to it during and after");
	if (is_ata(m) && caps.frozen)
		refusal.reasons.push_back("its ATA security is frozen (most BIOSes do that at boot): it refuses security and sanitize commands until it's power cycled");
	if (is_ata(m) && caps.locked)
		refusal.reasons.push_back("its ATA security is locked: it needs its password first");
	if (!refusal.reasons.empty())
		return refusal;

	SanitizePlan plan;
	plan.target = target;
	plan.request = request;
	plan.category = category_of(m, id.media);
	plan.cancellable = !is_firmware(m);
	plan.confirmation = confirmation_for(id);
	const std::string name = name_of(id);
	plan.scope = id.media == Media::image_file
		? std::format("every byte of the file {}, {} ({} bytes)", name, human(id.size), id.size)
		: std::format("all of {}, {} ({} sectors of {} bytes)", name, human(id.size), id.size / caps.sector_size, caps.sector_size);

	plan.steps.push_back("check again that the target is the one this plan was made for, and stop if it isn't");
	if (!caps.volumes.empty()) {
		std::string names;
		for (const std::string& v : caps.volumes)
			names += (names.empty() ? "" : ", ") + v;
		plan.steps.push_back(std::format("take its volumes offline ({}): lock and dismount each one, and stop with nothing written if one is in use", names));
	}
	if (m == Method::overwrite) {
		plan.steps.push_back(std::format("write {} over every sector, from the first to the last", request.zeros ? "zeros" : std::format("a pseudorandom pattern (seed {})", request.seed)));
		plan.steps.push_back("flush, so the writes have reached it");
	} else {
		plan.steps.push_back(std::format("send {} and wait for it to say it's done, however long that takes", to_string(m)));
		plan.steps.push_back("read its sanitize status, and take only a successful end as completion");
	}
	if (request.verify)
		plan.steps.push_back(m == Method::overwrite ? "read every sector back and compare it with the pattern"
			: is_crypto(m) ? "read sectors back and check none of them still holds what it held before"
			: "read sectors back and check they're erased");
	else
		plan.steps.push_back("no read back: the result will say completed, not verified");
	plan.steps.push_back("write a report: what was done, what was checked, and what wasn't");

	// what it can't promise
	plan.caveats.push_back("categories follow the development plan's summary of NIST SP 800-88: no conformance with the standard is claimed");
	plan.caveats.push_back("verification says the method did what it should; validation, whether that's enough for this data, is a separate decision this report doesn't make");
	if (id.media == Media::image_file)
		plan.caveats.push_back("this only overwrites a test image's bytes. the drive the file is on can keep older copies of it (journals, snapshots, copy-on-write, SSD wear leveling): it's for testing fenrir, not for sanitizing anything");
	if (m == Method::overwrite && id.media == Media::hdd)
		plan.caveats.push_back("sectors the drive has remapped keep what they held: writing can't reach them");
	if (m == Method::overwrite && (id.media == Media::sata_ssd || id.media == Media::nvme_ssd))
		plan.caveats.push_back("an SSD keeps spare and remapped blocks writes can't reach: overwriting is clear, never purge, on one");
	if (m == Method::overwrite && (id.media == Media::usb_flash || id.media == Media::unknown))
		plan.caveats.push_back("a flash controller nobody can ask anything decides where writes go: old data may stay in blocks it doesn't show. at best a limited clear");
	if (m == Method::ata_secure_erase && id.media == Media::sata_ssd)
		plan.caveats.push_back("SECURITY ERASE UNIT on an SSD does what its firmware does, which varies by model: counted as clear, not purge");
	if (is_crypto(m) && !caps.encrypted_from_start)
		plan.caveats.push_back("crypto erase only works if the drive encrypted everything it ever stored with the key it destroys, and no copy of that key exists");
	if (m == Method::nvme_sanitize_block || m == Method::nvme_sanitize_crypto || m == Method::nvme_sanitize_overwrite)
		plan.caveats.push_back("NVMe sanitize applies to the whole drive, every namespace on it, not to one");
	if (!plan.cancellable)
		plan.caveats.push_back("once the drive accepts it, it can't be stopped: it goes on after a reset or power loss until it's done");
	return plan;
}

storage::Result<storage::Authorization> storage::authorize(const SanitizePlan& plan, const TargetIdentity& now, std::string_view typed) {
	if (typed != plan.confirmation)
		return fail(Errc::invalid_argument, "approving the plan: what was typed isn't the confirmation it asks for");
	if (now != plan.target.identity)
		return fail(Errc::mismatch, "approving the plan: the target isn't the one the plan was made for any more");
	return Authorization(plan);
}

std::string storage::iso_now() {
	return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}
