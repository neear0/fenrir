#include <doctest/doctest.h>

#include <string>
#include <variant>
#include <vector>

#include "storage/drive_erase.h"
#include "storage/sanitize.h"

// the drive eraser's rules, on drives described in code. nothing here ever hands a real drive to the writer: the only
// drive numbers execute() sees are 62 and 63, with a model and serial no real drive has, so even a machine with that
// many drives would fail the identity check before anything is opened for writing
using storage::DiskInfo;
using storage::Errc;
using storage::Media;

namespace {
	DiskInfo stick() {
		DiskInfo d;
		d.number = 7;
		d.path = "\\\\.\\PhysicalDrive7";
		d.model = "USB DISK 2.0";
		d.serial = "940B05055030";
		d.bus = "USB";
		d.removable = true;
		d.rotational = false;
		d.size = 31457280000;
		d.sector_size = 512;
		d.volumes = { "E:\\" };
		return d;
	}

	std::string joined(const std::vector<std::string>& lines) {
		std::string out;
		for (const std::string& l : lines)
			out += l + "\n";
		return out;
	}
}

TEST_CASE("a USB stick that holds nothing fenrir needs has no blockers") {
	CHECK(storage::drive_blockers(stick(), { 0, 1 }).empty());
	CHECK(storage::drive_media(stick()) == Media::usb_flash);
	DiskInfo spinning = stick();
	spinning.rotational = true; // a hard disk in a USB enclosure
	CHECK(storage::drive_media(spinning) == Media::hdd);
	DiskInfo silent = stick();
	silent.rotational.reset();
	CHECK(storage::drive_media(silent) == Media::usb_flash);
}

TEST_CASE("everything that makes a drive unsafe to erase is a blocker, each with its reason") {
	DiskInfo d = stick();
	d.bus = "SATA";
	d.system = true;
	d.error = "its size couldn't be read";
	d.serial.clear();
	d.size = 0;
	d.sector_size = 520;
	d.spanned_volume = true;
	const std::string why = joined(storage::drive_blockers(d, { 7 }));
	CHECK(why.find("attached over SATA, not USB") != std::string::npos);
	CHECK(why.find("Windows is on it") != std::string::npos);
	CHECK(why.find("fenrir itself, its temp folder") != std::string::npos);
	CHECK(why.find("couldn't all be read") != std::string::npos);
	CHECK(why.find("no serial number") != std::string::npos);
	CHECK(why.find("no size") != std::string::npos);
	CHECK(why.find("sector size isn't usable") != std::string::npos);
	CHECK(why.find("on another drive too") != std::string::npos);

	DiskInfo nvme = stick();
	nvme.bus = "NVMe";
	CHECK(storage::drive_blockers(nvme, {}).size() == 1);
	DiskInfo unknown = stick();
	unknown.bus.clear();
	CHECK(joined(storage::drive_blockers(unknown, {})).find("unknown bus") != std::string::npos);
}

TEST_CASE("a drive's plan: limited clear at most, its volumes offline first, the serial's end to type") {
	storage::Target t;
	t.identity = storage::drive_identity(stick());
	t.capabilities.methods = { storage::Method::overwrite };
	t.capabilities.volumes = stick().volumes;
	const auto result = storage::plan_sanitize(t, {});
	REQUIRE(std::holds_alternative<storage::SanitizePlan>(result));
	const auto& plan = std::get<storage::SanitizePlan>(result);
	CHECK(plan.category == storage::Category::limited_clear);
	CHECK(plan.confirmation == "ERASE 7 055030"); // the drive number and the serial's end
	CHECK(joined(plan.steps).find("take its volumes offline (E:\\)") != std::string::npos);
	CHECK(joined(plan.caveats).find("flash controller") != std::string::npos);

	t.capabilities.blockers = { "Windows is on it" };
	const auto refused = storage::plan_sanitize(t, {});
	REQUIRE(std::holds_alternative<storage::PlanRefusal>(refused));
	CHECK(joined(std::get<storage::PlanRefusal>(refused).reasons).find("Windows is on it") != std::string::npos);
}

TEST_CASE("the drive eraser does nothing to a drive that isn't there, or isn't the planned one") {
	storage::DriveOverwriteBackend missing(63);
	CHECK_FALSE(missing.inspect().has_value());

	// an approved plan for drive 63, handed to the backend for 63 (it isn't there) and for 62 (another number)
	DiskInfo ghost = stick();
	ghost.number = 63;
	ghost.model = "NOT A REAL DRIVE"; // nothing a real drive could be
	ghost.serial = "NOT-A-REAL-SERIAL";
	ghost.path = "\\\\.\\PhysicalDrive63";
	storage::Target t;
	t.identity = storage::drive_identity(ghost);
	t.capabilities.methods = { storage::Method::overwrite };
	const auto plan = std::get<storage::SanitizePlan>(storage::plan_sanitize(t, {}));
	auto approved = storage::authorize(plan, t.identity, plan.confirmation);
	REQUIRE(approved.has_value());

	const auto not_there = missing.execute(*approved);
	REQUIRE_FALSE(not_there.has_value());
	if (storage::drive_erase_built())
		CHECK(not_there.error().code == Errc::not_found);
	else
		CHECK(not_there.error().code == Errc::unsupported);

	storage::DriveOverwriteBackend other(62);
	const auto wrong = other.execute(*approved);
	REQUIRE_FALSE(wrong.has_value());
	CHECK((wrong.error().code == Errc::mismatch || wrong.error().code == Errc::unsupported));
}
