#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <iterator>
#include <vector>
#include <string_view>

using Microsoft::WRL::ComPtr;

// Use the validator and blob factory from the same SDK as the project's shader compiler.
int inspect_shader(std::vector<char>& bytes, std::string_view mode)
{
    HMODULE compiler = LoadLibraryExW(SDK_DXC_DIR L"dxcompiler.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!compiler || bytes.size() > UINT32_MAX) return 4;
    auto create_utils = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(compiler, "DxcCreateInstance"));
    if (!create_utils) return 4;
    if (mode == "--disassemble") {
        ComPtr<IDxcCompiler3> dx;
        ComPtr<IDxcResult> result;
        ComPtr<IDxcBlobUtf8> text;
        DxcBuffer input{bytes.data(), bytes.size(), 0};
        HRESULT status = E_FAIL;
        if (FAILED(create_utils(CLSID_DxcCompiler, IID_PPV_ARGS(&dx))) ||
            FAILED(dx->Disassemble(&input, IID_PPV_ARGS(&result))) ||
            FAILED(result->GetStatus(&status)) || FAILED(status) ||
            FAILED(result->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&text), nullptr)) || !text) return 5;
        std::fwrite(text->GetStringPointer(), 1, text->GetStringLength(), stdout);
        return 0;
    }
    HMODULE checker = LoadLibraryExW(SDK_DXC_DIR L"dxil.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    // A DXC without a separate dxil.dll (the SDK's 1.9.2602 is one) validates in dxcompiler.dll.
    if (!checker) checker = compiler;
    auto create_validator = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(checker, "DxcCreateInstance"));
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcValidator> validator;
    ComPtr<IDxcBlobEncoding> blob;
    if (!create_validator || FAILED(create_utils(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(create_validator(CLSID_DxcValidator, IID_PPV_ARGS(&validator))) ||
        FAILED(utils->CreateBlobFromPinned(bytes.data(), static_cast<UINT32>(bytes.size()), DXC_CP_ACP, &blob)))
        return 4;
    const bool sign = mode == "--sign";
    const UINT32 flags = mode == "--module" ? DxcValidatorFlags_ModuleOnly :
                         sign               ? DxcValidatorFlags_InPlaceEdit :
                                              DxcValidatorFlags_Default;
    ComPtr<IDxcOperationResult> result;
    HRESULT hr = validator->Validate(blob.Get(), flags, &result);
    if (sign) {
        HRESULT status = E_FAIL;
        if (FAILED(hr) || !result || FAILED(result->GetStatus(&status))) return 4;
        if (FAILED(status)) {
            ComPtr<IDxcBlobEncoding> errors;
            if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors && errors->GetBufferPointer()) {
                std::fwrite(errors->GetBufferPointer(), 1, (std::min)(errors->GetBufferSize(), size_t{2048}), stderr);
            }
            return 5;
        }
        ComPtr<IDxcBlob> signed_blob;
        const void* ptr = blob->GetBufferPointer();
        std::size_t size = blob->GetBufferSize();
        if (SUCCEEDED(result->GetResult(&signed_blob)) && signed_blob &&
            signed_blob->GetBufferPointer() && signed_blob->GetBufferSize()) {
            ptr = signed_blob->GetBufferPointer();
            size = signed_blob->GetBufferSize();
        }
        if (!ptr || !size) return 4;
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 4;
        if (std::fwrite(ptr, 1, size, stdout) != size || std::fflush(stdout) != 0) return 4;
        return 0;
    }
    std::printf("{\"call_hr\":\"0x%08lx\"", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && result) {
        HRESULT status = E_FAIL;
        if (FAILED(result->GetStatus(&status))) return 4;
        std::printf(",\"status\":\"0x%08lx\"", static_cast<unsigned long>(status));
        ComPtr<IDxcBlobEncoding> errors;
        if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors) {
            std::printf(",\"diagnostic_bytes\":%zu,\"diagnostics_truncated\":%s",
                        errors->GetBufferSize(), errors->GetBufferSize() > 2048 ? "true" : "false");
            std::fwrite(errors->GetBufferPointer(), 1, (std::min)(errors->GetBufferSize(), size_t{2048}), stderr);
        }
    }
    std::puts("}");
    return 0;
}

// Consume data on stdin. Neither parsing path creates a device.
int main(int argc, char** argv)
{
    _setmode(_fileno(stdin), _O_BINARY);
    std::vector<char> bytes{std::istreambuf_iterator<char>(std::cin), {}};
    if (bytes.empty() || std::cin.bad()) return 2;
    if (argc == 2) {
        std::string_view mode(argv[1]);
        if (mode == "--container" || mode == "--module" || mode == "--disassemble" || mode == "--sign")
            return inspect_shader(bytes, mode);
        return 2;
    }
    if (argc != 1) return 2;
    ComPtr<ID3D12RootSignatureDeserializer> legacy;
    ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
    HRESULT old_hr = D3D12CreateRootSignatureDeserializer(bytes.data(), bytes.size(), IID_PPV_ARGS(&legacy));
    HRESULT new_hr = D3D12CreateVersionedRootSignatureDeserializer(bytes.data(), bytes.size(), IID_PPV_ARGS(&reader));
    std::printf("{\"legacy_hr\":\"0x%08lx\",\"versioned_hr\":\"0x%08lx\"",
                static_cast<unsigned long>(old_hr), static_cast<unsigned long>(new_hr));
    if (SUCCEEDED(new_hr) && reader) {
        const auto* source = reader->GetUnconvertedRootSignatureDesc();
        if (!source) return 3;
        std::printf(",\"version\":%u", static_cast<unsigned>(source->Version));
        ComPtr<ID3DBlob> original;
        HRESULT hr = D3D12SerializeVersionedRootSignature(source, &original, nullptr);
        std::printf(",\"original_serialize_hr\":\"0x%08lx\"", static_cast<unsigned long>(hr));
        if (source->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
            auto copy = *source;
            const unsigned flags = static_cast<unsigned>(copy.Desc_1_1.Flags);
            std::printf(",\"flags\":%u,\"parameters\":%u,\"samplers\":%u",
                        flags, copy.Desc_1_1.NumParameters, copy.Desc_1_1.NumStaticSamplers);
            constexpr unsigned observed_bits = 0x03000000;
            if ((flags & observed_bits) == observed_bits) {
                copy.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags & ~observed_bits);
                ComPtr<ID3DBlob> control;
                hr = D3D12SerializeVersionedRootSignature(&copy, &control, nullptr);
                std::printf(",\"without_observed_bits_hr\":\"0x%08lx\"", static_cast<unsigned long>(hr));
            }
        }
    }
    std::puts("}");
    return 0;
}
