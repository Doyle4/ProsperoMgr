#pragma once
// Native PS5 toasts. Notify() carries ProsperoMgr's own icon (sceNotificationSend rich
// toast, see notify.cpp) and falls back to plain text if that call fails.

#include <string>

namespace prospero {

void Notify(const std::string &message);

// Plain text-only toast (sceKernelSendNotificationRequest) — no icon. Use for routine
// technical status lines that don't need the branded rich toast.
void NotifyDebug(const std::string &message);

} // namespace prospero
