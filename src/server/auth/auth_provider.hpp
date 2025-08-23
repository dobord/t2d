// auth_provider.hpp
// Simple pluggable authentication strategy abstraction (stub OAuth placeholder).
#pragma once
#include <memory>
#include <string>
#include <string_view>

namespace t2d::auth {

struct AuthResult
{
    bool ok{false};
    std::string user_id; // filled when ok
    std::string reason; // error reason when !ok
};

class IAuthProvider
{
public:
    virtual ~IAuthProvider() = default;
    virtual AuthResult validate(std::string_view token) = 0; // synchronous prototype
};

// Factory for provider by mode string ("disabled", "stub")
std::unique_ptr<IAuthProvider> make_provider(const std::string &mode, const std::string &stub_prefix);

// Global pointer (prototype DI); set at startup before any auth usage.
void set_provider(IAuthProvider *p) noexcept;
IAuthProvider *provider() noexcept;

} // namespace t2d::auth
