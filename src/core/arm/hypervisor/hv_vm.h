// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Hypervisor {

bool IsAvailable();

void SelectForProcess(bool is_64bit);

}
