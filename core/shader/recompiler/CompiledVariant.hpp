#ifndef CORE_SHADER_RECOMPILER_COMPILEDVARIANT_HPP
#define CORE_SHADER_RECOMPILER_COMPILEDVARIANT_HPP

#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrMetadata/CompiledShaderInfo.hpp"
#include "Optimization/include/Optimization/BindingAllocator.hpp"
#include <array>
#include <cstddef>

namespace ShaderRecompiler {

struct CompiledVariant {
    BindingLayout layout;
    CompiledShaderInfo info;
    CompiledBindingLayout bindings;
    CompiledShaderArtifact artifact;
    // The digest of this variant's own disk cache entry name (ShaderDiskCache::BuildArtifactIdentity
    // of the key it was compiled or loaded for), set where that entry is named. It is what a
    // specialized module or a prepared binding plan is keyed on besides the specialization inputs,
    // and it stays empty for a variant built without the disk cache, which is also why it is not
    // stored: the variant entry's key already names itself.
    std::array<std::byte, 16> artifactIdentity{};
};

}

#endif
