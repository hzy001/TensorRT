/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cuda.h>
#if CUDA_VERSION >= 10010

#ifndef BERT_COMMON_H
#define BERT_COMMON_H

#include "NvInfer.h"
#include "NvInferRuntimeCommon.h"
#include "common/checkMacrosPlugin.h"
#include "common/plugin.h"
#include "cublas_v2.h"
#include "cuda_fp16.h"

#include <algorithm>
#include <cassert>
#include <cuda_runtime_api.h>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

#define TRT_UNUSED (void)

#define BERT_PRINT_DEBUG_MSG 0

#if BERT_PRINT_DEBUG_MSG
#define BERT_DEBUG_MSG(msg) (gLogVerbose << (msg) << std::endl)
#define BERT_DEBUG_VALUE(key, value) (gLogVerbose << key << value << std::endl)
#else
#define BERT_DEBUG_MSG(msg) TRT_UNUSED(msg)
#define BERT_DEBUG_VALUE(key, value)                                                                                   \
    TRT_UNUSED(key);                                                                                                   \
    TRT_UNUSED(value)
#endif

using half = __half;

constexpr uint32_t BDIM = 1; // batch dimension
constexpr uint32_t SDIM = 0; // seq len dimension
constexpr uint32_t HDIM = 2; // hidden dimension

constexpr int32_t kSM_53 = 53;
constexpr int32_t kSM_70 = 70;
constexpr int32_t kSM_72 = 72;
constexpr int32_t kSM_75 = 75;
constexpr int32_t kSM_80 = 80;
constexpr int32_t kSM_86 = 86;
constexpr int32_t kSM_87 = 87;
constexpr int32_t kSM_89 = 89;
constexpr int32_t kSM_90 = 90;

// For full mask mode, we must produce the compressed mask format expected by the fused attention path. Currently, only
// two sequence lengths are supported. We hard code the sizes here.
// The number of threads per CTA: warps_m * warps_n * warps_k * 32;
constexpr size_t threadsPerCta128 = 2 * 2 * 32;
constexpr size_t threadsPerCta384 = 1 * 8 * 32;

// The number of xmmas in the M dimension. We use one uint32_t per XMMA in the M dimension: (s + 16*warps_m - 1)
// / (16*warps_m);
constexpr size_t xmmasM128 = 4;
constexpr size_t xmmasM384 = 24;

// Packed mask size per batch. Layout is XMMAS_M * THREADS_PER_CTA.
constexpr size_t unfusedMaskSize = 1;
constexpr size_t packedMaskSize64 = xmmasM128 * threadsPerCta128;
constexpr size_t packedMaskSize96 = xmmasM128 * threadsPerCta128;
constexpr size_t packedMaskSize128 = xmmasM128 * threadsPerCta128;
constexpr size_t packedMaskSize384 = xmmasM384 * threadsPerCta384;

namespace nvinfer1
{
namespace plugin
{
namespace bert
{

inline int getSMVersion()
{
    int device{-1};
    PLUGIN_CHECK(cudaGetDevice(&device));
    cudaDeviceProp props;
    PLUGIN_CHECK(cudaGetDeviceProperties(&props, device));
    return nvinfer1::plugin::getTrtSMVersionDec(props.major, props.minor);
}

inline int getMHAMaskPackedSize(int smVersion, nvinfer1::DataType dataType, int sequenceLength)
{
    // this code must match EmbLayerNormPluginDynamic::getOutputDimensions in embLayerNormPlugin.cpp
    int packedSize = unfusedMaskSize;
    bool isSmOK = (smVersion == kSM_75 || smVersion == kSM_80 || smVersion == kSM_86 || smVersion == kSM_87
        || smVersion == kSM_90);
    bool isPrecisionOK = (dataType == nvinfer1::DataType::kINT8 || dataType == nvinfer1::DataType::kHALF);
    if (isSmOK && isPrecisionOK)
    {
        if (sequenceLength == 64)
        {
            packedSize = packedMaskSize64;
        }
        else if (sequenceLength == 96)
        {
            packedSize = packedMaskSize96;
        }
        else if (sequenceLength == 128)
        {
            packedSize = packedMaskSize128;
        }
        else if (sequenceLength == 384)
        {
            packedSize = packedMaskSize384;
        }
    }
    return packedSize;
}

inline uint32_t getElementSize(nvinfer1::DataType t) noexcept
{
    switch (t)
    {
    case nvinfer1::DataType::kINT32: return 4;
    case nvinfer1::DataType::kFLOAT: return 4;
    case nvinfer1::DataType::kHALF: return 2;
    case nvinfer1::DataType::kBOOL:
    case nvinfer1::DataType::kUINT8:
    case nvinfer1::DataType::kINT8:
    case nvinfer1::DataType::kFP8: return 1;
    }
    return 0;
}

inline int64_t getWeightsSize(nvinfer1::Weights const& w, nvinfer1::DataType type)
{
    return w.count * getElementSize(type);
}

inline int64_t volume(nvinfer1::Dims const& d)
{
    return std::accumulate(d.d, d.d + d.nbDims, int64_t{1}, std::multiplies<int64_t>{});
}

template <typename IntType>
constexpr IntType ceildiv(IntType a, IntType b)
{
    return (a + b - 1) / b;
}
template <typename IntType>
constexpr IntType alignTo(IntType a, IntType b)
{
    return ceildiv(a, b) * b;
}

template <typename T>
inline T* deserToDev(char const*& buffer, size_t nbElem)
{
    void* dev{nullptr};
    const size_t len = sizeof(T) * nbElem;
    PLUGIN_CUASSERT(cudaMalloc(&dev, len));
    PLUGIN_CUASSERT(cudaMemcpy(dev, buffer, len, cudaMemcpyHostToDevice));

    buffer += len;
    return static_cast<T*>(dev);
}

template <typename T>
inline void serFromDev(char*& buffer, T const* data, size_t nbElem)
{
    const size_t len = sizeof(T) * nbElem;
    PLUGIN_CUASSERT(cudaMemcpy(buffer, static_cast<void const*>(data), len, cudaMemcpyDeviceToHost));
    buffer += len;
}

template <typename T>
inline T* devToDev(T const* data, size_t nbElem)
{
    void* dev{nullptr};
    const size_t len = sizeof(T) * nbElem;
    PLUGIN_CUASSERT(cudaMalloc(&dev, len));
    PLUGIN_CUASSERT(cudaMemcpy(dev, static_cast<void const*>(data), len, cudaMemcpyDeviceToDevice));
    return static_cast<T*>(dev);
}

template <typename T>
cublasStatus_t inline cublasGemm(cublasHandle_t handle, cublasOperation_t transa, cublasOperation_t transb, int m,
    int n, int k, const T alpha, T const* A, int lda, T const* B, int ldb, const T beta, T* C, int ldc);

template <>
cublasStatus_t inline cublasGemm(cublasHandle_t handle, cublasOperation_t transa, cublasOperation_t transb, int m,
    int n, int k, float const alpha, float const* A, int lda, float const* B, int ldb, float const beta, float* C,
    int ldc)
{

    return cublasSgemm(handle, transa, transb, m, n, k, &alpha, A, lda, B, ldb, &beta, C, ldc);
}

template <>
cublasStatus_t inline cublasGemm(cublasHandle_t handle, cublasOperation_t transa, cublasOperation_t transb, int m,
    int n, int k, const half alpha, half const* A, int lda, half const* B, int ldb, const half beta, half* C, int ldc)
{
    return cublasHgemm(handle, transa, transb, m, n, k, &alpha, A, lda, B, ldb, &beta, C, ldc);
}

template <typename T>
cublasStatus_t inline cublasGemmStridedBatchedEx(cublasHandle_t handle, cublasOperation_t transa,
    cublasOperation_t transb, int m, int n, int k, const T alpha, T const* A, int lda, long long int strideA,
    T const* B, int ldb, long long int strideB, const T beta, T* C, int ldc, long long int strideC, int batchCount,
    cublasGemmAlgo_t algo);

template <>
cublasStatus_t inline cublasGemmStridedBatchedEx(cublasHandle_t handle, cublasOperation_t transa,
    cublasOperation_t transb, int m, int n, int k, float const alpha, float const* A, int lda, long long int strideA,
    float const* B, int ldb, long long int strideB, float const beta, float* C, int ldc, long long int strideC,
    int batchCount, cublasGemmAlgo_t algo)
{

    return ::cublasGemmStridedBatchedEx(handle, transa, transb, m, n, k, &alpha, A, CUDA_R_32F, lda, strideA, B,
        CUDA_R_32F, ldb, strideB, &beta, C, CUDA_R_32F, ldc, strideC, batchCount, CUDA_R_32F, algo);
}

template <>
cublasStatus_t inline cublasGemmStridedBatchedEx(cublasHandle_t handle, cublasOperation_t transa,
    cublasOperation_t transb, int m, int n, int k, const half alpha, half const* A, int lda, long long int strideA,
    half const* B, int ldb, long long int strideB, const half beta, half* C, int ldc, long long int strideC,
    int batchCount, cublasGemmAlgo_t algo)
{
    return ::cublasGemmStridedBatchedEx(handle, transa, transb, m, n, k, &alpha, A, CUDA_R_16F, lda, strideA, B,
        CUDA_R_16F, ldb, strideB, &beta, C, CUDA_R_16F, ldc, strideC, batchCount, CUDA_R_16F, algo);
}

template <typename T>
cublasStatus_t inline cublasGemmStridedBatched(cublasHandle_t handle, cublasOperation_t transa,
    cublasOperation_t transb, int m, int n, int k, const T alpha, T const* A, int lda, long long int strideA,
    T const* B, int ldb, long long int strideB, const T beta, T* C, int ldc, long long int strideC, int batchCount);

template <>
cublasStatus_t inline cublasGemmStridedBatched(cublasHandle_t handle, cublasOperation_t transa,
    cublasOperation_t transb, int m, int n, int k, float const alpha, float const* A, int lda, long long int strideA,
    float const* B, int ldb, long long int strideB, float const beta, float* C, int ldc, long long int strideC,
    int batchCount)
{

    return cublasSgemmStridedBatched(
        handle, transa, transb, m, n, k, &alpha, A, lda, strideA, B, ldb, strideB, &beta, C, ldc, strideC, batchCount);
}

template <>
cublasStatus_t inline cublasGemmStridedBatched(cublasHandle_t handle, cublasOperation_t transa,
    cublasOperation_t transb, int m, int n, int k, const half alpha, half const* A, int lda, long long int strideA,
    half const* B, int ldb, long long int strideB, const half beta, half* C, int ldc, long long int strideC,
    int batchCount)
{
    return cublasHgemmStridedBatched(
        handle, transa, transb, m, n, k, &alpha, A, lda, strideA, B, ldb, strideB, &beta, C, ldc, strideC, batchCount);
}

struct CublasConfigHelper
{
    cublasPointerMode_t pm;
    cublasMath_t mm;
    cublasHandle_t cublas;
    CublasConfigHelper(cublasHandle_t cublas_)
        : cublas(cublas_)
    {
        PLUGIN_CUBLASASSERT(cublasGetPointerMode(cublas, &pm));
        PLUGIN_CUBLASASSERT(cublasGetMathMode(cublas, &mm));
        PLUGIN_CUBLASASSERT(cublasSetPointerMode(cublas, CUBLAS_POINTER_MODE_HOST));
        PLUGIN_CUBLASASSERT(cublasSetMathMode(cublas, CUBLAS_TENSOR_OP_MATH));
    }
    ~CublasConfigHelper()
    {
        cublasSetMathMode(cublas, mm);
        cublasSetPointerMode(cublas, pm);
    }
};

template <typename T>
struct CudaDeleter
{
    void operator()(T* buf)
    {
        PLUGIN_CUASSERT(cudaFree(buf));
    }
};

template <typename T>
using cuda_unique_ptr = std::unique_ptr<T, bert::CudaDeleter<T>>;

template <typename T>
using cuda_shared_ptr = std::shared_ptr<T>;

template <typename T>
void make_cuda_shared(cuda_shared_ptr<T>& ptr, void* cudaMem)
{
    ptr.reset(static_cast<T*>(cudaMem), bert::CudaDeleter<T>());
}

struct WeightsWithOwnership : public nvinfer1::Weights
{
    WeightsWithOwnership()
    {
        values = nullptr;
        count = 0;
    }
    ~WeightsWithOwnership()
    {
        operator delete[](const_cast<void*>(values));
    }

    WeightsWithOwnership(WeightsWithOwnership const&) = delete;
    WeightsWithOwnership operator=(WeightsWithOwnership const&) = delete;
    WeightsWithOwnership(WeightsWithOwnership const&&) = delete;
    WeightsWithOwnership operator=(WeightsWithOwnership const&&) = delete;

    void convertAndCopy(nvinfer1::Weights const& src, nvinfer1::DataType type)
    {
        this->type = type;
        this->count = src.count;

        if (type == nvinfer1::DataType::kFLOAT)
        {
            auto destBuf = new float[src.count];
            this->values = destBuf;

            if (src.type == nvinfer1::DataType::kFLOAT)
            {
                BERT_DEBUG_MSG("Float Weights(Host) => Float Array(Host)");
                std::copy_n(static_cast<float const*>(src.values), src.count, destBuf);
            }
            else
            {
                PLUGIN_ASSERT(src.type == nvinfer1::DataType::kHALF);

                BERT_DEBUG_MSG("Half Weights(Host) => Float Array(Host)");
                auto const s = static_cast<half const*>(src.values);
                auto d = static_cast<float*>(const_cast<void*>(this->values));

                for (auto it = 0; it < src.count; it++)
                {
                    d[it] = __half2float(s[it]);
                }
            }
        }
        else if (type == nvinfer1::DataType::kHALF)
        {
            auto destBuf = new half[src.count];
            this->values = destBuf;

            if (src.type == nvinfer1::DataType::kHALF)
            {
                BERT_DEBUG_MSG("Half Weights(Host) => Half Array(Host)");
                std::copy_n(static_cast<half const*>(src.values), src.count, destBuf);
            }
            else
            {
                PLUGIN_ASSERT(src.type == nvinfer1::DataType::kFLOAT);

                BERT_DEBUG_MSG("Float Weights(Host) => Half Array(Host)");
                auto const s = static_cast<float const*>(src.values);
                auto d = static_cast<half*>(const_cast<void*>(this->values));

                for (auto it = 0; it < src.count; it++)
                {
                    d[it] = __float2half(s[it]);
                }
            }
        }
        else
        {
            throw std::runtime_error("Unsupported DataType specified for plugin.");
        }
    }

    void convertAndCopy(char const*& srcBuf, size_t count, nvinfer1::DataType type) noexcept
    {
        this->type = type;
        this->count = count;
        auto const nbBytes = getWeightsSize(*this, type);
        auto destBuf = new char[nbBytes];
        this->values = destBuf;

        std::copy_n(srcBuf, nbBytes, destBuf);
        srcBuf += nbBytes;
    }
};

template <typename T>
inline void copyToDevice(WeightsWithOwnership& hostWeights, size_t nbBytes, cuda_unique_ptr<T>& cudaWeights)
{
    if (hostWeights.values)
    {
        void* cudaMem{nullptr};
        PLUGIN_CUASSERT(cudaMalloc(&cudaMem, nbBytes));
        PLUGIN_CUASSERT(cudaMemcpy(cudaMem, hostWeights.values, nbBytes, cudaMemcpyHostToDevice));
        cudaWeights.reset(static_cast<T*>(cudaMem));
    }
}

inline void convertAndCopyToDevice(nvinfer1::Weights const& src, float* destDev)
{

    size_t wordSize = sizeof(float);
    size_t nbBytes = src.count * wordSize;
    if (src.type == nvinfer1::DataType::kFLOAT)
    {
        BERT_DEBUG_MSG("Float Weights(Host) => Float Array(Device)");
        PLUGIN_CUASSERT(cudaMemcpy(destDev, src.values, nbBytes, cudaMemcpyHostToDevice));
    }
    else
    {
        BERT_DEBUG_MSG("Half Weights(Host) => Float Array(Device)");
        std::vector<float> tmp(src.count);
        half const* values = reinterpret_cast<half const*>(src.values);

        for (size_t it = 0; it < tmp.size(); it++)
        {
            tmp[it] = __half2float(values[it]);
        }

        PLUGIN_CUASSERT(cudaMemcpy(destDev, &tmp[0], nbBytes, cudaMemcpyHostToDevice));
    }
}

inline void convertAndCopyToDevice(nvinfer1::Weights const& src, half* destDev)
{
    size_t wordSize = sizeof(half);
    size_t nbBytes = src.count * wordSize;
    if (src.type == nvinfer1::DataType::kHALF)
    {
        BERT_DEBUG_MSG("Half Weights(Host) => Half Array(Device)");
        PLUGIN_CUASSERT(cudaMemcpy(destDev, src.values, nbBytes, cudaMemcpyHostToDevice));
    }
    else
    {
        BERT_DEBUG_MSG("Float Weights(Host) => Half Array(Device)");
        std::vector<half> tmp(src.count);
        float const* values = reinterpret_cast<float const*>(src.values);

        for (size_t it = 0; it < tmp.size(); it++)
        {
            tmp[it] = __float2half(values[it]);
        }
        PLUGIN_CUASSERT(cudaMemcpy(destDev, &tmp[0], nbBytes, cudaMemcpyHostToDevice));
    }
}

inline nvinfer1::DataType fieldTypeToDataType(const nvinfer1::PluginFieldType ftype)
{
    switch (ftype)
    {
    case nvinfer1::PluginFieldType::kFLOAT32:
    {
        BERT_DEBUG_MSG("PluginFieldType is Float32");
        return nvinfer1::DataType::kFLOAT;
    }
    case nvinfer1::PluginFieldType::kFLOAT16:
    {
        BERT_DEBUG_MSG("PluginFieldType is Float16");
        return nvinfer1::DataType::kHALF;
    }
    case nvinfer1::PluginFieldType::kINT32:
    {
        BERT_DEBUG_MSG("PluginFieldType is Int32");
        return nvinfer1::DataType::kINT32;
    }
    case nvinfer1::PluginFieldType::kINT8:
    {
        BERT_DEBUG_MSG("PluginFieldType is Int8");
        return nvinfer1::DataType::kINT8;
    }
    default: throw std::invalid_argument("No corresponding datatype for plugin field type");
    }
}

} // namespace bert
} // namespace plugin
} // namespace nvinfer1
#endif // BERT_COMMON_H

#endif // CUDA_VERSION >= 10010
