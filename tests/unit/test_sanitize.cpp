#include <doctest/doctest.h>

#include <string>
#include <variant>

#include "storage/sanitize.h"

using storage::Category;
using storage::Errc;
using storage::Media;
using storage::Method;
using storage::SanitizeStatus;

namespace {
	storage::Target drive(Media media, std::vector<Method> methods) {
		storage::Target t;
		t.identity = { media, "TEST DRIVE 1000", "SN-0001-ABC123", "sim:test", 1u << 20, 0 };
		t.capabilities.methods = std::move(methods);
		return t;
	}

	storage::SanitizePlan planned(const std::variant<storage::SanitizePlan, storage::PlanRefusal>& result) {
		REQUIRE(std::holds_alternative<storage::SanitizePlan>(result));
		return std::get<storage::SanitizePlan>(result);
	}

	std::string reasons(const std::variant<storage::SanitizePlan, storage::PlanRefusal>& result) {
		REQUIRE(std::holds_alternative<storage::PlanRefusal>(result));
		std::string out;
		for (const std::string& r : std::get<storage::PlanRefusal>(result).reasons)
			out += r + "\n";
		return out;
	}

	bool has(const std::vector<std::string>& lines, std::string_view part) {
		for (const std::string& l : lines)
			if (l.find(part) != std::string::npos)
				return true;
		return false;
	}
}

TEST_CASE("the matrix never calls more than a method can do on a medium") {
	CHECK(storage::category_of(Method::overwrite, Media::hdd) == Category::clear);
	CHECK(storage::category_of(Method::overwrite, Media::nvme_ssd) == Category::clear); // never purge on flash
	CHECK(storage::category_of(Method::overwrite, Media::usb_flash) == Category::limited_clear);
	CHECK(storage::category_of(Method::overwrite, Media::unknown) == Category::limited_clear);
	CHECK(storage::category_of(Method::overwrite, Media::image_file) == Category::none);
	CHECK(storage::category_of(Method::ata_secure_erase, Media::hdd) == Category::purge);
	CHECK(storage::category_of(Method::ata_secure_erase, Media::sata_ssd) == Category::clear);
	CHECK(storage::category_of(Method::nvme_sanitize_crypto, Media::nvme_ssd) == Category::purge);
	CHECK(storage::category_of(Method::nvme_sanitize_block, Media::sata_ssd) == Category::none);
	CHECK(storage::category_of(Method::ata_sanitize_block, Media::hdd) == Category::none);
	for (Method m : { Method::overwrite, Method::ata_secure_erase, Method::ata_sanitize_block, Method::ata_sanitize_crypto, Method::nvme_sanitize_block,
			 Method::nvme_sanitize_crypto, Method::nvme_sanitize_overwrite }) {
		CHECK(storage::method_from(storage::to_string(m)) == m);
		CHECK(storage::category_of(m, Media::image_file) == Category::none);
		CHECK(storage::category_of(m, Media::usb_flash) != Category::purge);
	}
	CHECK_FALSE(storage::method_from("shred").has_value());
}

TEST_CASE("a plan says what it would do, what it may be called, and what has to be typed") {
	const auto plan = planned(storage::plan_sanitize(drive(Media::nvme_ssd, { Method::overwrite, Method::nvme_sanitize_crypto }), { .method = Method::nvme_sanitize_crypto }));
	CHECK(plan.category == Category::purge);
	CHECK(plan.confirmation == "ERASE ABC123"); // the serial's end
	CHECK_FALSE(plan.cancellable);
	CHECK(has(plan.caveats, "whole drive, every namespace"));
	CHECK(has(plan.caveats, "can't be stopped"));
	CHECK(has(plan.caveats, "no conformance"));
	CHECK(has(plan.steps, "check again that the target is the one"));

	const auto overwrite = planned(storage::plan_sanitize(drive(Media::nvme_ssd, { Method::overwrite }), {}));
	CHECK(overwrite.category == Category::clear);
	CHECK(overwrite.cancellable);
	CHECK(has(overwrite.caveats, "never purge"));

	storage::Target image = drive(Media::image_file, { Method::overwrite });
	image.identity.path = "C:/tests/disposable.img";
	image.identity.serial.clear();
	const auto test = planned(storage::plan_sanitize(image, { .verify = false }));
	CHECK(test.category == Category::none);
	CHECK(test.confirmation == "ERASE disposable.img");
	CHECK(has(test.caveats, "not for sanitizing anything"));
	CHECK(has(test.steps, "completed, not verified"));
}

TEST_CASE("what a target can't do, or shouldn't be doing, is refused with every reason and nothing else tried") {
	SUBCASE("an unsupported method: no fallback to another one") {
		const auto r = storage::plan_sanitize(drive(Media::nvme_ssd, { Method::overwrite }), { .method = Method::nvme_sanitize_block });
		CHECK(reasons(r).find("nothing else is tried instead") != std::string::npos);
		CHECK(std::get<storage::PlanRefusal>(r).status == SanitizeStatus::unsupported);
	}
	SUBCASE("a method that isn't sanitization on that media") {
		CHECK(reasons(storage::plan_sanitize(drive(Media::hdd, { Method::ata_sanitize_block }), { .method = Method::ata_sanitize_block })).find("isn't a sanitization method") != std::string::npos);
	}
	SUBCASE("the system drive, mounted, frozen, locked, and no size, all at once") {
		storage::Target t = drive(Media::sata_ssd, { Method::ata_secure_erase });
		t.capabilities.system = t.capabilities.mounted = t.capabilities.frozen = t.capabilities.locked = true;
		t.identity.size = 0;
		const std::string why = reasons(storage::plan_sanitize(t, { .method = Method::ata_secure_erase }));
		CHECK(why.find("offline workflow") != std::string::npos);
		CHECK(why.find("mounted") != std::string::npos);
		CHECK(why.find("frozen") != std::string::npos);
		CHECK(why.find("locked") != std::string::npos);
		CHECK(why.find("no size") != std::string::npos);
	}
	SUBCASE("frozen only matters to ATA commands") {
		storage::Target t = drive(Media::sata_ssd, { Method::overwrite });
		t.capabilities.frozen = true;
		CHECK(std::holds_alternative<storage::SanitizePlan>(storage::plan_sanitize(t, {})));
	}
	SUBCASE("a test image is only ever overwritten") {
		storage::Target t = drive(Media::image_file, { Method::overwrite, Method::nvme_sanitize_block });
		CHECK(reasons(storage::plan_sanitize(t, { .method = Method::nvme_sanitize_block })).find("can only be overwritten") != std::string::npos);
	}
}

TEST_CASE("approval needs the confirmation exactly, and the same target") {
	const storage::Target t = drive(Media::hdd, { Method::overwrite });
	const auto plan = planned(storage::plan_sanitize(t, {}));
	CHECK(storage::authorize(plan, t.identity, plan.confirmation).has_value());
	CHECK(storage::authorize(plan, t.identity, "").error().code == Errc::invalid_argument);
	CHECK(storage::authorize(plan, t.identity, "erase abc123").error().code == Errc::invalid_argument);
	CHECK(storage::authorize(plan, t.identity, plan.confirmation + " ").error().code == Errc::invalid_argument);

	storage::TargetIdentity swapped = t.identity;
	swapped.serial = "SN-0002-XYZ789";
	CHECK(storage::authorize(plan, swapped, plan.confirmation).error().code == Errc::mismatch);
	swapped = t.identity;
	swapped.size += 512;
	CHECK(storage::authorize(plan, swapped, plan.confirmation).error().code == Errc::mismatch);
}

TEST_CASE("a report keeps verification and validation apart and escapes what it quotes") {
	storage::SanitizeReport report;
	report.plan = planned(storage::plan_sanitize(drive(Media::hdd, { Method::overwrite }), {}));
	report.plan.target.identity.model = "A \"quoted\" model\\x\n";
	report.status = SanitizeStatus::verified;
	report.events = { "started", "done" };
	report.verification = { true, 1u << 20, 0, 0, "every sector read back as zeros" };
	const std::string json = storage::to_json(report);
	CHECK(json.find("\"status\": \"verified\"") != std::string::npos);
	CHECK(json.find("\"model\": \"A \\\"quoted\\\" model\\\\x\\n\"") != std::string::npos);
	CHECK(json.find("\"validation\": \"not assessed") != std::string::npos);
	CHECK(json.find("\"category_at_most\": \"clear\"") != std::string::npos);
	CHECK(json.find("\"kind\": \"sanitize report, unsigned\"") != std::string::npos);
}
