// nl-dispatch/dicom_handler.hpp
//
// DispatchHandler for kind='dicom' destinations. Sends all .dcm files
// under `Assignment::study_file_root` to the peer SCP in a single
// association, as C-STORE requests.
//
// Implementation: DCMTK's DcmSCU. We negotiate presentation contexts at
// association open, then loop over files sending each via sendSTORERequest.
// One success-or-failure response per file is captured into
// DispatchResult::response_detail_json (a JSON array of
// {sop_uid, status_code} objects) so the operator can drill into
// per-instance results from the UI later.

#pragma once

#include "handler.hpp"

namespace nlr {

class DicomDispatchHandler final : public DispatchHandler {
public:
    DispatchResult dispatch(const Assignment& a, const Destination& d) override;
};

}  // namespace nlr
