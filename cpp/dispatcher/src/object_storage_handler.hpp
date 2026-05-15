// nl-dispatch/object_storage_handler.hpp
//
// S3-compatible object-storage dispatcher (AWS, MinIO, Wasabi, Backblaze
// B2's S3 interface, etc.). Signs requests using AWS Signature V4 via
// libcurl's CURLOPT_AWS_SIGV4 (libcurl >= 7.75.0).
//
// Destination config:
//   {
//     "endpoint":      "https://s3.amazonaws.com",    // or http://minio:9000
//     "bucket":        "dicom-archive",
//     "key_template":  "${Modality}/${StudyDate}/${StudyInstanceUID}/",
//     "region":        "us-east-1",
//     "storage_class": "STANDARD_IA",                 // optional
//     "force_path_style": false,                       // S3 path-style for MinIO
//     "timeout_s":     120
//   }
//
// Per-file object key:
//   <bucket>/<key_template_expanded>/<file's path relative to study_file_root>
//
// Tags-bearing values in the template are taken from the work_queue
// row's tags JSONB (same expand_template helper used by file_handler
// and http_handler). The leaf filename ensures per-instance keys differ
// — the receiver names files `<sop_instance_uid>.dcm`, so two instances
// under the same study get distinct keys without requiring per-instance
// template substitution.
//
// Credential: `aws_keys` ({access_key_id, secret_access_key,
// session_token?}). Other auth modes (IAM role from metadata service,
// SigV4 over GCS / Wasabi-specific headers) are out of scope for v1.

#pragma once

#include "handler.hpp"

namespace nlr {

class ObjectStorageDispatchHandler final : public DispatchHandler {
public:
    ObjectStorageDispatchHandler();
    ~ObjectStorageDispatchHandler() override;

    DispatchResult dispatch(const Assignment& a, const Destination& d) override;
};

}  // namespace nlr
