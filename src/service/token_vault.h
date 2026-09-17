#pragma once
// ============================================================
// ShieldCord — token_vault.h
// Layer 2: Discord Token Vault interface (stub)
// ============================================================
#include "../shared/common.h"
#include <atomic>

namespace sc {

class TokenVault {
public:
    static TokenVault& Instance();
    bool Initialize();
    void Shutdown();
    bool IsActive() const { return m_initialized; }

private:
    TokenVault() = default;
    SC_DISALLOW_COPY(TokenVault)
    std::atomic_bool m_initialized{ false };
};

} // namespace sc
