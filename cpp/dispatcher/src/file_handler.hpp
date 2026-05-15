// nl-dispatch/file_handler.hpp
//
// Filesystem dispatcher. Writes a copy of every .dcm file in the study
// to a destination directory derived from a path template.
//
// Destination config (JSONB):
//   {
//     "path_template": "/archive/${PatientID}/${StudyInstanceUID}/",
//     "preserve_hierarchy": true,   // keep series/ subdirs from landing
//     "fsync": false                // best-effort durability
//   }

#pragma once

#include "handler.hpp"

namespace nlr {

class FileDispatchHandler final : public DispatchHandler {
public:
    DispatchResult dispatch(const Assignment& a, const Destination& d) override;
};

}  // namespace nlr
