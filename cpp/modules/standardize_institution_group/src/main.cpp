// nl-mod-standardize-institution-group
//
// Sets (0008,1040) InstitutionalDepartmentName on every .dcm file
// under input_path, then writes the result to output_path preserving
// the relative subdirectory layout. Creates the tag if it doesn't
// already exist; replaces it if it does.
//
// Why a separate module instead of using anonymize_basic with
// `{"replace_tags":{"InstitutionalDepartmentName":"NVRA"}}`? Two
// reasons: (1) the intent is clearer at the rule layer — operators
// reading the chain see "standardize_institution_group" and know what
// it does without reading the JSON config; (2) it's a clean template
// for adding more targeted single-purpose modules (set_protocol_name,
// set_referring_physician, etc.) that don't need the
// strip-arbitrary-tags surface of anonymize_basic.
//
// Config shape (processing_jobs.config / rule_processing_chain.config_override):
//
//   {
//     "value": "NVRA"      // optional; defaults to "NVRA" if omitted
//   }
//
// The default lets operators bind the module to a rule with an empty
// config (`{}`) and still get the canonical value. Use the override
// when you want a different department name per rule (e.g. one rule
// sets "NVRA-CT", another sets "NVRA-MR").

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "nl_router/module/worker.hpp"

namespace {

namespace fs = std::filesystem;

// Default value when the rule's config doesn't override it. Hardcoded
// to "NVRA" per the deployment we're shipping this for; can still be
// overridden via the config object.
constexpr const char* kDefaultDepartment = "NVRA";

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

// Apply the tag set to one file. putAndInsertString handles both
// "tag exists, replace" and "tag missing, insert" in one call — that's
// exactly the create-or-replace semantic the spec calls for.
bool apply_to_file(const fs::path& src, const fs::path& dst,
                    const std::string& department_name, std::string& err)
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

    // The actual mutation. DCMTK_TAG_InstitutionalDepartmentName == (0008,1040).
    // putAndInsertString with replaceOld=OFTrue replaces an existing value or
    // inserts a new element if the tag wasn't present.
    cond = ds->putAndInsertString(
        DCM_InstitutionalDepartmentName,
        department_name.c_str(),
        /*replaceOld=*/OFTrue);
    if (cond.bad()) {
        err = std::string{"putAndInsertString failed: "} +
              (cond.text() ? cond.text() : "?");
        return false;
    }

    // Preserve the original transfer syntax — don't re-encode pixel data.
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

// The function the worker runtime calls once per claimed
// processing_jobs row. input_path / output_path are pre-resolved to
// absolute paths by the router; config_json is the merged
// (module-default ← rule-override) config object.
nl_router::module::ProcessResult process(
    const std::string& input_path,
    const std::string& output_path,
    const std::string& config_json)
{
    using Result = nl_router::module::ProcessResult;

    if (input_path.empty() || output_path.empty()) {
        return Result::failure("input_path or output_path empty");
    }

    // ---- Parse config ---------------------------------------------------
    std::string department = kDefaultDepartment;
    try {
        const auto j = nlohmann::json::parse(config_json);
        if (j.contains("value") && j["value"].is_string()) {
            department = j["value"].get<std::string>();
        }
        // Empty value isn't useful — fall back to the default rather than
        // silently writing a zero-length string. Operators who really want
        // an empty value should use the strip-tag side of anonymize_basic.
        if (department.empty()) {
            department = kDefaultDepartment;
        }
    } catch (const nlohmann::json::exception& e) {
        // Empty / null config is fine — we just use the default. Log the
        // parse error but proceed; the rule may have left config_override
        // null deliberately.
        std::fprintf(stderr,
            "{\"lvl\":\"warn\",\"msg\":\"standardize_institution_group.config_parse_warn\","
            "\"detail\":\"%s\",\"using_default\":\"%s\"}\n",
            e.what(), department.c_str());
    }

    // ---- Walk + transform ----------------------------------------------
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
        if (apply_to_file(f.source, dst, department, err)) {
            ++n_ok;
        } else {
            ++n_fail;
            if (first_err.empty()) {
                first_err = f.rel.string() + ": " + err;
            }
            std::fprintf(stderr,
                "{\"lvl\":\"warn\",\"msg\":\"standardize_institution_group.file_failed\","
                "\"file\":\"%s\",\"error\":\"%s\"}\n",
                f.source.c_str(), err.c_str());
        }
    }

    // Structured per-job summary log line — visible in
    // `journalctl -u nl-router-module@standardize_institution_group`.
    std::fprintf(stderr,
        "{\"lvl\":\"info\",\"msg\":\"standardize_institution_group.job_summary\","
        "\"value\":\"%s\",\"ok\":%d,\"fail\":%d,\"total\":%zu}\n",
        department.c_str(), n_ok, n_fail, files.size());

    if (n_fail > 0) {
        return Result::failure(
            "standardize_institution_group: " + std::to_string(n_fail) + " of " +
            std::to_string(files.size()) + " files failed (first: " + first_err + ")");
    }
    return Result::success();
}

}  // namespace

int main(int argc, char** argv) {
    return nl_router::module::run_worker(argc, argv, &process);
}
