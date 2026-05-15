# Processing module binaries. Each subdirectory builds one
# nl-mod-<kind> executable that links against the shared
# nl_router::module worker runtime.

add_subdirectory(common)
add_subdirectory(anonymize_basic)

# Future kinds:
#   add_subdirectory(anonymize_dicom_psi)
#   add_subdirectory(tag_modifier)
#   add_subdirectory(transcode)
#   add_subdirectory(uid_remap)
#   add_subdirectory(http_callout)
#   add_subdirectory(custom_script)
