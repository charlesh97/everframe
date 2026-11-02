# EverFrame board manufacturing release v1.0.0

Exported 2026-11-01 from Git base `2a4e3c9` plus the reviewed schematic/PCB corrections included in this package. The source ZIP in `source/` is the authoritative design snapshot for this release.

## Firmware compatibility

This revision remains compatible with the current firmware: display chip select is still U1 GPIO43 (`ESP32_CS`). No firmware sources, binaries, or build products are included.

## Package contents

- `fabrication/gerbers/`: four copper layers, solder mask, paste, silkscreen, board outline, and Gerber job file.
- `fabrication/drill/`: separate plated and non-plated Excellon files, maps, and drill report.
- `assembly/`: consolidated/front/back pick-and-place CSVs, BOM CSV, and top/bottom assembly drawings.
- `documentation/`: schematic PDF, STEP model, netlist, DRC/ERC reports, review findings, IC stock snapshot, and paste-aperture audit.
- `source/`: self-contained KiCad design source archive.

Placement files use millimetres and the KiCad absolute board coordinate system. Bottom-side X coordinates are not mirrored. Confirm the assembler's coordinate and rotation convention during intake.

## Release checks

- Paste: 242 SMD copper pads and 242 paste apertures; no missing, wrong-side, undersized, or oversized apertures. J1 shield tabs S1/S2 include corrected mask and paste openings. D5 intentionally uses separate reduced paste-only apertures.
- Placement/BOM: 73 populated BOM references and 73 consolidated PnP placements; U1 is included as an SMD module.
- DRC: 0 unconnected pads and no shorts, electrical clearance, hole-clearance, or net-parity faults. The 91 remaining warnings are 67 library-revision mismatches plus 24 silkscreen-only warnings. There are also 73 informational footprint/symbol field-metadata parity notices. Exported silkscreen is clipped from solder-mask openings.
- ERC: 0 errors and 39 warnings, all documented in `documentation/review-findings.md`.
- Procurement: exact orderable MPN and distributor part-number fields are populated for both IC/module designators, U1 and U2.

## Important remaining engineering risk

The display now operates from the battery-capable XIAO 3.3 V rail. Validate rail droop during simultaneous display refresh and Wi-Fi activity; the panel specification permits a high refresh inrush, and regulator margin has not been demonstrated by this file review.
