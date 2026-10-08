# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
#
# wcx_pcie: the PCIe PIPE and Data Link Layer decoders (eight decoder ids,
# one per PIPE data width and layer; see SPEC.md §1).

file(GLOB _pcie_unit_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_*.c")
list(SORT _pcie_unit_tests)

wcx_add_decoder(
    NAME         pcie
    ID           ferrite.pcie_pipe_w16
    SOURCES      src/pcie_crc.c
                 src/pcie_scrambler.c
                 src/pcie_pipeline.c
                 src/pcie_sink.c
                 src/pcie_fe_common.c
                 src/pcie_pipe_fe.c
                 src/pcie_dll_fe.c
                 src/pcie_detector.c
                 src/pcie_plugin.c
    INCLUDE_DIRS src
    UNIT_TESTS   ${_pcie_unit_tests}
    FIXTURES_DIR fixtures
    FUZZ_TARGET  fuzz/fuzz_pcie.c)

# Hand-written smoke fixtures (tests/vcd/README.md derives every expected
# transaction by hand): the same golden + lifecycle pair as fixtures/, under
# the test names pcie.golden.smoke.* and pcie.lifecycle.smoke.*.
file(GLOB _pcie_smoke_vcds CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/vcd/*.vcd")
list(SORT _pcie_smoke_vcds)
foreach(vcd IN LISTS _pcie_smoke_vcds)
    wcx_add_fixture_tests(pcie wcx_pcie ferrite.pcie_pipe_w16 smoke "${vcd}")
endforeach()
