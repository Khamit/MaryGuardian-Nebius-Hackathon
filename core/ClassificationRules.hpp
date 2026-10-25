#pragma once

#include <set>
#include <string>

namespace mary::classification {

// Result from deterministic, local rules. No model/network calls are made here.
struct RuleResult {
    // type is populated only when candidate_type exists in the caller's allowedTypes.
    std::string type;
    std::string candidate_type;
    bool type_supported = false;
    bool type_confident = false;
    int type_score = 0;
    int type_margin = 0;

    std::string company;
    std::string date;       // ISO YYYY-MM-DD
    std::string number;
    std::string currency;   // ISO 4217 when explicitly identified
    long long amount_minor = -1; // cents/kopecks/tiyn; -1 means not found
};

// Classify text using weighted phrase rules and extract common metadata.
// allowedTypes should be populated from extract::docTypes(); this avoids silently
// writing a type that the database/UI does not know about.
RuleResult classify(const std::string& text,
                    const std::string& filename,
                    const std::set<std::string>& allowedTypes);

} // namespace mary::classification
