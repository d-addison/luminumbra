#pragma once

#include <span>
#include <string>

namespace Luminumbra::Rendering {

// Shared by initial compilation and reload. With no defines, source is left
// byte-identical. Failure leaves it unchanged and supplies a compile diagnostic.
bool InjectShaderDefines(std::string& source,
                         std::span<const std::string> defines,
                         std::string& diagnostic);

} // namespace Luminumbra::Rendering
