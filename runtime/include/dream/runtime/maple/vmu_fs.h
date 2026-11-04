// What is on a memory card, read from the card's own file system: the free space, and one entry
// per file with the description and icon the console's file manager would show. And the file
// manager's two edits, copying a save to another card and deleting one, so cards can be shared
// between titles the way a real one is.
//
// All from the 128 KB image alone, so a settings screen or the launcher can work on a card without
// the title's help. The layout is the one MemoryCard::format() writes
// (docs/vmu-creation-study.md): the system block at 255, the allocation table at 254, the
// directory growing down from 253.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dream::maple {

struct VmuFile {
    std::string name;         // the twelve-character directory name, trailing spaces dropped
    std::string description;  // the file header's short description, printable ASCII only
    bool game = false;        // a mini-game for the card itself rather than a save
    bool no_copy = false;     // the title marked it copy-protected
    unsigned blocks = 0;
    // The first frame of the file's icon, 32x32, RGBA with red in the low byte. Empty when the
    // file carries no icon or its header could not be read.
    std::vector<std::uint32_t> icon;
};

struct VmuContents {
    bool formatted = false;
    unsigned free_blocks = 0;  // of the save area
    unsigned user_blocks = 0;  // the save area's size, 200 on a standard card
    std::vector<VmuFile> files;
};

constexpr unsigned kVmuIconSize = 32;

// Anything malformed -- a bad chain, an out-of-range block -- ends that file's reading rather than
// the listing: a damaged save is still worth seeing in the list.
VmuContents read_vmu(const std::vector<std::uint8_t>& image);

// A blank, formatted card, as MemoryCard::format() makes it.
std::vector<std::uint8_t> blank_vmu();

enum class VmuEdit {
    Done,
    NotACard,       // an image is not a formatted 128 KB card
    NotFound,       // no file of that name
    Exists,         // the destination has a file of that name: a title keeps one of each
    NoSpace,        // not enough free blocks
    DirectoryFull,  // no free directory entry
    Protected,      // the title marked the save copy-protected
    MiniGame,       // mini-games must start at block 0; copying them is not supported
};

// Copies the save called `name` (as read_vmu lists it) from one card to another, as the console's
// file manager does: the blocks are taken from the top of the free area down, chained in the
// allocation table, and the directory entry is copied with its new first block. `to` is left as
// it was unless the result is Done. Copy protection is honoured unless `ignore_protection`, which
// is for moving a player's own saves between cards they keep.
VmuEdit copy_vmu_file(const std::vector<std::uint8_t>& from, std::string_view name,
                      std::vector<std::uint8_t>& to, bool ignore_protection = false);

// Deletes the file called `name`: its blocks become free and its directory entry empty.
VmuEdit delete_vmu_file(std::vector<std::uint8_t>& image, std::string_view name);

}  // namespace dream::maple
