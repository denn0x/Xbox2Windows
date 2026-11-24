#pragma once

#include <d3d12.h>
#include <dstorage.h>

namespace dstorage_rfc1950 {

// Custom format zero is assigned to RFC 1950 by this adapter. One request
// contains exactly one stream, with the declared uncompressed byte count.
struct DecodeResult {
    HRESULT status;
    UINT64 consumed;
    UINT64 produced;
};

DecodeResult decode(const DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST& request) noexcept;

// Service one job from a queue owned by this adapter. S_FALSE means empty.
// The return value describes the queue operation. The codec HRESULT is
// published under the dequeued request's ID and also returned in decoded.
HRESULT complete_one(IDStorageCustomDecompressionQueue* queue, DecodeResult& decoded) noexcept;

}
