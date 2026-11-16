#include <doctest/doctest.h>

#include <algorithm>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "storage/sanitize.h"
#include "storage/simulated_drive.h"

using storage::Category;
using storage::Errc;
using storage::Method;
using storage::SanitizeStatus;

namespace {
	storage::SanitizePlan planned(const std::variant<storage::SanitizePlan, storage::PlanRefusal>& result) {
		REQUIRE(std::holds_alternative<storage::SanitizePlan>(result));
		return std::get<storage::SanitizePlan>(result);
	}

	bool has(const std::vector<std::string>& lines, std::string_view part) {
		return std::ranges::any_of(lines, [&](const std::string& l) { return l.find(part) != std::string::npos; });
	}

	// plan, approve and run, the way the cli and the window do: inspect, plan, authorize with the confirmation, execute
	storage::Result<storage::SanitizeReport> run(storage::SimulatedDrive& drive, storage::SanitizeRequest request, const storage::CancellationToken* cancel = nullptr) {
		auto target = drive.inspect();
		REQUIRE(target.has_value());
		const auto plan = planned(storage::plan_sanitize(*target, request));
		auto approved = storage::authorize(plan, target->identity, plan.confirmation);
		REQUIRE(approved.has_value());
		return drive.execute(*approved, cancel);
	}

	bool all_zero(std::span<const std::byte> bytes) {
		return std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{ 0 }; });
	}
}

TEST_CASE("every simulated drive erases with every method it supports, and reading back says so") {
	for (std::string_view profile : storage::SimulatedDrive::profiles()) {
		auto first = storage::SimulatedDrive::make(profile);
		REQUIRE(first.has_value());
		for (Method m : first->inspect()->capabilities.methods) {
			if (storage::category_of(m, first->inspect()->identity.media) == Category::none)
				continue;
			CAPTURE(profile);
			CAPTURE(storage::to_string(m));
			auto drive = storage::SimulatedDrive::make(profile);
			const auto report = run(*drive, { .method = m, .zeros = false, .seed = 42 });
			REQUIRE(report.has_value());
			CHECK(report->status == SanitizeStatus::verified);
			CHECK(report->verification.done);
			CHECK(report->verification.mismatches == 0);
			CHECK(report->bytes_done == drive->contents().size());
			if (m != Method::overwrite && m != Method::ata_sanitize_crypto && m != Method::nvme_sanitize_crypto)
				CHECK(all_zero(drive->contents()));
		}
	}
	CHECK_FALSE(storage::SimulatedDrive::make("floppy").has_value());
}

TEST_CASE("the simulated drive's failures end in the status they are, never in success") {
	SUBCASE("firmware that says it worked when it didn't: reading back catches it") {
		for (Method m : { Method::overwrite, Method::nvme_sanitize_block, Method::nvme_sanitize_crypto }) {
			auto drive = storage::SimulatedDrive::make("nvme-ssd", { .lies = true });
			const auto report = run(*drive, { .method = m });
			REQUIRE(report.has_value());
			CHECK(report->status == SanitizeStatus::failed);
			CHECK(report->verification.mismatches > 0);
		}
	}
	SUBCASE("without reading back, the same lie can only be called completed") {
		auto drive = storage::SimulatedDrive::make("nvme-ssd", { .lies = true });
		const auto report = run(*drive, { .method = Method::nvme_sanitize_block, .verify = false });
		REQUIRE(report.has_value());
		CHECK(report->status == SanitizeStatus::completed);
		CHECK_FALSE(report->verification.done);
	}
	SUBCASE("a write error partway") {
		auto drive = storage::SimulatedDrive::make("hdd", { .fail_at = 0.5 });
		const auto report = run(*drive, {});
		REQUIRE(report.has_value());
		CHECK(report->status == SanitizeStatus::failed);
		CHECK(report->bytes_done == drive->contents().size() / 2);
		CHECK(all_zero(drive->contents().first(static_cast<std::size_t>(report->bytes_done))));
		CHECK_FALSE(all_zero(drive->contents().subspan(static_cast<std::size_t>(report->bytes_done))));
	}
	SUBCASE("power lost during an overwrite") {
		auto drive = storage::SimulatedDrive::make("usb-flash", { .power_loss_at = 0.25 });
		const auto report = run(*drive, {});
		REQUIRE(report.has_value());
		CHECK(report->status == SanitizeStatus::interrupted);
		CHECK_FALSE(drive->sanitize_pending()); // an overwrite doesn't go on by itself
	}
	SUBCASE("power lost during a firmware sanitize: it finishes at the next power on") {
		auto drive = storage::SimulatedDrive::make("nvme-ssd", { .power_loss_at = 0.3 });
		const auto report = run(*drive, { .method = Method::nvme_sanitize_block });
		REQUIRE(report.has_value());
		CHECK(report->status == SanitizeStatus::interrupted);
		CHECK(drive->sanitize_pending());
		CHECK_FALSE(all_zero(drive->contents()));
		CHECK(drive->power_on());
		CHECK(all_zero(drive->contents()));
	}
	SUBCASE("the drive's sanitize status says it failed") {
		auto drive = storage::SimulatedDrive::make("sata-ssd", { .fail_at = 0.6 });
		const auto report = run(*drive, { .method = Method::ata_sanitize_block });
		REQUIRE(report.has_value());
		CHECK(report->status == SanitizeStatus::failed);
	}
	SUBCASE("it can't be read back afterwards") {
		auto drive = storage::SimulatedDrive::make("hdd", { .unreadable_after = true });
		const auto report = run(*drive, {});
		REQUIRE(report.has_value());
		CHECK(report->status == SanitizeStatus::indeterminate);
	}
}

TEST_CASE("an overwrite stops when asked, a firmware sanitize can't") {
	storage::CancellationToken cancel;
	cancel.cancel();
	auto hdd = storage::SimulatedDrive::make("hdd");
	auto stopped = run(*hdd, {}, &cancel);
	REQUIRE(stopped.has_value());
	CHECK(stopped->status == SanitizeStatus::interrupted);
	CHECK(stopped->bytes_done == 0);

	auto nvme = storage::SimulatedDrive::make("nvme-ssd");
	auto went_on = run(*nvme, { .method = Method::nvme_sanitize_block }, &cancel);
	REQUIRE(went_on.has_value());
	CHECK(went_on->status == SanitizeStatus::verified);
	CHECK(has(went_on->events, "can't be stopped"));
}

TEST_CASE("a drive swapped between approval and running is refused, and nothing is done to it") {
	auto drive = storage::SimulatedDrive::make("hdd");
	auto target = drive->inspect();
	const auto plan = planned(storage::plan_sanitize(*target, {}));
	auto approved = storage::authorize(plan, target->identity, plan.confirmation);
	REQUIRE(approved.has_value());

	const std::vector<std::byte> before(drive->contents().begin(), drive->contents().end());
	storage::TargetIdentity other = target->identity;
	other.serial = "SIM-HDD-999999";
	drive->swap(other);
	const auto refused = drive->execute(*approved);
	REQUIRE_FALSE(refused.has_value());
	CHECK(refused.error().code == Errc::mismatch);
	CHECK(std::ranges::equal(drive->contents(), before));
}

TEST_CASE("a drive that got mounted, or frozen, isn't planned for what that rules out") {
	auto drive = storage::SimulatedDrive::make("sata-ssd");
	drive->capabilities().mounted = true;
	CHECK(std::holds_alternative<storage::PlanRefusal>(storage::plan_sanitize(*drive->inspect(), {})));
	drive->capabilities().mounted = false;
	drive->capabilities().frozen = true;
	CHECK(std::holds_alternative<storage::PlanRefusal>(storage::plan_sanitize(*drive->inspect(), { .method = Method::ata_sanitize_crypto })));
	CHECK(std::holds_alternative<storage::SanitizePlan>(storage::plan_sanitize(*drive->inspect(), { .method = Method::overwrite })));
}

TEST_CASE("the overwrite pattern depends only on the seed and the offset") {
	std::vector<std::byte> whole(1000), part(333);
	storage::sanitation::pattern(whole, 5000, { .zeros = false, .seed = 9 });
	storage::sanitation::pattern(part, 5003, { .zeros = false, .seed = 9 });
	CHECK(std::equal(part.begin(), part.end(), whole.begin() + 3));
	storage::sanitation::pattern(part, 5003, { .zeros = false, .seed = 10 });
	CHECK_FALSE(std::equal(part.begin(), part.end(), whole.begin() + 3));
	storage::sanitation::pattern(part, 0, { .zeros = true });
	CHECK(all_zero(part));
}
