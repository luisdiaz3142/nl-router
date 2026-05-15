// Handler factory. Lives in its own translation unit so each handler's
// implementation file doesn't have to know about the others.

#include "dicom_handler.hpp"
#include "dicomweb_stow_handler.hpp"
#include "file_handler.hpp"
#include "gcp_dicomweb_handler.hpp"
#include "handler.hpp"
#include "http_handler.hpp"
#include "object_storage_handler.hpp"

#include <memory>
#include <string>

namespace nlr {

std::unique_ptr<DispatchHandler> make_handler(const std::string& kind) {
    if (kind == "dicom")          return std::make_unique<DicomDispatchHandler>();
    if (kind == "http")           return std::make_unique<HttpDispatchHandler>();
    if (kind == "file")           return std::make_unique<FileDispatchHandler>();
    if (kind == "dicomweb_stow")  return std::make_unique<DicomwebStowDispatchHandler>();
    if (kind == "gcp_dicomweb")   return std::make_unique<GcpDicomwebDispatchHandler>();
    if (kind == "object_storage") return std::make_unique<ObjectStorageDispatchHandler>();

    // Operators can run custom workers for any other kind via the shared
    // route_assignments + SKIP LOCKED contract; nullptr here makes the
    // worker idle on that destination rather than crashing.
    return nullptr;
}

}  // namespace nlr
