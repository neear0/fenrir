#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "jobs.h"
#include "result.h"

// erasure, P7: deciding what a target can be sanitized with and what that can honestly be called, then doing it to a
// simulated drive or a disposable test image file. nothing here can reach a real drive: there's no backend for one,
// DiskSource has no way to write, and tests/check_no_write_apis.cmake fails on any api that could.
// the words follow NIST SP 800-88 as the development plan summarizes it (clear, purge; verification is not
// validation). the standard itself wasn't consulted for this build, and no conformance is claimed
namespace storage {
	// what a target is made of, which decides what any method can achieve on it
	enum class Media : std::uint8_t {
		hdd,          // magnetic
		sata_ssd,
		nvme_ssd,
		usb_flash,    // sticks and cards: a controller nobody can ask anything
		unknown,
		image_file,   // a disk image: a file on some filesystem, on some other drive
	};
	std::string_view to_string(Media media) noexcept;

	enum class Method : std::uint8_t {
		overwrite,             // every addressable sector written with a pattern, then read back
		ata_secure_erase,      // ATA SECURITY ERASE UNIT
		ata_sanitize_block,    // ATA SANITIZE BLOCK ERASE
		ata_sanitize_crypto,   // ATA SANITIZE CRYPTO SCRAMBLE
		nvme_sanitize_block,   // NVMe Sanitize, block erase
		nvme_sanitize_crypto,  // NVMe Sanitize, crypto erase
		nvme_sanitize_overwrite,
	};
	std::string_view to_string(Method method) noexcept;
	std::optional<Method> method_from(std::string_view name) noexcept; // as to_string() spells it

	// what a method achieves on a medium, at most: what it may be called, never more
	enum class Category : std::uint8_t {
		none,          // not media sanitization at all (a test image's bytes, a file)
		limited_clear, // overwritten where it can be reached, copies may remain where it can't (flash, remapped sectors)
		clear,
		purge,
	};
	std::string_view to_string(Category category) noexcept;

	// how an operation ended. plan rule 8: a command that was sent isn't one that finished, one that finished isn't
	// one that was checked, and one that was checked isn't one that's adequate
	enum class SanitizeStatus : std::uint8_t {
		completed,     // the method finished, its result wasn't read back
		verified,      // it finished and reading back found what it should have
		validated,     // someone decided the outcome is adequate for their requirements: never set by fenrir
		failed,        // it reported failure, or reading back found what was there before
		incomplete,    // it stopped with part of the target done
		unsupported,   // the target can't do it: nothing was done, and nothing else was tried instead
		interrupted,   // cancelled, power lost or the target disappeared partway
		indeterminate, // it can't be told whether it worked
	};
	std::string_view to_string(SanitizeStatus status) noexcept;

	// who the target is, compared before planning, at approval and again just before anything runs
	struct TargetIdentity {
		Media media = Media::unknown;
		std::string model, serial;           // a drive's; a test image's: empty
		std::filesystem::path path;          // a test image's, a simulated drive's name
		std::uint64_t size = 0;
		std::uint64_t modified = 0;          // a test image's last write time, as a count: it changes if anything writes it
		bool operator==(const TargetIdentity&) const = default;
	};

	// what the target says it can do, and the state it's in
	struct Capabilities {
		std::vector<Method> methods;     // what it supports
		std::uint32_t sector_size = 512;
		bool frozen = false;             // ATA security frozen: no security or sanitize command until a power cycle
		bool locked = false;             // ATA security locked
		bool system = false;             // windows, its boot files or a page file are on it
		bool mounted = false;            // a volume on it is mounted
		bool encrypted_from_start = false; // all data was ever only written encrypted (crypto erase's precondition)
	};

	struct Target {
		TargetIdentity identity;
		Capabilities capabilities;
	};

	struct SanitizeRequest {
		Method method = Method::overwrite;
		bool zeros = true;           // overwrite: zeros, else a pseudorandom pattern from seed
		std::uint64_t seed = 0;
		bool verify = true;          // read it back afterwards
	};

	// a dry run: what would be done, what it may be called, and what to confirm. nothing runs from a plan alone
	struct SanitizePlan {
		Target target;
		SanitizeRequest request;
		Category category = Category::none;
		std::string scope;                 // what's covered, in words
		std::vector<std::string> steps;    // what will happen, in order
		std::vector<std::string> caveats;  // what it can't promise
		std::string confirmation;          // what has to be typed to approve it
		bool cancellable = true;           // a firmware sanitize can't be stopped once it's accepted
	};

	// a refusal says why, every reason it has
	struct PlanRefusal {
		SanitizeStatus status = SanitizeStatus::unsupported;
		std::vector<std::string> reasons;
	};

	// decides, without touching the target. refused (with every reason) when: the method isn't one the target supports
	// (no other one is picked instead) or isn't sanitization on its media, the target is the system's or mounted, ATA
	// security is frozen or locked, or the target has no size
	std::variant<SanitizePlan, PlanRefusal> plan_sanitize(const Target& target, const SanitizeRequest& request);

	// what the matrix says a method achieves on a medium (Category::none when it doesn't apply there)
	Category category_of(Method method, Media media) noexcept;

	// the plan, approved: only authorize() makes one, after the confirmation matched and the target, inspected again,
	// is still the one the plan was made for
	class Authorization {
	public:
		const SanitizePlan& plan() const noexcept { return plan_; }

	private:
		friend Result<Authorization> authorize(const SanitizePlan& plan, const TargetIdentity& now, std::string_view typed);
		explicit Authorization(SanitizePlan plan) : plan_(std::move(plan)) {}
		SanitizePlan plan_;
	};

	// invalid_argument when typed isn't plan.confirmation exactly; mismatch when the target isn't who it was
	Result<Authorization> authorize(const SanitizePlan& plan, const TargetIdentity& now, std::string_view typed);

	struct Verification {
		bool done = false;
		std::uint64_t bytes_checked = 0;
		std::uint64_t mismatches = 0;      // sectors that didn't read back as they should
		std::uint64_t first_mismatch = 0;  // byte offset of the first
		std::string note;
	};

	struct SanitizeReport {
		SanitizePlan plan;
		SanitizeStatus status = SanitizeStatus::indeterminate;
		std::string started, finished;     // utc, iso 8601
		std::uint64_t bytes_done = 0;      // what the method covered, as far as it says
		std::vector<std::string> events;   // what happened, in order
		std::vector<std::string> errors;
		Verification verification;
		std::string validation = "not assessed: whether this is adequate is a decision about the data's sensitivity, made by its owner";
	};

	// the report as JSON: the plan, every step and error, verification and validation apart, and the caveats
	std::string to_json(const SanitizeReport& report);
	std::string iso_now(); // utc
}
