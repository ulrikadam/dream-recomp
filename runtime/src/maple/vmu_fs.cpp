#include "dream/runtime/maple/vmu_fs.h"

#include <cstddef>
#include <cstring>

#include "dream/runtime/maple/maple.h"

namespace dream::maple {
namespace {

constexpr std::size_t kBlockSize = 512, kBlocks = 256;
constexpr std::uint16_t kFatFree = 0xFFFC, kFatLast = 0xFFFA;
constexpr std::size_t kEntrySize = 32;
// A save's header: descriptions, icon count, palette, then the icon frames.
constexpr std::size_t kHeaderDesc = 0x00, kHeaderDescLen = 16;
constexpr std::size_t kHeaderIcons = 0x40, kHeaderPalette = 0x60, kHeaderBitmap = 0x80;
constexpr std::size_t kIconBytes = kVmuIconSize * kVmuIconSize / 2;  // four bits a pixel

std::uint16_t u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::string text(const std::uint8_t* p, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n && p[i]; ++i)
        s.push_back(p[i] >= 0x20 && p[i] < 0x7F ? static_cast<char>(p[i]) : ' ');
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

// The first `want` bytes of a file, following its allocation chain from `first`, after skipping
// `skip` whole blocks. Short when the chain ends or goes somewhere it should not.
std::vector<std::uint8_t> read_chain(const std::vector<std::uint8_t>& image,
                                     const std::uint8_t* fat, unsigned first, unsigned skip,
                                     std::size_t want) {
    std::vector<std::uint8_t> out;
    unsigned block = first;
    for (unsigned steps = 0; steps < kBlocks && out.size() < want; ++steps) {
        if (block >= kBlocks)
            break;
        if (skip > 0) {
            --skip;
        } else {
            const std::uint8_t* p = image.data() + block * kBlockSize;
            const std::size_t n = want - out.size() < kBlockSize ? want - out.size() : kBlockSize;
            out.insert(out.end(), p, p + n);
        }
        const std::uint16_t next = u16(fat + block * 2);
        if (next == kFatLast || next == kFatFree)
            break;
        block = next;
    }
    return out;
}

// ARGB4444 to RGBA with red in the low byte; each four-bit channel stretched to eight.
std::uint32_t argb4444(std::uint16_t c) {
    const std::uint32_t a = ((c >> 12) & 0xF) * 17u, r = ((c >> 8) & 0xF) * 17u,
                        g = ((c >> 4) & 0xF) * 17u, b = (c & 0xF) * 17u;
    return r | (g << 8) | (b << 16) | (a << 24);
}

void read_header(const std::vector<std::uint8_t>& image, const std::uint8_t* fat, VmuFile& f,
                 unsigned first, unsigned header_block) {
    const std::vector<std::uint8_t> h =
        read_chain(image, fat, first, header_block, kHeaderBitmap + kIconBytes);
    if (h.size() >= kHeaderDesc + kHeaderDescLen)
        f.description = text(h.data() + kHeaderDesc, kHeaderDescLen);
    if (h.size() < kHeaderBitmap + kIconBytes || u16(h.data() + kHeaderIcons) == 0)
        return;
    std::uint32_t palette[16];
    for (unsigned i = 0; i < 16; ++i) palette[i] = argb4444(u16(h.data() + kHeaderPalette + i * 2));
    f.icon.resize(kVmuIconSize * kVmuIconSize);
    for (std::size_t i = 0; i < kIconBytes; ++i) {
        const std::uint8_t b = h[kHeaderBitmap + i];
        f.icon[i * 2] = palette[b >> 4];  // the high nibble is the left pixel
        f.icon[i * 2 + 1] = palette[b & 0xF];
    }
}

void put16(std::uint8_t* p, unsigned v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

// Where a formatted card keeps things, read from its system block and checked to be in range.
struct Layout {
    unsigned fat = 0, dir = 0, dir_blocks = 0, user_blocks = 0;
    unsigned first_system() const { return dir + 1 - dir_blocks; }  // the lowest directory block
};

bool layout(const std::vector<std::uint8_t>& image, Layout& l) {
    if (image.size() != kBlocks * kBlockSize)
        return false;
    const std::uint8_t* root = image.data() + 255 * kBlockSize;
    for (unsigned i = 0; i < 16; ++i)
        if (root[i] != 0x55)
            return false;
    l.fat = u16(root + 0x46);
    l.dir = u16(root + 0x4A);
    l.dir_blocks = u16(root + 0x4C);
    l.user_blocks = u16(root + 0x50);
    return l.fat < kBlocks && l.dir < kBlocks && l.dir_blocks >= 1 && l.dir_blocks <= l.dir + 1 &&
           l.user_blocks <= l.first_system();
}

// Every directory slot, as an offset into the image, in the order the directory runs.
std::vector<std::size_t> slots(const Layout& l) {
    std::vector<std::size_t> out;
    for (unsigned k = 0; k < l.dir_blocks; ++k)
        for (std::size_t e = 0; e < kBlockSize / kEntrySize; ++e)
            out.push_back((l.dir - k) * kBlockSize + e * kEntrySize);
    return out;
}

bool in_use(const std::uint8_t* entry) {
    return entry[0] == 0x33 || entry[0] == 0xCC;
}

// The directory entry of the file read_vmu would list as `name`, or npos.
std::size_t find_entry(const std::vector<std::uint8_t>& image, const Layout& l,
                       std::string_view name) {
    for (std::size_t off : slots(l))
        if (in_use(image.data() + off) && text(image.data() + off + 4, 12) == name)
            return off;
    return static_cast<std::size_t>(-1);
}

}  // namespace

std::vector<std::uint8_t> blank_vmu() {
    MemoryCard card;
    card.format();
    return card.image();
}

VmuEdit copy_vmu_file(const std::vector<std::uint8_t>& from, std::string_view name,
                      std::vector<std::uint8_t>& to, bool ignore_protection) {
    Layout lf, lt;
    if (!layout(from, lf) || !layout(to, lt))
        return VmuEdit::NotACard;
    const std::size_t src = find_entry(from, lf, name);
    if (src == static_cast<std::size_t>(-1))
        return VmuEdit::NotFound;
    const std::uint8_t* entry = from.data() + src;
    if (entry[0] == 0xCC)
        return VmuEdit::MiniGame;
    if (entry[1] == 0xFF && !ignore_protection)
        return VmuEdit::Protected;
    if (find_entry(to, lt, name) != static_cast<std::size_t>(-1))
        return VmuEdit::Exists;

    // The source's blocks, in order; a chain that does not match the size it claims is refused
    // rather than copied half.
    const unsigned size = u16(entry + 0x18);
    const std::uint8_t* from_fat = from.data() + lf.fat * kBlockSize;
    std::vector<unsigned> chain;
    for (unsigned b = u16(entry + 2); size > 0 && chain.size() < size;) {
        if (b >= lf.first_system())
            return VmuEdit::NotACard;
        chain.push_back(b);
        const std::uint16_t next = u16(from_fat + b * 2);
        if (next == kFatLast || next == kFatFree)
            break;
        b = next;
    }
    if (size == 0 || chain.size() != size)
        return VmuEdit::NotACard;

    // Free blocks from the top of the save area down, and a free directory slot.
    std::uint8_t* to_fat = to.data() + lt.fat * kBlockSize;
    std::vector<unsigned> blocks;
    for (unsigned b = lt.user_blocks; b-- > 0 && blocks.size() < size;)
        if (u16(to_fat + b * 2) == kFatFree)
            blocks.push_back(b);
    if (blocks.size() < size)
        return VmuEdit::NoSpace;
    std::size_t slot = static_cast<std::size_t>(-1);
    for (std::size_t off : slots(lt))
        if (!in_use(to.data() + off)) {
            slot = off;
            break;
        }
    if (slot == static_cast<std::size_t>(-1))
        return VmuEdit::DirectoryFull;

    for (std::size_t i = 0; i < size; ++i) {
        std::memcpy(to.data() + blocks[i] * kBlockSize, from.data() + chain[i] * kBlockSize,
                    kBlockSize);
        put16(to_fat + blocks[i] * 2, i + 1 < size ? blocks[i + 1] : kFatLast);
    }
    std::memcpy(to.data() + slot, entry, kEntrySize);
    put16(to.data() + slot + 2, blocks[0]);
    return VmuEdit::Done;
}

VmuEdit delete_vmu_file(std::vector<std::uint8_t>& image, std::string_view name) {
    Layout l;
    if (!layout(image, l))
        return VmuEdit::NotACard;
    const std::size_t off = find_entry(image, l, name);
    if (off == static_cast<std::size_t>(-1))
        return VmuEdit::NotFound;
    std::uint8_t* fat = image.data() + l.fat * kBlockSize;
    // Never past the save area into the directory, whatever a damaged chain says.
    unsigned b = u16(image.data() + off + 2);
    for (unsigned steps = 0; steps < kBlocks && b < l.first_system(); ++steps) {
        const std::uint16_t next = u16(fat + b * 2);
        put16(fat + b * 2, kFatFree);
        if (next == kFatLast || next == kFatFree)
            break;
        b = next;
    }
    std::memset(image.data() + off, 0, kEntrySize);
    return VmuEdit::Done;
}

VmuContents read_vmu(const std::vector<std::uint8_t>& image) {
    VmuContents c;
    if (image.size() != kBlocks * kBlockSize)
        return c;
    const std::uint8_t* root = image.data() + 255 * kBlockSize;
    for (unsigned i = 0; i < 16; ++i)
        if (root[i] != 0x55)
            return c;
    c.formatted = true;
    const unsigned fat_block = u16(root + 0x46);
    const unsigned dir_block = u16(root + 0x4A), dir_blocks = u16(root + 0x4C);
    c.user_blocks = u16(root + 0x50);
    if (fat_block >= kBlocks || dir_block >= kBlocks || c.user_blocks > kBlocks)
        return c;
    const std::uint8_t* fat = image.data() + fat_block * kBlockSize;
    for (unsigned b = 0; b < c.user_blocks; ++b)
        if (u16(fat + b * 2) == kFatFree)
            ++c.free_blocks;

    // The directory occupies dir_blocks blocks counting down from dir_block, sixteen entries each.
    for (unsigned k = 0; k < dir_blocks && k <= dir_block; ++k) {
        const std::uint8_t* d = image.data() + (dir_block - k) * kBlockSize;
        for (std::size_t e = 0; e < kBlockSize / kEntrySize; ++e) {
            const std::uint8_t* entry = d + e * kEntrySize;
            const std::uint8_t type = entry[0];
            if (type != 0x33 && type != 0xCC)  // data and game; zero is an empty slot
                continue;
            VmuFile f;
            f.game = type == 0xCC;
            f.no_copy = entry[1] == 0xFF;
            f.name = text(entry + 4, 12);
            f.blocks = u16(entry + 0x18);
            const unsigned first = u16(entry + 2);
            // A game's header sits a block or so into the file, after its boot code.
            read_header(image, fat, f, first, f.game ? u16(entry + 0x1A) : 0u);
            c.files.push_back(std::move(f));
        }
    }
    return c;
}

}  // namespace dream::maple
