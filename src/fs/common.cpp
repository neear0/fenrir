#include <algorithm>
#include <format>
#include <set>

#include "common.h"

namespace {
	constexpr std::size_t max_diagnostics = 200;
	constexpr std::size_t max_claim_checks = 64;
}

void storage::fsutil::put_utf8(std::string& out, char32_t c) {
	if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF)
		c = 0xFFFD;
	if (c < 0x80) {
		out += static_cast<char>(c);
	} else if (c < 0x800) {
		out += static_cast<char>(0xC0 | (c >> 6));
		out += static_cast<char>(0x80 | (c & 0x3F));
	} else if (c < 0x10000) {
		out += static_cast<char>(0xE0 | (c >> 12));
		out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (c & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (c >> 18));
		out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (c & 0x3F));
	}
}

std::string storage::fsutil::utf8(std::u16string_view text) {
	std::string out;
	for (std::size_t i = 0; i < text.size(); i++) {
		const char32_t unit = text[i];
		if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
			put_utf8(out, 0x10000 + ((unit - 0xD800) << 10) + (text[i + 1] - 0xDC00));
			i++;
		} else {
			put_utf8(out, unit);
		}
	}
	return out;
}

void storage::fsutil::mark_shared_claims(std::vector<Claim>& claims, std::vector<RecoveryCandidate>& candidates) {
	std::ranges::sort(claims, {}, &Claim::first);
	std::set<std::pair<std::size_t, std::size_t>> told; // a pair of files sharing several pieces is said once
	for (std::size_t i = 0; i < claims.size(); i++) {
		for (std::size_t j = i + 1; j < claims.size() && j - i <= max_claim_checks && claims[j].first < claims[i].end; j++) {
			const Claim& a = claims[i];
			const Claim& b = claims[j];
			if (a.candidate == b.candidate || !told.insert(std::minmax(a.candidate, b.candidate)).second)
				continue;
			const std::uint64_t from = b.first, to = std::min(a.end, b.end) - 1;
			for (auto [mine, other] : { std::pair{ &a, &b }, std::pair{ &b, &a } }) {
				RecoveryCandidate& cand = candidates[mine->candidate];
				if (cand.integrity == Integrity::intact || cand.integrity == Integrity::partial)
					cand.integrity = Integrity::uncertain;
				cand.reasons.push_back(from == to
					? std::format("cluster {} is also claimed by deleted entry #{}: at most one of them still has its data there", from, candidates[other->candidate].id)
					: std::format("clusters {} to {} are also claimed by deleted entry #{}: at most one of them still has its data there", from, to, candidates[other->candidate].id));
			}
		}
	}
}

void storage::fsutil::note(std::vector<Diagnostic>& diagnostics, Diagnostic::Severity severity, std::string message) {
	if (diagnostics.size() < max_diagnostics)
		diagnostics.push_back({ severity, std::move(message) });
	else if (diagnostics.size() == max_diagnostics)
		diagnostics.push_back({ Diagnostic::Severity::warning, "more problems were found than are listed here" });
}
