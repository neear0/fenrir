// a sanitize report as JSON, unsigned: written by hand so nothing outside the standard library is needed

#include <format>
#include <iterator>

#include "storage/sanitize.h"

namespace {
	using namespace storage;

	std::string json_string(std::string_view text) {
		std::string out = "\"";
		for (const char c : text) {
			switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20)
					std::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
				else
					out += c; // utf-8 passes through
			}
		}
		return out + "\"";
	}

	std::string path_text(const std::filesystem::path& path) {
		const std::u8string text = path.u8string();
		return { reinterpret_cast<const char*>(text.data()), text.size() };
	}

	std::string list(const std::vector<std::string>& items, std::string_view indent) {
		if (items.empty())
			return "[]";
		std::string out = "[\n";
		for (std::size_t i = 0; i < items.size(); i++)
			out += std::format("{}  {}{}\n", indent, json_string(items[i]), i + 1 < items.size() ? "," : "");
		return out + std::string(indent) + "]";
	}
}

std::string storage::to_json(const SanitizeReport& r) {
	const SanitizePlan& p = r.plan;
	const TargetIdentity& id = p.target.identity;
	std::vector<std::string> methods;
	for (Method m : p.target.capabilities.methods)
		methods.emplace_back(to_string(m));

	std::string out = "{\n";
	out += "  \"tool\": \"fenrir\",\n";
	out += "  \"kind\": \"sanitize report, unsigned\",\n";
	out += std::format("  \"status\": {},\n", json_string(to_string(r.status)));
	out += std::format("  \"started\": {},\n  \"finished\": {},\n", json_string(r.started), json_string(r.finished));
	out += "  \"target\": {\n";
	out += std::format("    \"media\": {},\n", json_string(to_string(id.media)));
	out += std::format("    \"model\": {},\n    \"serial\": {},\n", json_string(id.model), json_string(id.serial));
	out += std::format("    \"path\": {},\n", json_string(path_text(id.path)));
	out += std::format("    \"size\": {},\n", id.size);
	out += std::format("    \"sector_size\": {},\n", p.target.capabilities.sector_size);
	out += std::format("    \"supports\": {}\n", list(methods, "    "));
	out += "  },\n";
	out += std::format("  \"method\": {},\n", json_string(to_string(p.request.method)));
	if (p.request.method == Method::overwrite)
		out += std::format("  \"pattern\": {},\n", json_string(p.request.zeros ? "zeros" : std::format("pseudorandom, seed {}", p.request.seed)));
	out += std::format("  \"category_at_most\": {},\n", json_string(to_string(p.category)));
	out += std::format("  \"scope\": {},\n", json_string(p.scope));
	out += std::format("  \"bytes_done\": {},\n", r.bytes_done);
	out += std::format("  \"steps_planned\": {},\n", list(p.steps, "  "));
	out += std::format("  \"events\": {},\n", list(r.events, "  "));
	out += std::format("  \"errors\": {},\n", list(r.errors, "  "));
	out += "  \"verification\": {\n";
	out += std::format("    \"done\": {},\n", r.verification.done ? "true" : "false");
	out += std::format("    \"bytes_checked\": {},\n", r.verification.bytes_checked);
	out += std::format("    \"mismatched_sectors\": {},\n", r.verification.mismatches);
	if (r.verification.mismatches != 0)
		out += std::format("    \"first_mismatch_at\": {},\n", r.verification.first_mismatch);
	out += std::format("    \"note\": {}\n", json_string(r.verification.note));
	out += "  },\n";
	out += std::format("  \"validation\": {},\n", json_string(r.validation));
	out += std::format("  \"caveats\": {}\n", list(p.caveats, "  "));
	out += "}\n";
	return out;
}
