# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Chili.CHIPS
#
# wcx_pcie_tlp: openPCIE's TLP decoder (TLP Downstream, TLP Upstream).

file(GLOB _tlp_unit_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_*.c")
list(SORT _tlp_unit_tests)

wcx_add_decoder(
    NAME         pcie_tlp
    ID           openpcie.pcie_tlp_down
    SOURCES      src/tlp_format.c
                 src/tlp_plugin.c
    INCLUDE_DIRS src
    UNIT_TESTS   ${_tlp_unit_tests}
    FIXTURES_DIR fixtures
    FUZZ_TARGET  fuzz/fuzz_pcie_tlp.c)
