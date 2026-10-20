# PCIe TLP decoder (openPCIE)

A free, open-source TLP decoder for [WaveCrux](https://wavecrux.app), written
by Chili.CHIPS for [openPCIE](https://github.com/chili-chips-ba/openPCIE) and
[openCologne-PCIE](https://github.com/chili-chips-ba/openCologne-PCIE). It
names every Transaction Layer Packet and pairs each completion with the
request it answers.

It is one layer of a stack. Ferrite's open [PCIe plugin](../pcie/) decodes
the link below the TLPs; WaveCrux Pro's
[PCIe TLP analysis](https://github.com/Ferrite-Engineering/wavecrux/blob/main/docs-site/docs/protocol-decoders.md#pcie-tlp-analysis)
checks and measures the TLPs. This decoder reads them for free, on every
WaveCrux edition, and does not repeat what Pro does.

## Who covers what

| | This decoder (free) | [Ferrite PCIe plugin](../pcie/) (free) | WaveCrux Pro PCIe TLP |
|---|---|---|---|
| Serial line, 8b/10b, PIPE ordered sets | | ✔ | |
| DLLPs, Ack/Nak, flow control, LCRC, sequence numbers | | ✔ | |
| TLP header name at the DLL (`TLP seq=5 MWr32 len=1`) | | ✔ | |
| Every TLP decoded: address, register name, value, status | ✔ | | ✔ |
| Completion paired with its request | ✔ | | ✔ |
| Latency, timeouts, tag reuse, byte count checks | | | ✔ |
| Base Spec rule checks, Max_Payload_Size | | | ✔ |
| 64 to 256-bit beats; Xilinx and Intel core presets | | | ✔ |
| Bus statistics | | | ✔ |

Start here, add Ferrite's plugin to see the link, and move to Pro when you
need the checks. `answers`, `register_name` and `value` mean what they mean
in Pro; the TLP name is in `type` here (this repository's convention) and in
`tlp` in Pro.

## Use

**Signals:** one TLP DW per clock: `clk`, `tlp_data` (32), `tlp_sop`,
`tlp_eop`, `tlp_valid`; optionally the opposite direction as `peer_data`,
`peer_sop`, `peer_eop` and `peer_valid`, to pair completions. openPCIE's
`tlp_dw_monitor.sv` produces this stream.

**Decoders:** `openpcie.pcie_tlp_down` (TLP Downstream) and
`openpcie.pcie_tlp_up` (TLP Upstream). They are the same decoder, registered
twice so the two rows of a link get different names.

**Parameters:** `labels` (`compact` or `full`), `span` (`packet` or
`until_next`). The output contract is in [`SPEC.md`](SPEC.md).

**Needs** WaveCrux 1.0.1 or later.

**In use:** openPCIE's test bench, with a ready-made session
([5.sim: Viewing the TLPs in WaveCrux](https://github.com/chili-chips-ba/openPCIE/blob/main/5.sim/README.md#viewing-the-tlps-in-wavecrux)),
and openCologne-PCIE's
([5.sim: WaveCrux decoders](https://github.com/chili-chips-ba/openCologne-PCIE/blob/main/5.sim/README.md#wavecrux-decoders)).

## Planned

Only what neither Ferrite's plugin nor Pro covers today. We agree each item
with Ferrite before building it.

1. **TLPs from the DLL decoder** ([#6](https://github.com/Ferrite-Engineering/wavecrux-decoders/issues/6)):
   decode the TLPs Ferrite's DLL decoder frames, so a PIPE or serial dump
   gets a TLP view without a DW-stream adapter.
2. **BAR sizing in one row:** turn the write-all-ones, read-back, write-address
   sequence into `BAR0: 4 KB, 32-bit, non-prefetchable, at 0x80000000`.
3. **Enumeration through switches:** Type 1 bus numbers (primary, secondary,
   subordinate), shown as the topology the Root Complex built.
4. **Register values decoded:** Command and Status bits by name, class codes
   (`Ethernet controller`), Header Type, Interrupt Pin.
5. **Message names:** `Assert_INTA`, `PM_Active_State_Nak`, vendor-defined
   messages, instead of a bare message code.

## Opensource PCIE ecosystem

This decoder was written for our two NLnet-funded open-source PCIE projects,
the two ends of one link. It decodes openPCIE's co-simulation today, and
openCologne-PCIE's once its EndPoint RTL runs in the test bench.

| [openPCIE](https://github.com/chili-chips-ba/openPCIE) | [openCologne-PCIE](https://github.com/chili-chips-ba/openCologne-PCIE) |
|:---:|:---:|
| <a href="https://github.com/chili-chips-ba/openPCIE"><img alt="openPCIE logo" width="160" src="https://raw.githubusercontent.com/chili-chips-ba/openPCIE/main/0.doc/artwork/pcie2rootc.hex.png"></a> | <a href="https://github.com/chili-chips-ba/openCologne-PCIE"><img alt="openCologne-PCIE logo" width="160" src="https://raw.githubusercontent.com/chili-chips-ba/openCologne-PCIE/main/0.doc/artwork/openCologne-PCIE.hex.png"></a> |
| **Root Complex**: a PCIE Gen2 RC on AMD Artix-7, driven by a soft RISC-V SoC | **EndPoint**: a soft PCIE EP protocol stack on Cologne Chip GateMate |

**In openPCIE:**
- RTL around the Xilinx PCIE and GTP hard macros, with our eduBOS5 RISC-V core; on hardware, Gen2 x4 and x1 links to an EP, and a link through a PCIE switch
- a layered C driver and a demo app
- full HW/SW co-simulation (wyvernSemi's VProc and pcieVHost), with ready-made WaveCrux sessions at TLP, DLL and PIPE level
- an open backplane PCB in KiCad, heading to Crowd Supply
- builds with the open-source openXC7 flow, not only Vivado

**In openCologne-PCIE:**
- a layered, portable EP stack (PHY, PIPE, MAC, DLL), minimal by design: x1, Gen1 (Gen2 best effort), PIO reads and writes
- PCIE cards for GateMate (A1, A2, A4), plug-and-play with the openPCIE backplane
- an open-source SerDes BFM: a drop-in for the GateMate SerDes hard macro, whose vendor model is under NDA. It is checked equivalent to that model, its 8b/10b is formally proven, and it simulates about 20 times faster
- verification IP: pcieVHost co-simulation, DLL and PHY simulation passing; WaveCrux views of the DLL
- builds with the open-source GateMate flow (Yosys, nextpnr)
- work in progress: the top-level simulation, the link on hardware, LiteX integration

**The two ends of the link** (click a diagram for its description):

| openPCIE Root Complex RTL | openCologne-PCIE EndPoint RTL |
|:---:|:---:|
| <a href="https://github.com/chili-chips-ba/openPCIE#rtl-architecture"><img alt="openPCIE Root Complex RTL block diagram" width="100%" src="https://raw.githubusercontent.com/chili-chips-ba/openPCIE/main/0.doc/2.rtl/block-diagram.drawio.png"></a> | <a href="https://github.com/chili-chips-ba/openCologne-PCIE#rtl-architecture"><img alt="openCologne-PCIE EndPoint RTL block diagram" width="100%" src="https://raw.githubusercontent.com/chili-chips-ba/openCologne-PCIE/main/0.doc/2.rtl/block-diagram.drawio.png"></a> |

**The openCologne-PCIE test bench:** [wyvernSemi](https://github.com/wyvernSemi)'s
[pcieVHost](https://github.com/wyvernSemi/pcievhost), by Simon Southwell, plays
the Root Complex and drives our EndPoint at its PIPE interface.

<p align="center">
<a href="https://github.com/chili-chips-ba/openCologne-PCIE#simulation-architecture"><img alt="openCologne-PCIE test bench: pcieVHost as the host, driving the EndPoint RTL at its PIPE interface" width="80%" src="https://raw.githubusercontent.com/chili-chips-ba/openCologne-PCIE/main/0.doc/5.sim/tb-structure.drawio.png"></a>
</p>

Together they work toward a fully open PCIE stack, with only the PHY left
in a hard macro. And the PHY is next: an open-source SerDes for the
[GF180MCU](https://github.com/google/gf180mcu-pdk) process node is in the
works, tracked by [wafer.space](https://wafer.space), who run shuttles on it.

## Community Outreach

- [2026-10-20, LinkedIn: Martin Fink (Ferrite), "Our first testimonial is in"](https://lnkd.in/p/g-yGBEU8)

## Build

```
cmake --preset dev && cmake --build --preset dev && ctest --preset dev -R pcie_tlp
python3 decoders/pcie-tlp/tools/generate_fixtures.py --check
```

Licence: Apache-2.0, Chili.CHIPS.
