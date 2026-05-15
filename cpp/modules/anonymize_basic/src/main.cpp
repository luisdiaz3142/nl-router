// nl-mod-anonymize-basic
//
// Walks every .dcm file under input_path, applies the configured tag
// transformations (strip / replace), and writes the result to output_path
// preserving the relative subdirectory layout.
//
// Config shape (processing_jobs.config):
//
//   {
//     "remove_tags":  ["PatientName", "PatientID", "ReferringPhysicianName"],
//     "replace_tags": {
//       "InstitutionName": "ANONYMIZED",
//       "PatientName":      "ANONYMOUS"
//     }
//   }
//
// Tag names use DCMTK's data dictionary (DICOM standard names). Unknown
// names produce a per-file warning and are skipped; the rest of the
// anonymization still happens.

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcdicent.h>
#include <dcmtk/dcmdata/dcdict.h>
#include <dcmtk/dcmdata/dcfilefo.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "nl_router/module/worker.hpp"

namespace {

namespace fs = std::filesystem;

// Resolve a DCMTK data-dictionary name (e.g. "PatientName") to its tag.
// Returns std::nullopt for unknown names.
std::optional<DcmTagKey> resolve_tag_name(const std::string& name) {
    const DcmDataDictionary& dict = dcmDataDict.rdlock();
    // DCMTK's name-based lookup is a single-arg overload (vs the
    // key-based 2-arg form used elsewhere in the codebase).
    const DcmDictEntry* entry = dict.findEntry(name.c_str());
    DcmTagKey key;
    bool found = false;
    if (entry != nullptr) {
        key   = entry->getKey();
        found = true;
    }
    dcmDataDict.rdunlock();
    if (!found) return std::nullopt;
    return key;
}

// Recursively collect every .dcm file under root, paired with its
// relative path so we can preserve directory structure in the output.
struct InstanceFile {
    fs::path source;
    fs::path rel;
};
std::vector<InstanceFile> walk(const fs::path& root) {
    std::vector<InstanceFile> out;
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator{};
         it.increment(ec))
    {
        if (!it->is_regular_file(ec) || ec) continue;
        if (it->path().extension() != ".dcm") continue;
        InstanceFile f;
        f.source = it->path();
        f.rel    = fs::relative(it->path(), root, ec);
        if (ec) f.rel = it->path().filename();
        out.push_back(std::move(f));
    }
    return out;
}

// Anonymize one file by mutating its dataset in place.
//
// `remove_keys` carries pre-resolved DcmTagKeys for every "remove_tags"
// entry that successfully resolved at config-load time. Same for
// `replace_kv`. The (config-validated, pre-resolved) lookup avoids a
// per-file dictionary search.
struct PreparedConfig {
    std::vector<DcmTagKey>                       remove_keys;
    std::vector<std::pair<DcmTagKey,std::string>> replace_kv;
    std::vector<std::string>                     unknown_remove;   // for log
    std::vector<std::string>                     unknown_replace;
};

bool apply_to_file(const fs::path& src, const fs::path& dst,
                    const PreparedConfig& prep, std::string& err)
{
    DcmFileFormat ff;
    OFCondition cond = ff.loadFile(src.c_str());
    if (cond.bad()) {
        err = std::string{"loadFile failed: "} + (cond.text() ? cond.text() : "?");
        return false;
    }
    DcmDataset* ds = ff.getDataset();
    if (!ds) {
        err = "getDataset returned null";
        return false;
    }

    // Remove tags. DCMTK's findAndDeleteElement is a no-op if the tag
    // wasn't present — idempotent, which is what we want for
    // anonymization.
    for (const auto& key : prep.remove_keys) {
        ds->findAndDeleteElement(key);
    }
    // Replace tags. putAndInsertString handles both "exists, replace"
    // and "doesn't exist, insert".
    for (const auto& [key, value] : prep.replace_kv) {
        cond = ds->putAndInsertString(key, value.c_str(), /*replaceOld=*/OFTrue);
        if (cond.bad()) {
            err = std::string{"putAndInsertString failed for tag: "} +
                  (cond.text() ? cond.text() : "?");
            return false;
        }
    }

    // Preserve transfer syntax so we don't accidentally re-encode pixel
    // data — anonymization is supposed to be lossless at the pixel
    // level.
    const E_TransferSyntax xfer = ds->getOriginalXfer();

    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (ec) { err = "create_directories: " + ec.message(); return false; }

    cond = ff.saveFile(dst.c_str(), xfer);
    if (cond.bad()) {
        err = std::string{"saveFile failed: "} + (cond.text() ? cond.text() : "?");
        return false;
    }
    return true;
}

nl_router::module::ProcessResult process(
    const std::string& input_path,
    const std::string& output_path,
    const std::string& config_json)
{
    using Result = nl_router::module::ProcessResult;

    if (input_path.empty() || output_path.empty()) {
        return Result::failure("input_path or output_path empty");
    }

    // ---- Parse + pre-resolve config ----
    PreparedConfig prep;
    try {
        const auto j = nlohmann::json::parse(config_json);

        if (j.contains("remove_tags") && j["remove_tags"].is_array()) {
            for (const auto& name_j : j["remove_tags"]) {
                if (!name_j.is_string()) continue;
                const auto name = name_j.get<std::string>();
                if (auto k = resolve_tag_name(name); k.has_value()) {
                    prep.remove_keys.push_back(*k);
                } else {
                    prep.unknown_remove.push_back(name);
                }
            }
        }
        if (j.contains("replace_tags") && j["replace_tags"].is_object()) {
            for (auto it = j["replace_tags"].begin();
                 it != j["replace_tags"].end(); ++it)
            {
                if (!it.value().is_string()) continue;
                const auto name  = it.key();
                const auto value = it.value().get<std::string>();
                if (auto k = resolve_tag_name(name); k.has_value()) {
                    prep.replace_kv.emplace_back(*k, value);
                } else {
                    prep.unknown_replace.push_back(name);
                }
            }
        }
    } catch (const nlohmann::json::exception& e) {
        return Result::failure(std::string{"config JSON invalid: "} + e.what());
    }

    if (!prep.unknown_remove.empty() || !prep.unknown_replace.empty()) {
        std::string warn = "unknown tag names ignored: ";
        for (const auto& n : prep.unknown_remove) warn += n + " ";
        for (const auto& n : prep.unknown_replace) warn += n + "= ";
        // Logged via stderr; the worker runtime captures it via its own
        // logger if the operator runs at debug level. We deliberately
        // don't fail — operators see the warning and fix.
        std::fprintf(stderr, "{\"lvl\":\"warn\",\"msg\":\"anonymize.unknown_tags\","
                              "\"detail\":\"%s\"}\n", warn.c_str());
    }

    // ---- Walk + transform ----
    const fs::path in_root{input_path};
    const fs::path out_root{output_path};
    const auto files = walk(in_root);
    if (files.empty()) {
        return Result::failure("no .dcm files under " + input_path);
    }

    int n_ok = 0, n_fail = 0;
    std::string first_err;
    for (const auto& f : files) {
        const fs::path dst = out_root / f.rel;
        std::string err;
        if (apply_to_file(f.source, dst, prep, err)) {
            ++n_ok;
        } else {
            ++n_fail;
            if (first_err.empty()) {
                first_err = f.rel.string() + ": " + err;
            }
            std::fprintf(stderr,
                "{\"lvl\":\"warn\",\"msg\":\"anonymize.file_failed\","
                "\"file\":\"%s\",\"error\":\"%s\"}\n",
                f.source.c_str(), err.c_str());
        }
    }

    if (n_fail > 0) {
        return Result::failure(
            "anonymize: " + std::to_string(n_fail) + " of " +
            std::to_string(files.size()) + " files failed (first: " + first_err + ")");
    }
    return Result::success();
}

}  // namespace

int main(int argc, char** argv) {
    return nl_router::module::run_worker(argc, argv, &process);
}
