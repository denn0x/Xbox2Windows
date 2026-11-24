#include "dstorage_rfc1950.h"

#include <zlib.h>
#include <climits>
#include <cstring>
#include <memory>
#include <new>

namespace dstorage_rfc1950 {

DecodeResult decode(const DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST& request) noexcept
{
    if (request.CompressionFormat != DSTORAGE_CUSTOM_COMPRESSION_0 || request.Flags != 0)
        return {E_NOTIMPL, 0, 0};
    if (!request.SrcBuffer || !request.DstBuffer || !request.SrcSize || !request.DstSize ||
        request.SrcSize > ULONG_MAX || request.DstSize > ULONG_MAX)
        return {E_INVALIDARG, 0, 0};

    // A truncated trailer or bad checksum may be reported after zlib has
    // written output. Publish the bytes only once every size and checksum agrees.
    auto output = std::unique_ptr<Bytef[]>(new (std::nothrow) Bytef[static_cast<size_t>(request.DstSize)]);
    if (!output) return {E_OUTOFMEMORY, 0, 0};
    uLong used = static_cast<uLong>(request.SrcSize);
    uLongf written = static_cast<uLongf>(request.DstSize);
    const int result = uncompress2(output.get(), &written, static_cast<const Bytef*>(request.SrcBuffer), &used);
    if (result == Z_MEM_ERROR) return {E_OUTOFMEMORY, used, written};
    if (result != Z_OK || used != request.SrcSize || written != request.DstSize)
        return {HRESULT_FROM_WIN32(ERROR_INVALID_DATA), used, written};
    std::memcpy(request.DstBuffer, output.get(), written);
    return {S_OK, used, written};
}

HRESULT complete_one(IDStorageCustomDecompressionQueue* queue, DecodeResult& decoded) noexcept
{
    decoded = {E_PENDING, 0, 0};
    if (!queue) return E_POINTER;
    DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST request{};
    UINT32 count{};
    const HRESULT hr = queue->GetRequests(1, &request, &count);
    if (FAILED(hr)) return hr;
    if (!count) return S_FALSE;
    decoded = decode(request);
    DSTORAGE_CUSTOM_DECOMPRESSION_RESULT result{request.Id, decoded.status};
    return queue->SetRequestResults(1, &result);
}

}
