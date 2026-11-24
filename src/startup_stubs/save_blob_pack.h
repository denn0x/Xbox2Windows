// Experimental (docs/startup-runtime.md): the caller-buffer layout of an XGameSave blob read.
// One 24-byte record per blob (payload pointer at offset 16), then each name, a NUL and the
// payload. The records point into that same buffer. Synchronous and asynchronous reads share this.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace x2w_save_pack {

struct BlobInfo {
    const char* name;
    std::uint32_t size;
};
struct Blob {
    BlobInfo info;
    unsigned char* data;
};
static_assert(sizeof(Blob) == 24 && offsetof(Blob, data) == 16, "XGameSaveBlob layout");

struct Piece {
    std::string name;
    std::vector<unsigned char> bytes;
};

// False when the byte count does not fit in size_t. out is unchanged then.
inline bool bytes_needed(const std::vector<Piece>& pieces, std::size_t& out)
{
    if (pieces.size() > SIZE_MAX / sizeof(Blob)) return false;
    std::size_t bytes = pieces.size() * sizeof(Blob);
    for (const Piece& piece : pieces) {
        if (piece.name.size() == SIZE_MAX) return false;
        const std::size_t extra = piece.name.size() + 1 + piece.bytes.size();
        if (extra < piece.name.size() || extra > SIZE_MAX - bytes) return false;
        bytes += extra;
    }
    out = bytes;
    return true;
}

// dest has room for bytes_needed. Pointers in the records address dest itself.
inline void write(const std::vector<Piece>& pieces, void* dest)
{
    auto* records = static_cast<Blob*>(dest);
    auto* cursor = reinterpret_cast<unsigned char*>(records + pieces.size());
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const Piece& piece = pieces[i];
        auto* name = reinterpret_cast<char*>(cursor);
        if (!piece.name.empty()) std::memcpy(cursor, piece.name.data(), piece.name.size());
        cursor[piece.name.size()] = 0;
        cursor += piece.name.size() + 1;
        records[i].info = {name, static_cast<std::uint32_t>(piece.bytes.size())};
        records[i].data = cursor;
        if (!piece.bytes.empty()) std::memcpy(cursor, piece.bytes.data(), piece.bytes.size());
        cursor += piece.bytes.size();
    }
}

}