// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// One Unity configuration for every environment. PlatformIO only generates one for a board with a framework, and the Cortex-M environments have none.

#pragma once

#if defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')

    // The Cortex-M environments only compile and link the tests, so nothing needs to print.
    #define UNITY_OUTPUT_CHAR(c) ((void) (c))
    #define UNITY_OUTPUT_FLUSH()

#else

    #include <stdio.h>

    #define UNITY_OUTPUT_CHAR(c) putchar(c)
    #define UNITY_OUTPUT_FLUSH() fflush(stdout)

#endif
