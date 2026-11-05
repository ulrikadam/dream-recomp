#pragma once
#include <memory>

#include "dream/runtime/wince/media.h"
#include "dream/runtime/wince/process.h"

namespace dream::wince {
// Guest COM ABI, using 32-bit pointers and SH-4 integer/stack arguments. No host COM objects
// or Windows DLLs are exposed to guest code. See docs/wince-directx.md for supported methods.
class DirectX {
public:
    explicit DirectX(ProcessMemory& memory);
    ~DirectX();
    DirectX(const DirectX&) = delete;
    DirectX& operator=(const DirectX&) = delete;
    std::optional<std::uint32_t> resolve(const Import& symbol) const;
    bool invoke(sh4::Ctx& cpu, Memory& memory, std::uint32_t target);
    void mix(std::span<std::int16_t> stereo);
    const std::vector<std::uint32_t>& frame() const;
    std::uint32_t width() const;
    std::uint32_t height() const;
    std::uint64_t presents() const;
    const std::string& diagnostic() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace dream::wince
