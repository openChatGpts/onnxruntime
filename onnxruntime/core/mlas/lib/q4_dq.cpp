/*++

Copyright (c) Microsoft Corporation. All rights reserved.

Licensed under the MIT License.

Module Name:

    q4_dq.cpp

Abstract:

    This module contains the data structures and implementations
    for blocked int4 quantization and dequantization.

    Int4 block quantization is used to compress weight tensors of large
    language models.

--*/

#include "q4common.h"

template<typename T>
constexpr
size_t
BlkQ4BufSize(size_t N, size_t K)
{
    const size_t KBlocks = MlasDivRoundup(K, T::BlkLen);
    return N * KBlocks * T::BlobSize;
}

size_t
MLASCALL
MlasQ4GemmPackBSize(MLAS_BLK_QUANT_TYPE QType, size_t N, size_t K)
{
    if (GetMlasPlatform().FpQ4GemmDispatch == nullptr) {
        return 0;
    }

    switch (QType) {
        case BlkQ4Sym:
            return BlkQ4BufSize<MLAS_Q4TYPE_BLK0>(N, K);
        case BlkQ4Sym64:
            return BlkQ4BufSize<MLAS_Q4TYPE_BLK2>(N, K);
        case BlkQ4Sym128:
            return BlkQ4BufSize<MLAS_Q4TYPE_BLK4>(N, K);
        default:
            return BlkQ4BufSize<MLAS_Q4TYPE_BLK1>(N, K);
    }
}


template<typename T>
MLAS_FORCEINLINE
void
MlasQ4GemmPackBImpl(void* PackedBuf, const float* FpData, size_t N, size_t K, size_t ldb)
{
    auto* dst_ptr = reinterpret_cast<uint8_t*>(PackedBuf);

    for (size_t n = 0; n < N; n ++) {
        const float* src = FpData; // starting from top of the column

        for (size_t k = 0; k < K; k += T::BlkLen) {
            size_t klen = std::min(size_t(T::BlkLen), K - k);
            float amax = 0.0f; // abs(max)
            float max = 0.0f;

            for (size_t l = 0; l < klen; l++) {
                const float v = src[ldb * l];
                if (amax < fabsf(v)) {
                    amax = fabsf(v);
                    max = v;
                }
            }

            const float scale = max / (-8);
            const float reciprocal_scale = scale ? 1.0f / scale : 0.0f;
            MlasQ4BlkScale<T>(dst_ptr) = scale;
            uint8_t* data = MlasQ4BlkData<T>(dst_ptr);

            for (size_t kk = 0; kk < klen; kk += 32) {
                size_t kklen = std::min((size_t)32, klen - kk);
                for (size_t l = 0; l < 16; l++) {
                    const float v0 = l < kklen ? src[ldb * (kk + l)] * reciprocal_scale : 0;
                    const uint8_t vi0 = (uint8_t)std::min(15.0f, std::max(0.0f, v0 + 8.5f));

                    const size_t l1 = l + 16;
                    const float v1 = (l1 < kklen) ? src[ldb * (kk + l1)] * reciprocal_scale : 0;
                    const uint8_t vi1 = (uint8_t)std::min(15.0f, std::max(0.0f, v1 + 8.5f));

                    data[l] = vi0 | (vi1 << 4);
                }
                data += 16;
            }

            // Move to next block of values in this column
            dst_ptr += T::BlobSize;
            src += ldb * klen;
        }

        FpData++; // move to next column
    }
}

template<>
MLAS_FORCEINLINE
void
MlasQ4GemmPackBImpl<MLAS_Q4TYPE_BLK1>(
    void* PackedBuf, const float* FpData, size_t N, size_t K, size_t ldb)
{
    auto* dst_ptr = reinterpret_cast<uint8_t*>(PackedBuf);

    for (size_t n = 0; n < N; n++) {
        const float* src = FpData; // starting from top of the column

        for (size_t k = 0; k < K; k += MLAS_Q4TYPE_BLK1::BlkLen) {
            size_t klen = std::min(MLAS_Q4TYPE_BLK1::BlkLen, K - k);
            float min = std::numeric_limits<float>::max();
            float max = -min;

            for (size_t l = 0; l < klen; l++) {
                const float v = src[ldb * l];
                if (v < min) min = v;
                if (v > max) max = v;
            }
            min = std::min(min, 0.0f);
            max = std::max(max, 0.0f);

            const float scale = (max - min) / ((1 << 4) - 1);
            const float reciprocal_scale = scale ? 1.0f / scale : 0.0f;
            float zero_point_fp = min;
            if (scale != 0.0f) {
                zero_point_fp = 0.f - min / scale;
            }

            // Handle any clamping
            uint8_t& zp = MlasQ4BlkZeroPoint<MLAS_Q4TYPE_BLK1>(dst_ptr);
            if (zero_point_fp < 0.0f) {
                zp = 0;
            } else if (zero_point_fp > 15.0f) {
                zp = 15;
            } else {
                zp = (uint8_t)roundf(zero_point_fp);
            }
            MlasQ4BlkScale<MLAS_Q4TYPE_BLK1>(dst_ptr) = scale;
            uint8_t* data = MlasQ4BlkData<MLAS_Q4TYPE_BLK1>(dst_ptr);

            for (size_t kk = 0; kk < klen; kk += 32) {
                size_t kklen = std::min((size_t)32, klen - kk);
                for (size_t l = 0; l < 16; l++) {
                    const float v0 = l < kklen ? src[ldb * (kk + l)] : 0;
                    const uint8_t vi0 = (uint8_t)std::min(
                        15.0f, std::max(0.0f, roundf(v0 * reciprocal_scale + zp)));

                    const size_t l1 = l + 16;
                    const float v1 = (l1 < kklen) ? src[ldb * (kk + l1)] : 0;
                    const uint8_t vi1 = (uint8_t)std::min(
                        15.0f, std::max(0.0f, roundf(v1 * reciprocal_scale + zp)));

                    data[l] = vi0 | (vi1 << 4);
                }
                data += 16;
            }
            // move to next block of values in this column
            dst_ptr += MLAS_Q4TYPE_BLK1::BlobSize;
            src += ldb * klen;
        }
        FpData++; // move to next column
    }
}

void
MLASCALL
MlasQ4GemmPackB(
    MLAS_BLK_QUANT_TYPE QType,
    void* PackedBuf,
    const float* FpData,
    size_t N,
    size_t K,
    size_t ldb
    )
{
    switch (QType) {
        case BlkQ4Sym:
            return MlasQ4GemmPackBImpl<MLAS_Q4TYPE_BLK0>(PackedBuf, FpData, N, K, ldb);
        case BlkQ4Sym64:
            return MlasQ4GemmPackBImpl<MLAS_Q4TYPE_BLK2>(PackedBuf, FpData, N, K, ldb);
        case BlkQ4Sym128:
            return MlasQ4GemmPackBImpl<MLAS_Q4TYPE_BLK4>(PackedBuf, FpData, N, K, ldb);
        default:
            return MlasQ4GemmPackBImpl<MLAS_Q4TYPE_BLK1>(PackedBuf, FpData, N, K, ldb);
    }
}

template<typename T>
MLAS_FORCEINLINE
void
MlasQ4GemmUnPackBImpl(float* FpData, const void* PackedBuf, size_t N, size_t K, size_t ldb)
{
    const auto* src = reinterpret_cast<const uint8_t*>(PackedBuf);
    for (size_t n = 0; n < N; n++) {
        for (size_t k = 0; k < K; k += T::BlkLen) {
            size_t CountK = std::min(K - k, T::BlkLen);

            float* dest = FpData + ldb * k + n;
            const float scale = MlasQ4BlkScale<T>(src);
            const uint8_t* data = MlasQ4BlkData<T>(src);

            for (size_t kk = 0; kk < CountK; kk += 32) {
                size_t kklen = std::min((size_t)32, CountK - kk);
                for (size_t l = 0; l < 16; l++) {
                    const uint8_t vi = data[l];

                    if (l < kklen) {
                        const int vi0 = (vi & 0x0F) - 8;
                        const float v0 = vi0 * scale;
                        dest[ldb * (kk + l)] = v0;
                    }

                    const size_t l1 = l + 16;
                    if (l1 < kklen) {
                        const int vi1 = (vi >> 4) - 8;
                        const float v1 = vi1 * scale;
                        dest[ldb * (kk + l1)] = v1;
                    }
                }
                data += 16;
            }
            src += T::BlobSize;
        }
    }
}

template<>
MLAS_FORCEINLINE
void
MlasQ4GemmUnPackBImpl<MLAS_Q4TYPE_BLK1>(
    float* FpData, const void* PackedBuf, size_t N, size_t K, size_t ldb)
{
    const auto* src = reinterpret_cast<const uint8_t*>(PackedBuf);
    for (size_t n = 0; n < N; n++) {
        for (size_t k = 0; k < K; k += MLAS_Q4TYPE_BLK1::BlkLen) {
            size_t CountK = std::min(K - k, MLAS_Q4TYPE_BLK1::BlkLen);

            float* dest = FpData + ldb * k + n;
            const float s = MlasQ4BlkScale<MLAS_Q4TYPE_BLK1>(src);
            const uint8_t z = MlasQ4BlkZeroPoint<MLAS_Q4TYPE_BLK1>(src);
            const uint8_t* pp = MlasQ4BlkData<MLAS_Q4TYPE_BLK1>(src);

            for (size_t kk = 0; kk < CountK; kk += 32) {
                size_t kklen = std::min((size_t)32, CountK - kk);
                for (size_t l = 0; l < 16; l++) {
                    const uint8_t vi = pp[l];

                    if (l < kklen) {
                        const int8_t vi0 = vi & 0x0F;
                        const float v0 = (vi0 - z) * s;
                        dest[ldb * (kk + l)] = v0;
                    }

                    size_t l1 = l + 16;
                    if (l1 < kklen) {
                        const int8_t vi1 = vi >> 4;
                        const float v1 = (vi1 - z) * s;
                        dest[ldb * (kk + l1)] = v1;
                    }
                }
                pp += 16;
            }
            src += MLAS_Q4TYPE_BLK1::BlobSize;
        }
    }
}

void
MLASCALL
MlasQ4GemmUnPackB(
    MLAS_BLK_QUANT_TYPE QType,
    float* FpData,
    const void* PackedBuf,
    size_t N,
    size_t K,
    size_t ldb
    )
{
    switch (QType) {
        case BlkQ4Sym:
            return MlasQ4GemmUnPackBImpl<MLAS_Q4TYPE_BLK0>(FpData, PackedBuf, N, K, ldb);
        case BlkQ4Sym64:
            return MlasQ4GemmUnPackBImpl<MLAS_Q4TYPE_BLK2>(FpData, PackedBuf, N, K, ldb);
        case BlkQ4Sym128:
            return MlasQ4GemmUnPackBImpl<MLAS_Q4TYPE_BLK4>(FpData, PackedBuf, N, K, ldb);
        default:
            return MlasQ4GemmUnPackBImpl<MLAS_Q4TYPE_BLK1>(FpData, PackedBuf, N, K, ldb);
    }
}



/***************************************************************
 * The quantization format that pack data and quantization
 * parameters into separate buffers.
 */


template <
    int Row_,    ///< rows of a matrix
    int Column_  ///< columns of a matrix
    >
struct Shape2D {
    static int const kRow = Row_;              ///< rows of a matrix
    static int const kColumn = Column_;        ///< columns of a matrix
    static int const kCount = Row_ * Column_;  ///< total number of elements in a matrix
};


template <int qbits>
struct BitsTraits {
    static_assert(qbits <= 8, "Only BitsTraits are for small number of bits!");

    static constexpr int kBits = qbits;
    static constexpr int kMax = (1 << qbits) - 1;
    static constexpr int kMid = 1 << (qbits - 1);
    static constexpr float kMaxFp = static_cast<float>(kMax);

    // number of qbit elements to pack into whole bytes
    static constexpr int kPackSize = (qbits == 8) ? 1 : (qbits == 4) ? 2 : (qbits == 2) ? 4 : 0;
    static_assert(kPackSize != 0, "Packing to whole bytes not supported for this qbits!");
};


/**
 * @brief Rectify min/max from a set of weights, and convert to scale and zero point
 *        for quantization
 * @tparam ScaleT   type of scale, usually floating point of various bits
 * @tparam qbits  number of int bits used for zero point value
 * @param[in]   min
 * @param[in]   max
 * @param[out]  scale
 * @param[out]  zp
 */
template <typename ScaleT, int qbits>
MLAS_FORCEINLINE
void
range2scalezp(float min, float max, ScaleT& scale, uint8_t& zp)
{
    constexpr int zp_max = BitsTraits<qbits>::kMax;
    constexpr float zp_max_fp = BitsTraits<qbits>::kMaxFp;

    min = std::min(min, 0.0f);
    max = std::max(max, 0.0f);

    float scale_f = (max - min) / zp_max;

    float zero_point_fp = min;
    if (scale_f != 0.0f) {
        zero_point_fp = 0.f - min / scale_f;
    }

    if (zero_point_fp < 0.0f) {
        zp = 0;
    } else if (zero_point_fp > zp_max_fp) {
        zp = zp_max;
    } else {
        zp = (uint8_t)roundf(zero_point_fp);
    }
    scale = ScaleT(scale_f);
}

template <typename ScaleT, int qbits>
MLAS_FORCEINLINE
void
range2scale(float min, float max, ScaleT& scale)
{
    constexpr int mid_v = BitsTraits<qbits>::kMid;
    constexpr float mid_fp = static_cast<float>(-mid_v);

    max = fabsf(max) > fabsf(min) ? max : min;

    scale = ScaleT(max / mid_fp);
};


/**
 * @brief Blockwise quantization methods
 * @tparam ElementT       source data type, e.g. fp32/fp16
 * @tparam block_size     number of elemenets quantized together
 * @tparam qbits          number of bits in each quantized element
 * @tparam Columnwise     true:  elements in a block come from one single column
 *                        false: elements in a block come from one single row
 */
template <
    typename ElementT,
    int32_t block_size,
    int32_t qbits,
    bool Columnwise>
struct BlockwiseQuantizer {
    // To support other qbits, need to add bit packing code for
    // storing to dst and zero points
    static_assert(qbits == 4, "Only 4b block quantization is supported!");

    using QuantBlk = std::conditional_t<Columnwise, Shape2D<block_size, 1>, Shape2D<1, block_size>>;
    using ThreadBlk = Shape2D<QuantBlk::kRow * BitsTraits<qbits>::kPackSize, QuantBlk::kColumn>;

    static
    MLAS_FORCEINLINE
    void quantizeMetaShape(int rows, int columns, int& meta_rows, int& meta_cols)
    {
        meta_rows = (rows + QuantBlk::kRow - 1) / QuantBlk::kRow;
        meta_cols = (columns + QuantBlk::kColumn - 1) / QuantBlk::kColumn;
    }

    static
    MLAS_FORCEINLINE
    void quantizedShape(int rows, int columns, int& q_rows, int& q_cols) {
        int meta_rows;
        int meta_cols;
        quantizeMetaShape(rows, columns, meta_rows, meta_cols);

        // quantized matrix is stored in column major, packed by column
        q_rows = (meta_rows * QuantBlk::kRow * qbits + 7) / 8;
        q_cols = meta_cols * QuantBlk::kColumn;
    }

    static MLAS_FORCEINLINE void quantizedBufferSizes(
        int rows, int columns, size_t& data_bytes, size_t& scale_num_elements, size_t* zero_point_bytes
    )
    {
        int meta_rows, meta_cols;
        quantizeMetaShape(rows, columns, meta_rows, meta_cols);
        int q_rows, q_cols;
        quantizedShape(rows, columns, q_rows, q_cols);

        data_bytes = q_rows * q_cols;
        scale_num_elements = meta_rows * meta_cols;

        if (zero_point_bytes) {
            // this works for qbits == 4 but may need to be updated for other qbits values
            *zero_point_bytes = ((meta_rows * qbits + 7) / 8) * meta_cols;
        }
    }

    /**
     * @brief Quantized a Matrix shape [rows, columns], resulting quantized
     *        and packed data are stored in column major (transposed)
     * @param[out] dst           pointer to the quantized weights, column major: [columns, rows]
     * @param[out] scale         pointer to the scales, column major: [columns/QuantBlk::kColumn, rows/QuantBlk::kRow]
     * @param[out] zero_points   pointer to the zero points, same shape as scale
     * @param[in]  src           pointer to the source matrix, row major: [rows, columns]
     * @param rows
     * @param columns
     * @param leadingDimension   stride of the source matrix, i.e. distance from one row to the next
     */
    static void quantizeAndTranspose(
        uint8_t* dst,
        ElementT* scales,
        uint8_t* zero_points,
        const ElementT* src,
        int32_t rows,
        int32_t columns,
        int32_t leadingDimension,
        MLAS_THREADPOOL* thread_pool)
    {
        // Thread partitioning
        const auto thrd_row_blks = (rows + ThreadBlk::kRow - 1) / ThreadBlk::kRow;
        const auto thrd_col_blks = (columns + ThreadBlk::kColumn - 1) / ThreadBlk::kColumn;
        const auto total_thrd_blks = thrd_row_blks * thrd_col_blks;

        const auto row_blks = (rows + QuantBlk::kRow - 1) / QuantBlk::kRow;

        int q_rows, q_cols;
        quantizedShape(rows, columns, q_rows, q_cols);

        MlasTryBatchParallel(
            thread_pool, total_thrd_blks,
            [&](ptrdiff_t block_idx) {
                uint8_t zp_bytes[BitsTraits<qbits>::kPackSize];
                std::fill_n(zp_bytes, BitsTraits<qbits>::kPackSize, (uint8_t)8);

                const int32_t r_blk_idx = static_cast<int32_t>(block_idx / thrd_col_blks);
                const int32_t c_blk_idx = static_cast<int32_t>(block_idx % thrd_col_blks);

                const int32_t r = r_blk_idx * ThreadBlk::kRow;
                const int32_t c = c_blk_idx * ThreadBlk::kColumn;

                const int32_t r_end = std::min(r + ThreadBlk::kRow, rows);
                const int32_t c_end = std::min(c + ThreadBlk::kColumn, columns);

                const int meta_row = r / QuantBlk::kRow;
                const int meta_col = c / QuantBlk::kColumn;

                // compute scale and zero point
                for (int kpack = 0; kpack < BitsTraits<qbits>::kPackSize; kpack++) {

                    // scan a single block to extract range [min, max]
                    float min = std::numeric_limits<float>::max();
                    float max = -min;
                    const int row_start = r + kpack * QuantBlk::kRow;
                    const int row_end = std::min(row_start + QuantBlk::kRow, r_end);
                    for (int i = row_start; i < row_end; ++i) {
                        for (int j = c; j < c_end; ++j) {
                            const float v = static_cast<float>(src[i * leadingDimension + j]);
                            if (v < min) min = v;
                            if (v > max) max = v;
                        }
                    }

                    // store scale and zero point at quant parameter matrix position
                    if (row_start < row_end) {
                        const int32_t meta_idx = meta_col * row_blks + meta_row + kpack;
                        if (zero_points == nullptr) {
                            range2scale<ElementT, qbits>(min, max, scales[meta_idx]);
                        } else {
                            range2scalezp<ElementT, qbits>(min, max, scales[meta_idx], zp_bytes[kpack]);
                        }
                    }
                }

                // !! 4b specific code as we need to pack 2 4b numbers into one byte
                if (zero_points != nullptr) {
                    const int32_t meta_idx = meta_col * ((row_blks + 1) / 2) + meta_row / 2;
                    zero_points[meta_idx] = (zp_bytes[0] & 0xf) | (zp_bytes[1] << 4);
                }

                for (int32_t j = c; j < c_end; ++j) {
                    const int32_t meta_c = j / QuantBlk::kColumn;
                    for (int32_t i = r; i < r_end; i += 2) {
                        const int32_t meta_r = i / QuantBlk::kRow;
                        const float scale = static_cast<float>(scales[meta_c * row_blks + meta_r]);
                        const float reciprocal_scale = scale ? 1.0f / scale : 0.0f;
                        const int8_t zp = zp_bytes[meta_r & 1];
                        const int8_t zp1 = zp_bytes[((i + 1) / QuantBlk::kRow) & 1];

                        const float v0 = static_cast<float>(src[i * leadingDimension + j]);
                        const uint8_t vi0 = (uint8_t)std::clamp(roundf(v0 * reciprocal_scale + zp),
                                                                0.0f, BitsTraits<qbits>::kMaxFp);

                        uint8_t vi1 = (uint8_t)zp;
                        if (i + 1 < r_end) {
                            float reciprocal_scale1 = reciprocal_scale;
                            if constexpr (QuantBlk::kRow == 1) {
                                const float scale1 =
                                    static_cast<float>(scales[meta_c * row_blks + meta_r + 1]);
                                reciprocal_scale1 = scale1 ? 1.0f / scale1 : 0.0f;
                            }
                            const float v1 = static_cast<float>(src[(i + 1) * leadingDimension + j]);
                            vi1 = (uint8_t)std::clamp(roundf(v1 * reciprocal_scale1 + zp1), 0.0f,
                                                      BitsTraits<qbits>::kMaxFp);
                        }

                        // !! 4b specific code
                        dst[j * q_rows + i / 2] = (vi0 & 0xf) | (vi1 << 4);
                    }
                }
            });
    }

    /**
     * @brief Dequantize a column major quantized matrix, and store the result in a column major
     * matrix for use in GEMM
     * @param[out] dst           pointer to the dequantized matrix, column major: [columns, rows]
     * @param[in]  weights       pointer to the quantized weights, column major: [columns, rows]
     * @param[in]  scales        pointer to the scales of quantized blocks, column major layout
     * @param[in]  zero_points   pointer to the zero points of quantized blocks, packed column major
     *                           scales
     * @param[in]  rows
     * @param[in]  columns
     */
    static void dequantize(
        ElementT* dst,
        const uint8_t* weights,
        const ElementT* scales,
        const uint8_t* zero_points,
        int32_t rows,
        int32_t columns,
        MLAS_THREADPOOL* thread_pool)
    {
        // Thread partitioning
        const auto thrd_row_blks = (rows + ThreadBlk::kRow - 1) / ThreadBlk::kRow;
        const auto thrd_col_blks = (columns + ThreadBlk::kColumn - 1) / ThreadBlk::kColumn;
        const auto total_thrd_blks = thrd_row_blks * thrd_col_blks;

        const auto row_blks = (rows + QuantBlk::kRow - 1) / QuantBlk::kRow;

        int q_rows, q_cols;
        quantizedShape(rows, columns, q_rows, q_cols);

        MlasTryBatchParallel(
            thread_pool, total_thrd_blks,
            [&](ptrdiff_t block_idx) {
                int32_t r_blk_idx = static_cast<int32_t>(block_idx / thrd_col_blks);
                int32_t c_blk_idx = static_cast<int32_t>(block_idx % thrd_col_blks);

                int32_t r = r_blk_idx * ThreadBlk::kRow;
                int32_t c = c_blk_idx * ThreadBlk::kColumn;

                int32_t r_end = std::min(r + ThreadBlk::kRow, rows);
                int32_t c_end = std::min(c + ThreadBlk::kColumn, columns);

                for (int32_t j = c; j < c_end; ++j) {
                    const int32_t meta_col = j / QuantBlk::kColumn;

                    // !! 4b specific code
                    // the whole loop is 4b specific due to sub 8 bit packing
                    // and unpacking. We can potentially make this qbits generic
                    // by wraping the packing/unpacking code like cutlass::Array
                    for (int32_t i = r; i < r_end; i += 2) {
                        const int32_t meta_row = i / QuantBlk::kRow;

                        const float scale0 =
                            static_cast<float>(scales[meta_col * row_blks + meta_row]);

                        const int zp_pair =
                            (zero_points == nullptr)
                                ? 0x88
                                : zero_points[meta_col * ((row_blks + 1) / 2) + meta_row / 2];
                        const int zp0 = (meta_row & 1) ? (zp_pair >> 4) : (zp_pair & 0xf);

                        const uint8_t vi0 = weights[j * q_rows + i / 2] & 0xf;
                        const float v0 = (static_cast<float>(vi0) - zp0) * scale0;

                        dst[j * rows + i] = static_cast<ElementT>(v0);
                        if ((i + 1) < r_end) {
                            float scale1 = scale0;
                            int zp1 = zp0;
                            if constexpr (QuantBlk::kRow == 1) {
                                scale1 =
                                    static_cast<float>(scales[meta_col * row_blks + meta_row + 1]);
                                zp1 = (zp_pair >> 4) & 0xf;
                            }
                            const uint8_t vi1 = weights[j * q_rows + i / 2] >> 4;
                            const float v1 = (static_cast<float>(vi1) - zp1) * scale1;
                            dst[j * rows + (i + 1)] = static_cast<ElementT>(v1);
                        }
                    }
                }
            });
    }
};

/**
 * @brief Blockwise quantization methods for QDQ format. Input tensor is quantized along column
 *        or row. Scales and zeros are calculated. Based on qbits, consecutive quantized elements
 *        in memory are packed together, which means the packing is along the row. Quantized data
 *        are stored in row major, so the output tensor reserves same shape, in terms of qbits type,
 *        as the input tensor.
 * @tparam Tin    source data type, e.g. fp32/fp16
 * @tparam qbits  number of bits in each quantized element
 */
template <typename Tin, int qbits>
struct BlockwiseQDQQuantizer {
    static_assert(qbits == 4 || qbits == 2, "Only 4bit or 2bit block quantization is supported!");

    const int32_t mask_ = (1 << qbits) - 1;
    const int32_t pack_size_ = BitsTraits<qbits>::kPackSize;
    constexpr int32_t shift_bit_ = qbits == 4 ? 1 : (qbits == 2 ? 2 : 0); 

    static MLAS_FORCEINLINE uint8_t SetElem(uint8_t val, int32_t idx, uint8_t dst)
    {
        auto shift = idx * qbits;
        return ((val & mask_) << shift) | (dst & (~(mask_ << shift)));
    }

    static MLAS_FORCEINLINE uint8_t Pack(uint8_t vals[pack_size])
    {
        return qbits == 4
                   ? ((vals[0] & 0xF) | ((vals[1] & 0xF) << 4))
               : qbits == 2
                   ? ((vals[0] & 3) | ((vals[1] & 3) << 2) | ((vals[2] & 3) << 4) | ((vals[3] & 3) << 6))
                   : vals[0];
    }

    /**
     * @brief Quantize a matrix shape [rows, columns] row-wise. Scales and zero points are calculated.
     *        Quantized data are packed row-wise based on qbits. Quantized data are stored in row
     *        major, so the output tensor reserves the shape, in terms output type.
     *        Thread block is [1, quant_block_size * pack_size_].
     * @param src               the source matrix, row major: [rows * columns]
     * @param scales            the scales of quantized blocks, row major layout with shape:
     *                          [rows * ceil(columns / quant_block_size)]
     * @param zero_points       the zero points of quantized blocks, packed. Same shape as scales
     *                          in terms of output type. In terms of uint8_t, the shape is:
     *                          [ceil(rows * ceil(columns / quant_block_size) * qbits / 8)]
     * @param dst               the quantized weights, row major: [rows * columns] in terms of
     *                          output type. In terms of uint8_t, the shape is: [ceil(rows * columns * qbits / 8]
     * @param rows              number of rows in the source matrix
     * @param columns           number of columns in the source matrix, must satisfy
     *                          ceil(columns / quant_block_size) % pack_size_ == 0, so in each thread block,
     *                          zero points are packed into one byte.
     * @param quant_block_size  number of elements quantized together.
     * @param thread_pool       thread pool for parallel processing
     */
    static void quantizeRowWise(
        const Tin* src,
        Tin* scales,
        uint8_t* zero_points,
        uint8_t* dst,
        int32_t rows,
        int32_t columns,
        int32_t quant_block_size,
        MLAS_THREADPOOL* thread_pool
    )
    {
        ORT_THROW("BlockwiseQDQQuantizer::BlockwiseQDQQuantizer is not implemented");
    }

    /**
     * @brief Quantize a matrix shape [rows, columns] column-wise. Scales and zero points are calculated.
     *        Quantized data are packed row-wise based on qbits. Quantized data are stored in row major
     *        so the output tensor reserves the shape, in terms output type.
     *        Thread block is [quant_block_size, thread_block_size]. thread_block_size % pack_size_ == 0.
     * @param src               the source matrix, row major: [rows * columns]
     * @param scales            the scales of quantized blocks, row major with shape: [ceil(rows/quant_block_size) * columns]
     * @param zero_points       the zero points of quantized blocks, packed. Same shape as scales in terms of output type.
     *                          In uint8_t, the shape is: [ceil(columns * ceil(rows / quant_block_size) * qbits / 8)]
     * @param dst               the quantized weights, row major: [rows * columns] in terms of output type. In uint8_t,
     *                          the shape is: [ceil(rows * columns * qbits / 8]
     * @param rows              number of rows in the source matrix
     * @param columns           number of columns in the source matrix, must be multiple of pack_size.
     * @param quant_block_size  number of rows/columns quantized together
     * @param thread_pool       thread pool for parallel processing
     */
    static void quantizeColumnWise(
        const Tin* src,
        Tin* scales,
        uint8_t* zero_points,
        uint8_t* dst,
        int32_t rows,
        int32_t columns,
        int32_t quant_block_size,
        MLAS_THREADPOOL* thread_pool
    )
    {
        ORT_ENFORCE(columns % pack_size_ == 0, "Columns of ", qbits, " tensor must be multiple of ", pack_size_);
        const int32_t thr_blk_size = 16;
        const auto num_row_thr_blk = (rows + quant_block_size - 1) / quant_block_size;
        const auto num_col_thr_blk = (columns + thr_blk_size - 1) / thr_blk_size;
        const auto num_thr_blk = num_row_thr_blk * num_col_thr_blk;
        const auto minf = std::numeric_limits<float>::lowest();
        const auto maxf = std::numeric_limits<float>::max();

        MlasTryBatchParallel(
            thread_pool, static_cast<ptrdiff_t>(num_thr_blk),
            [&](ptrdiff_t thr_blk_idx) {
                // TODO(fajin): tweak stride to optimize L1 cache usage
                uint8_t zp_t[pack_size_];
                uint8_t out_t[pack_size_];
                float reciprocal_scale_t[pack_size_];
                float zp_f_t[pack_size_];
                float vmin_t[pack_size_];
                float vmax_t[pack_size_];

                const int32_t row_quant_blk_idx = static_cast<int32_t>(thr_blk_idx / num_col_thr_blk);
                const int32_t col_thr_blk_idx = static_cast<int32_t>(thr_blk_idx % num_col_thr_blk);

                const int32_t row_idx = row_quant_blk_idx * quant_block_size;
                const int32_t col_idx = col_thr_blk_idx * thr_blk_size;

                const int32_t row_size = std::min(quant_block_size, rows - row_idx);
                const int32_t col_size = std::min(thr_blk_size, columns - col_idx);

                auto input_idx = row_idx * columns + col_idx;
                auto scale_idx = row_quant_blk_idx * columns + col_idx;
                auto input_end_idx = input_idx + col_size;

                Tin scale_tt;

                // input_idx, scale_idx and input_end_idx must be multiple of pack_size_
                for (input_idx + pack_size_ < input_end_idx; input_idx += pack_size_, scale_idx += pack_size_) {
                    std::fill_n(zp_t, pack_size_, static_cast<uint8_t>(BitsTraits<qbits>::kMid));
                    std::fill_n(vmin_t, pack_size_, maxf);
                    std::fill_n(vmax_t, pack_size_, minf);

                    // calculate min/max of pack_size_ quant blocks
                    for (int32_t j = 0, input_idx_t = input_idx; j < row_size; ++j, input_idx_t += columns) {
                        // TODO(fajin): use SIMD
                        for (int32_t i = 0; i < pack_size_; ++i) {
                            auto v = static_cast<float>(src[input_idx_t + i]);
                            vmin_t[i] = std::min(vmin_t[i], v);
                            vmax_t[i] = std::max(vmax_t[i], v);
                        }
                    }

                    // calculate scale and zero point of pack_size_ quant blocks
                    for (int32_t i = 0; i < pack_size_; ++i) {
                        if (zero_points) {
                            range2scalezp<Tin, qbits>(vmin_t[i], vmax_t[i], scale_tt, zp_t[i]);
                        } else {
                            range2scale<Tin, qbits>(vmin_t[i], vmax_t[i], scale_tt);
                        }

                        scales[scale_idx + i] = scale_tt;

                        float scalef = static_cast<float>(scale_tt);
                        reciprocal_scale_t[i] = scalef ? 1.0f / scalef : 0.0f;
                        zp_f_t[i] = static_cast<float>(zp_t[i]);
                    }

                    if (zero_points) {
                        zero_points[scale_idx >> shift_bit_] = Pack(zp_t);
                    }

                    // quantize and pack
                    for (int32_t j = 0, input_idx_t = input_idx; j < row_size; ++j, input_idx_t += columns) {
                        // TODO(fajin): use SIMD
                        for (int32_t i = 0; i < pack_size_; ++i) {
                            auto v = static_cast<float>(src[input_idx_t + i]);
                            out_t[i] = static_cast<uint8_t>(
                                std::clamp(
                                    std::roundf(v * reciprocal_scale_t[i]) + zp_f_t[i],
                                    0.0f,
                                    BitsTraits<qbits>::kMaxFp
                                )
                            );
                        }

                        dst[input_idx_t >> shift_bit_] = Pack(out_t);
                    }
                }
            }
        );
    }

    /**
     * @brief Transpose a quantized tensor to use in MatMulNbits. The input tensor is in row major,
     *        [rows, columns] in terms of qbits type, packed along rows. The output tensor is in
     *        [columns, ceil(rows / quant_block_size), ceil(block_size * qbits / 8)] in uint8_t type.
     *        Since both input tensor and output tensor are packed, it's not needed to consider sign
     *        during the unpacking/packing in transpose.
     * @param src               the quantized tensor, row major: [rows * columns] in terms of qbits type
     *                          In uint8_t, the shape is: [ceil(rows * columns * qbits / 8]
     * @param dest              the transposed quantized tensor, column major. In uint8_t, the shape is
     *                          [columns * ceil(rows / quant_block_size) * ceil(block_size * qbits / 8)]
     * @param rows              number of rows in the source matrix
     * @param columns           number of columns in the source matrix
     * @param quant_block_size  number of rows/columns quantized together
     * @param thread_pool       thread pool for parallel processing
     */
    static void Transpose(
      const uint8_t* src, 
      uint8_t *dest, 
      int32_t rows, 
      int32_t columns, 
      int32_t quant_block_size,
      MLAS_THREADPOOL* thread_pool)
    {

    }
};

template <typename T, int qbits>
void
MlasBlockwiseQuantMetaShape(
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    int& meta_rows,
    int& meta_cols
    )
{
    switch (block_size) {
        case 16: {
            if (columnwise) {
                BlockwiseQuantizer<T, 16, qbits, true>::quantizeMetaShape(rows, columns, meta_rows, meta_cols);
            } else {
                BlockwiseQuantizer<T, 16, qbits, false>::quantizeMetaShape(rows, columns, meta_rows, meta_cols);
            }
            break;
        }
        case 32: {
            if (columnwise) {
                BlockwiseQuantizer<T, 32, qbits, true>::quantizeMetaShape(rows, columns, meta_rows, meta_cols);
            } else {
                BlockwiseQuantizer<T, 32, qbits, false>::quantizeMetaShape(
                                    rows, columns, meta_rows, meta_cols);
            }
            break;
        }
        case 64: {
            if (columnwise) {
                BlockwiseQuantizer<T, 64, qbits, true>::quantizeMetaShape(rows, columns, meta_rows,
                                                                      meta_cols);
            } else {
                BlockwiseQuantizer<T, 64, qbits, false>::quantizeMetaShape(rows, columns, meta_rows,
                                                                       meta_cols);
            }
            break;
        }
        case 128: {
            if (columnwise) {
                BlockwiseQuantizer<T, 128, qbits, true>::quantizeMetaShape(rows, columns, meta_rows,
                                                                      meta_cols);
            } else {
                BlockwiseQuantizer<T, 128, qbits, false>::quantizeMetaShape(rows, columns, meta_rows,
                                                                       meta_cols);
            }
            break;
        }
        case 256: {
            if (columnwise) {
                BlockwiseQuantizer<T, 256, qbits, true>::quantizeMetaShape(rows, columns, meta_rows,
                                                                      meta_cols);
            } else {
                BlockwiseQuantizer<T, 256, qbits, false>::quantizeMetaShape(rows, columns, meta_rows,
                                                                       meta_cols);
            }
            break;
        }
        default:
            meta_rows = 0;
            meta_cols = 0;
            break;
    }
}



template <typename T, int qbits>
void
MlasBlockwiseQuantizedShape(
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    int& q_rows,
    int& q_cols
    )
{
    switch (block_size) {
        case 16: {
            if (columnwise) {
                BlockwiseQuantizer<T, 16, qbits, true>::quantizedShape(rows, columns, q_rows, q_cols);
            } else {
                BlockwiseQuantizer<T, 16, qbits, false>::quantizedShape(rows, columns, q_rows, q_cols);
            }
            break;
        }
        case 32: {
            if (columnwise) {
                BlockwiseQuantizer<T, 32, qbits, true>::quantizedShape(rows, columns, q_rows, q_cols);
            } else {
                BlockwiseQuantizer<T, 32, qbits, false>::quantizedShape(
                                    rows, columns, q_rows, q_cols);
            }
            break;
        }
        case 64: {
            if (columnwise) {
                BlockwiseQuantizer<T, 64, qbits, true>::quantizedShape(rows, columns, q_rows, q_cols);
            } else {
                BlockwiseQuantizer<T, 64, qbits, false>::quantizedShape(rows, columns, q_rows, q_cols);
            }
            break;
        }
        case 128: {
            if (columnwise) {
                BlockwiseQuantizer<T, 128, qbits, true>::quantizedShape(rows, columns, q_rows, q_cols);
            } else {
                BlockwiseQuantizer<T, 128, qbits, false>::quantizedShape(rows, columns, q_rows, q_cols);
            }
            break;
        }
        case 256: {
            if (columnwise) {
                BlockwiseQuantizer<T, 256, qbits, true>::quantizedShape(rows, columns, q_rows, q_cols);
            } else {
                BlockwiseQuantizer<T, 256, qbits, false>::quantizedShape(rows, columns, q_rows, q_cols);
            }
            break;
        }
        default:
            q_rows = 0;
            q_cols = 0;
            break;
    }
}


template
void
MlasBlockwiseQuantMetaShape<float, 4>(
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    int& meta_rows,
    int& meta_cols
    );

template
void
MlasBlockwiseQuantMetaShape<MLAS_FP16, 4>(
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    int& meta_rows,
    int& meta_cols
    );

template
void
MlasBlockwiseQuantizedShape<float, 4>(
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    int& q_rows,
    int& q_cols
    );

template
void
MlasBlockwiseQuantizedShape<MLAS_FP16, 4>(
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    int& q_rows,
    int& q_cols
    );

void MLASCALL
MlasBlockwiseQuantizedBufferSizes(
    int qbits,
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    size_t& q_data_size_in_bytes,
    size_t& q_scale_num_elements,
    size_t* q_zero_point_size_in_bytes
)
{
    q_data_size_in_bytes = q_scale_num_elements = 0;
    if (q_zero_point_size_in_bytes) {
        *q_zero_point_size_in_bytes = 0;
    }

    if (qbits == 4) {
        switch (block_size) {
            case 16:
                if (columnwise) {
                    BlockwiseQuantizer<float, 16, 4, true>::quantizedBufferSizes(
                        rows, columns, q_data_size_in_bytes, q_scale_num_elements, q_zero_point_size_in_bytes
                    );
                } else {
                    BlockwiseQuantizer<float, 16, 4, false>::quantizedBufferSizes(
                        rows, columns, q_data_size_in_bytes, q_scale_num_elements, q_zero_point_size_in_bytes
                    );
                }
                break;

            case 32:
                if (columnwise) {
                    BlockwiseQuantizer<float, 32, 4, true>::quantizedBufferSizes(
                        rows, columns, q_data_size_in_bytes, q_scale_num_elements, q_zero_point_size_in_bytes
                    );
                } else {
                    BlockwiseQuantizer<float, 32, 4, false>::quantizedBufferSizes(
                        rows, columns, q_data_size_in_bytes, q_scale_num_elements, q_zero_point_size_in_bytes
                    );
                }
                break;

            case 64:
                if (columnwise) {
                    BlockwiseQuantizer<float, 64, 4, true>::quantizedBufferSizes(
                        rows, columns, q_data_size_in_bytes, q_scale_num_elements, q_zero_point_size_in_bytes
                    );
                } else {
                    BlockwiseQuantizer<float, 64, 4, false>::quantizedBufferSizes(
                        rows, columns, q_data_size_in_bytes, q_scale_num_elements, q_zero_point_size_in_bytes
                    );
                }
                break;

            case 128:
                if (columnwise) {
                    BlockwiseQuantizer<float, 128, 4, true>::quantizedBufferSizes(
                        rows, columns, q_data_size_in_bytes, q_scale_num_elements, q_zero_point_size_in_bytes
                    );
                } else {
                    BlockwiseQuantizer<float, 128, 4, false>::quantizedBufferSizes(
                        rows, columns, q_data_size_in_bytes, q_scale_num_elements, q_zero_point_size_in_bytes
                    );
                }
                break;

            case 256:
                if (columnwise) {
                    BlockwiseQuantizer<float, 256, 4, true>::quantizedBufferSizes(
                        rows, columns, q_data_size_in_bytes, q_scale_num_elements, q_zero_point_size_in_bytes
                    );
                } else {
                    BlockwiseQuantizer<float, 256, 4, false>::quantizedBufferSizes(
                        rows, columns, q_data_size_in_bytes, q_scale_num_elements, q_zero_point_size_in_bytes
                    );
                }
                break;

            default:
                // Only block size 16, 32, 64, 128, 256 are supported.
                break;
        }
    }
}


template <typename T, int qbits>
void
MlasQuantizeBlockwise(
    uint8_t* dst,
    T* scales,
    uint8_t* zero_points,
    const T* src,
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    int leading_dimension,
    MLAS_THREADPOOL* thread_pool
    )
{
    switch (block_size) {
        case 16:
            if (columnwise) {
                BlockwiseQuantizer<T, 16, qbits, true>::quantizeAndTranspose(
                    dst, scales, zero_points, src, rows, columns, leading_dimension, thread_pool);
            } else {
                BlockwiseQuantizer<T, 16, qbits, false>::quantizeAndTranspose(
                    dst, scales, zero_points, src, rows, columns, leading_dimension, thread_pool);
            }
            break;

        case 32:
            if (columnwise) {
                BlockwiseQuantizer<T, 32, qbits, true>::quantizeAndTranspose(
                    dst, scales, zero_points, src, rows, columns, leading_dimension, thread_pool);
            } else {
                BlockwiseQuantizer<T, 32, qbits, false>::quantizeAndTranspose(
                    dst, scales, zero_points, src, rows, columns, leading_dimension, thread_pool);
            }
            break;

        case 64:
            if (columnwise) {
                BlockwiseQuantizer<T, 64, qbits, true>::quantizeAndTranspose(
                    dst, scales, zero_points, src, rows, columns, leading_dimension, thread_pool);
            } else {
                BlockwiseQuantizer<T, 64, qbits, false>::quantizeAndTranspose(
                    dst, scales, zero_points, src, rows, columns, leading_dimension, thread_pool);
            }
            break;

        case 128:
            if (columnwise) {
                BlockwiseQuantizer<T, 128, qbits, true>::quantizeAndTranspose(
                    dst, scales, zero_points, src, rows, columns, leading_dimension, thread_pool);
            } else {
                BlockwiseQuantizer<T, 128, qbits, false>::quantizeAndTranspose(
                    dst, scales, zero_points, src, rows, columns, leading_dimension, thread_pool);
            }
            break;

        case 256:
            if (columnwise) {
                BlockwiseQuantizer<T, 256, qbits, true>::quantizeAndTranspose(
                    dst, scales, zero_points, src, rows, columns, leading_dimension, thread_pool);
            } else {
                BlockwiseQuantizer<T, 256, qbits, false>::quantizeAndTranspose(
                    dst, scales, zero_points, src, rows, columns, leading_dimension, thread_pool);
            }
            break;

        default:
            // Only block size 16, 32, 64, 128, 256 are supported.
            break;
    }
}

template
void
MlasQuantizeBlockwise<float, 4>(
    uint8_t* dst,
    float* scales,
    uint8_t* zero_points,
    const float* src,
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    int leading_dimension,
    MLAS_THREADPOOL* thread_pool
    );

template
void
MlasQuantizeBlockwise<MLAS_FP16, 4>(
    uint8_t* dst,
    MLAS_FP16* scales,
    uint8_t* zero_points,
    const MLAS_FP16* src,
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    int leading_dimension,
    MLAS_THREADPOOL* thread_pool
    );


template <typename T, int qbits>
void
MlasDequantizeBlockwise(
    T* dst,
    const uint8_t* src,
    const T* scales,
    const uint8_t* zero_points,
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    MLAS_THREADPOOL* thread_pool
    )
{
    switch (block_size) {
        case 16:
            if (columnwise) {
                BlockwiseQuantizer<T, 16, qbits, true>::dequantize(dst, src, scales, zero_points, rows,
                                                               columns, thread_pool);
            } else {
                BlockwiseQuantizer<T, 16, qbits, false>::dequantize(dst, src, scales, zero_points, rows,
                                                                columns, thread_pool);
            }
            break;
        case 32:
            if (columnwise) {
                BlockwiseQuantizer<T, 32, qbits, true>::dequantize(dst, src, scales, zero_points, rows,
                                                               columns, thread_pool);
            } else {
                BlockwiseQuantizer<T, 32, qbits, false>::dequantize(dst, src, scales, zero_points, rows,
                                                                columns, thread_pool);
            }
            break;
        case 64:
            if (columnwise) {
                BlockwiseQuantizer<T, 64, qbits, true>::dequantize(dst, src, scales, zero_points, rows,
                                                               columns, thread_pool);
            } else {
                BlockwiseQuantizer<T, 64, qbits, false>::dequantize(dst, src, scales, zero_points, rows,
                                                                columns, thread_pool);
            }
            break;
        case 128:
            if (columnwise) {
                BlockwiseQuantizer<T, 128, qbits, true>::dequantize(dst, src, scales, zero_points, rows,
                                                                columns, thread_pool);
            } else {
                BlockwiseQuantizer<T, 128, qbits, false>::dequantize(dst, src, scales, zero_points,
                                                                 rows, columns, thread_pool);
            }
            break;
        case 256:
            if (columnwise) {
                BlockwiseQuantizer<T, 256, qbits, true>::dequantize(dst, src, scales, zero_points, rows,
                                                                columns, thread_pool);
            } else {
                BlockwiseQuantizer<T, 256, qbits, false>::dequantize(dst, src, scales, zero_points,
                                                                 rows, columns, thread_pool);
            }
            break;
        default:
            // Only block size 16, 32, 64, 128, 256 are supported.
            break;
    }
}

template
void
MlasDequantizeBlockwise<float, 4>(
    float* dst,
    const uint8_t* src,
    const float* scales,
    const uint8_t* zero_points,
    int block_size,
    bool columnwise,
    int rows,
    int columns,
    MLAS_THREADPOOL* thread_pool
    );
