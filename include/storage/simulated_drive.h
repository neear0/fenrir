#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "sanitize.h"

// a drive that exists only in memory, to try every erasure workflow and the ways it goes wrong on: what a real drive
// does as far as fenrir can see it (identity, capabilities, a sanitize that runs to a status, sectors read back), with
// faults to order. nothing in it reaches any hardware
namespace storage {
	struct SimulatedFaults {
		std::optional<double> fail_at;        // the drive reports failure when this share of the work is done (0..1)
		std::optional<double> power_loss_at;  // power lost then: interrupted. a firmware sanitize finishes at the next power on
		bool lies = false;                    // reports success without erasing anything
		bool unreadable_after = false;        // reads fail once it's done: what it did can't be checked
	};

	class SimulatedDrive final : public ISanitizationBackend {
	public:
		// a profile: "hdd", "sata-ssd", "nvme-ssd" or "usb-flash", 4 MiB, full of data. nothing for another name
		static std::optional<SimulatedDrive> make(std::string_view profile, SimulatedFaults faults = {});
		static std::vector<std::string_view> profiles();

		SimulatedDrive(Target target, std::vector<std::byte> contents, SimulatedFaults faults = {});

		Result<Target> inspect() override;
		Result<SanitizeReport> execute(const Authorization& approved, const CancellationToken* cancel = nullptr, ProgressSink* progress = nullptr) override;

		// what's on it now
		std::span<const std::byte> contents() const noexcept { return contents_; }
		// another drive is plugged in where this one was
		void swap(TargetIdentity other) { target_.identity = std::move(other); }
		// the state a real drive would be in, which inspect() reports
		Capabilities& capabilities() noexcept { return target_.capabilities; }
		// a firmware sanitize the power cut off goes on when the drive is powered again: true when one was pending
		bool power_on();
		bool sanitize_pending() const noexcept { return pending_.has_value(); }

	private:
		void erase(Method method, std::uint64_t from, std::uint64_t to, std::uint64_t key);

		Target target_;
		std::vector<std::byte> contents_;
		SimulatedFaults faults_;
		std::optional<Method> pending_;
		std::uint64_t key_ = 1; // its "encryption key": a crypto erase changes it, and every sector reads differently
	};

	namespace sanitation {
		// what an overwrite writes at offset: zeros, or a pseudorandom pattern that depends only on the seed and the
		// offset, so reading back can tell which byte should be what
		void pattern(std::span<std::byte> out, std::uint64_t offset, const SanitizeRequest& request) noexcept;
	}
}
