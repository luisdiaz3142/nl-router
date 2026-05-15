// Handler factory. Lives in its own translation unit so each handler's
// implementation file doesn't have to know about the others.

#include "dicom_handler.hpp"
#include "dicomweb_stow_handler.hpp"
#include "file_handler.hpp"
#include "handler.hpp"
#include "http_handler.hpp"

#include <memory>
#include <string>

namespace nlr {

std::unique_ptr<DispatchHandler> make_handler(const std::string& kind) {
    if (kind == "dicom")          return std::make_unique<DicomDispatchHandler>();
    if (kind == "http")           return std::make_unique<HttpDispatchHandler>();
    if (kind == "file")           return std::make_unique<FileDispatchHandler>();
    if (kind == "dicomweb_stow")  return std::make_unique<DicomwebStowDispatchHandler>();

    // gcp_dicomweb (OAuth2 service-account flow) and object_storage
    // (AWS SigV4) are deferred to follow-up slices. Operators can run
    // custom workers for any other kind via the shared route_assignments
    // + SKIP LOCKED contract; nullptr here makes the worker idle on that
    // destination rather than crashing.
    return nullptr;
}

}  // namespace nlr
