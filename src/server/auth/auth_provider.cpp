// SPDX-License-Identifier: Apache-2.0
#include "server/auth/auth_provider.hpp"

#include <atomic>

namespace t2d::auth {

namespace {
class DisabledProvider : public IAuthProvider
{
public:
    AuthResult validate(std::string_view token) override
    {
        // Always accept; generate synthetic id from token hash or fallback
        AuthResult r;
        r.ok = true;
        if (token.empty())
            r.user_id = "anon";
        else
            r.user_id = std::string(token.substr(0, 8));
        return r;
    }
};

class StubProvider : public IAuthProvider
{
public:
    explicit StubProvider(std::string prefix) : m_prefix(std::move(prefix)) {}

    AuthResult validate(std::string_view token) override
    {
        AuthResult r;
        if (token.empty()) {
            r.ok = false;
            r.reason = "empty_token";
            return r;
        }
        r.ok = true;
        r.user_id = m_prefix + std::string(token.substr(0, 10));
        return r;
    }

private:
    std::string m_prefix;
};

std::atomic<IAuthProvider *> g_provider{nullptr};
} // namespace

std::unique_ptr<IAuthProvider> make_provider(const std::string &mode, const std::string &stub_prefix)
{
    if (mode == "disabled")
        return std::make_unique<DisabledProvider>();
    if (mode == "stub")
        return std::make_unique<StubProvider>(stub_prefix);
    // default fallback
    return std::make_unique<DisabledProvider>();
}

void set_provider(IAuthProvider *p) noexcept
{
    g_provider.store(p, std::memory_order_release);
}

IAuthProvider *provider() noexcept
{
    return g_provider.load(std::memory_order_acquire);
}

} // namespace t2d::auth
