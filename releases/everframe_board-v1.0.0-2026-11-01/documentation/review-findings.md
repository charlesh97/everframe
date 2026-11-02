# EverFrame schematic and PCB review

Review date: 2026-11-01

## Outcome

The confirmed hardware bring-up blocker was reproduced from the design and fixed in the schematic and PCB:

1. **Q2 source and drain were reversed.** SI2301CDS is pin 2 = source and pin 3 = drain. The old schematic put pin 3 on `+3.3V` and pin 2 on the D4/L2 switch node. Q2 is now mirrored in the schematic and the PCB pad nets/routing are corrected. Source: [Vishay SI2301CDS datasheet](https://www.vishay.com/docs/68741/si2301cd.pdf).

Additional bring-up and assembly defects fixed:

- U2/display power was sourced only from the USB-derived `+5V` rail, so the display could not run from a battery. U2 input and C23 now use the XIAO `+3V3_ESP32` rail. Seeed documents that 5 V is not available when the XIAO is battery powered: [XIAO ESP32S3 getting started](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/).
- XIAO underside GND pad 20 was marked no-connect; it is now connected to GND in both schematic and PCB.
- XIAO pad 25 was present in the symbol but absent from the footprint. It was added using Seeed's current land geometry and assigned the schematic no-connect net. Source: [official XIAO KiCad footprint ZIP](https://files.seeedstudio.com/wiki/XIAO-KiCad-Library/New_XIAO_Series_Footprints.zip).
- XIAO BAT/GND pads 15 and 16 had been converted to plated slots. They are restored to the official SMD land geometry.
- J1 shield/hold-down tabs S1 and S2 had copper lands but no solder-mask openings or paste apertures. Both tabs now have matching mask and paste geometry for reflow assembly.
- U1's footprint lacked the KiCad `smd` assembly attribute, which omitted the XIAO module from SMD-only pick-and-place export. The attribute is now present in both the placed footprint and project library; the release PnP contains all 73 populated BOM references.
- Project-local symbol and footprint library tables were added, removing the unavailable-library ERC/DRC warnings.
- Zone fills were regenerated after routing changes.

## Verification

- KiCad 10.0.7 schematic netlist export: pass.
- Corrected netlist checks:
  - Q2 pin 2/source -> `+3.3V`.
  - Q2 pin 3/drain -> `Net-(D4-K)` / L2 switch node.
  - U1 pin 2/GPIO2 -> explicit no-connect.
  - U1 pin 7/GPIO43 -> `ESP32_CS`, matching the current firmware.
  - U1 pin 20 -> GND.
  - U2 pin 1 and C23 pin 1 -> `+3V3_ESP32`.
- PCB DRC with zone refill and schematic parity:
  - 0 shorts.
  - 0 clearance or hole-clearance violations.
  - 0 unconnected items.
  - 0 schematic/PCB net conflicts.
  - 67 footprint-library revision mismatch warnings remain.
  - 73 informational footprint/symbol field-metadata parity notices remain; there are no net-assignment parity conflicts.
- Solder-paste aperture audit:
  - 242 SMD copper pads and 242 paste apertures.
  - 0 missing, wrong-side, sub-0.15 mm, or greater-than-10 mm² rectangular-equivalent apertures.
  - D5 intentionally uses two separate reduced paste-only apertures over its two copper pads.
  - J1's two custom shield-tab apertures are present and match their copper lands.
- ERC: no newly introduced electrical errors. Thirty-nine existing warnings remain: 35 `C_Small` library-cache mismatches, two isolated SI2/SI3 label warnings, one local/global power-label naming warning, and one multiple-net-name warning.

## Remaining risks

- **3.3 V current budget:** the panel specification lists up to 174.2 mA inrush. The display now shares the XIAO 3.3 V regulator with the ESP32-S3. Validate the worst-case display-refresh plus Wi-Fi current and rail droop on hardware. A production revision should use a dedicated battery-capable 3.3 V buck-boost/load-switch rail if margin is insufficient. References: [`reference/7.3inch_e-Paper_(F)_Specification.pdf`](../../../reference/7.3inch_e-Paper_(F)_Specification.pdf), [TI TLV755P](https://www.ti.com/product/TLV755P), and [current XIAO schematic](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/XIAO_ESP32S3_V1.3_SCH_260115.pdf).
- **Connector mating:** J1 is an Amphenol bottom-contact FPC connector. Confirm that the production panel tail exposes contacts on the mating side before release. Source: [Amphenol 62684 series](https://www.amphenol-cs.com/product/62684501100alf.html).
- **Protection/testability:** battery reverse-polarity protection, a separable current-measurement point, and dedicated debug/test pads are still absent.
- **Non-electrical DRC cleanup:** 91 warnings remain: 67 placed-footprint versus installed-library revision mismatches, 19 silkscreen-over-copper warnings, and five silkscreen-to-edge warnings. These are not connectivity or clearance faults; fabrication Gerbers were exported with silkscreen clipped from solder-mask openings.
- Existing files under `output/` predate these edits and must not be sent to fabrication. Use the dated package under `releases/` instead.

## Review method

The review used the `schematic-review`, `layout-review`, `ee-schematic-rules`, `ee-layout-rules`, `schematic-capture`, `symbol-footprint`, and `pcb-routing` workflows from `ee-skills`, plus four independent subagent reviews for schematic connectivity, PCB geometry/routing, XIAO/reference-design cross-checking, and bring-up fault isolation.
