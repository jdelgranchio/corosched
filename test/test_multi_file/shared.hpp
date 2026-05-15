// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <corosched/corosched.hpp>

corosched::task wait_for_value(corosched::task_frame<1024> &frame, corosched::slot<int> &slot, int &out);
