// ============================================================
// ShieldCord — token_vault.cpp
// Layer 2: Discord Token Vault (stub — DACL-only for now)
// Full implementation requires minifilter or DLL injection.
// See PLAN.md Section 5.6 for details.
// ============================================================
#include "token_vault.h"
#include "logger.h"

namespace sc {

TokenVault& TokenVault::Instance() {
    static TokenVault inst;
    return inst;
}

bool TokenVault::Initialize() {
    LOG_INFO(L"TokenVault", L"Token Vault is DISABLED (DACL-only mode). "
             L"Enable in config.json when advanced mode is ready.");
    m_initialized = true;
    return true;
}

void TokenVault::Shutdown() {
    m_initialized = false;
}

} // namespace sc
