// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Must NOT compile: it uses strcpy, which banned.h poisons. The
// common.banned.rejects test builds this target and expects the build to
// fail. If this file ever compiles, the banned list has stopped working.

#include <string.h>

int main(void) {
    char dst[8];
    strcpy(dst, "x");
    return dst[0] == 'x' ? 0 : 1;
}
