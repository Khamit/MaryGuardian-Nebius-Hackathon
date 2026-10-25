#include "XrayDoc.hpp"
#include "Xray.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace mary { namespace xray {
using json = nlohmann::json;

namespace {
std::string str(const json& d, const char* k) { return (d.is_object() && d.contains(k) && d[k].is_string()) ? d[k].get<std::string>() : std::string(); }
long long num(const json& d, const char* k, long long def = 0) { return (d.is_object() && d.contains(k) && d[k].is_number_integer()) ? d[k].get<long long>() : def; }
} // namespace

json viewForDocument(const json& doc, const std::string& inboxDir, int page, Consistency* cons,
                     const std::vector<std::string>& protectedTerms, MaskLevel level) {
    namespace fs = std::filesystem;
    const std::string name = fs::path(str(doc, "filename")).filename().string();   // только имя файла: никаких путей от клиента
    if (name.empty()) return json{{"error", "document has no file"}};
    std::string ext = fs::path(name).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext != ".pdf") return json{{"error", "X-ray is available for PDF files"}};
    const fs::path path = fs::path(inboxDir) / name;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::is_symlink(path, ec)) return json{{"error", "the file is not in the inbox any more"}};

    const long long id = num(doc, "id");
    Options o;
    o.page = page < 0 ? 0 : page;
    o.level = level;
    o.terms = PiiMasker::parseTerms(protectedTerms);
    if (!str(doc, "company").empty()) o.needles.push_back({"Issuer", str(doc, "company"), "field", "Company the pipeline recognised"});
    if (!str(doc, "doc_number").empty()) o.needles.push_back({"Number", str(doc, "doc_number"), "field", "Document number"});
    for (const auto& f : dateForms(str(doc, "doc_date"))) o.needles.push_back({"Date", f, "field", "Document date"});
    for (const auto& f : dateForms(str(doc, "expires"))) o.needles.push_back({"Due / valid until", f, "field", "Deadline used for reminders"});
    if (num(doc, "amount_minor", -1) >= 0)
        for (const auto& f : amountForms(num(doc, "amount_minor"))) o.needles.push_back({"Amount", f, "field", "Total amount"});
    if (cons) {
        o.ibanFp = [cons](const std::string& norm) { return cons->fingerprint(norm); };
        o.riskFps = cons->riskFingerprints(id);
    }

    json r = buildPageView(path.string(), o);
    if (!r.value("ok", false)) return r;
    r["doc"] = {{"id", id}, {"filename", name}, {"company", str(doc, "company")}, {"doc_number", str(doc, "doc_number")},
                {"doc_type", str(doc, "doc_type")}, {"summary", str(doc, "summary")}};
    r["flags"] = cons ? cons->flags(id) : json::array();
    return r;
}

}} // namespace mary::xray
