#!/usr/bin/env python3
"""Generate a tiny synthetic CT DICOM file for receiver end-to-end testing.

Usage:
    python make_test_dicom.py /tmp/test.dcm
"""

from __future__ import annotations

import sys
from pathlib import Path

from pydicom.dataset import Dataset, FileDataset, FileMetaDataset
from pydicom.uid import CTImageStorage, ExplicitVRLittleEndian, generate_uid


def make_one(out: Path) -> None:
    file_meta = FileMetaDataset()
    file_meta.MediaStorageSOPClassUID = CTImageStorage
    file_meta.MediaStorageSOPInstanceUID = generate_uid()
    file_meta.TransferSyntaxUID = ExplicitVRLittleEndian

    ds = FileDataset(str(out), {}, file_meta=file_meta, preamble=b"\0" * 128)

    # ---- Identity ----
    ds.SOPClassUID = CTImageStorage
    ds.SOPInstanceUID = file_meta.MediaStorageSOPInstanceUID
    ds.StudyInstanceUID = generate_uid()
    ds.SeriesInstanceUID = generate_uid()
    ds.AccessionNumber = "ACC-TEST-001"
    ds.StudyID = "STUDY-001"

    # ---- Patient ----
    ds.PatientName = "Test^Patient^E2E"
    ds.PatientID = "PAT-E2E-12345"
    ds.PatientBirthDate = "19800101"
    ds.PatientSex = "O"

    # ---- Equipment ----
    ds.Modality = "CT"
    ds.StationName = "TEST_STATION"
    ds.InstitutionName = "Test Hospital"
    ds.Manufacturer = "Test Manufacturer"
    ds.ManufacturerModelName = "Model X"
    ds.DeviceSerialNumber = "SN-12345"

    # ---- Study / series ----
    ds.StudyDescription = "CHEST WITH CONTRAST"
    ds.SeriesDescription = "AX 2.0 MM"
    ds.ProtocolName = "PROTOCOL-A"
    ds.BodyPartExamined = "CHEST"
    ds.ReferringPhysicianName = "Refer^Physician"

    # ---- Timestamps ----
    ds.StudyDate = "20260515"
    ds.StudyTime = "100000"
    ds.SeriesDate = "20260515"
    ds.SeriesTime = "100100"
    ds.AcquisitionDate = "20260515"
    ds.AcquisitionTime = "100200"

    # ---- Numbering ----
    ds.SeriesNumber = "1"
    ds.InstanceNumber = "1"

    # ---- Acquisition / sequence ----
    ds.ImageType = ["ORIGINAL", "PRIMARY", "AXIAL"]
    ds.SliceThickness = "2.0"

    # ---- Charset ----
    ds.SpecificCharacterSet = "ISO_IR 100"

    # ---- Required image attrs so the file is well-formed ----
    ds.Rows = 4
    ds.Columns = 4
    ds.BitsAllocated = 16
    ds.BitsStored = 16
    ds.HighBit = 15
    ds.PixelRepresentation = 0
    ds.SamplesPerPixel = 1
    ds.PhotometricInterpretation = "MONOCHROME2"
    ds.PixelData = (b"\x00\x01" * (4 * 4))   # 16 px × 2 bytes

    ds.is_little_endian = True
    ds.is_implicit_VR = False

    ds.save_as(str(out), write_like_original=False)
    print(f"wrote {out}")
    print(f"  StudyInstanceUID  = {ds.StudyInstanceUID}")
    print(f"  SeriesInstanceUID = {ds.SeriesInstanceUID}")
    print(f"  SOPInstanceUID    = {ds.SOPInstanceUID}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: make_test_dicom.py <output.dcm>", file=sys.stderr)
        sys.exit(1)
    out = Path(sys.argv[1])
    out.parent.mkdir(parents=True, exist_ok=True)
    make_one(out)
