/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
// clang-format off
{% set wdesc =  "weighted" if weighted else "unweighted" %}

#include <ATen/ATen.h>
#include <ATen/Context.h>

#include "fbgemm_gpu/cpu_utils.h"
#include "fbgemm_gpu/dispatch_macros.h"
#include "fbgemm_gpu/embedding_common.h"
#include "fbgemm/FbgemmEmbedding.h"
#include "fbgemm_gpu/sparse_ops_utils.h"

#include <immintrin.h>
#include <emmintrin.h>

namespace {

using Tensor = at::Tensor;

inline uint32_t pruned_hash_function(uint32_t h) {
    // MurmorHash3 32-bit mixing function.
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

} // namespace

void pruned_hashmap_insert_{{ wdesc }}_cpu(
    Tensor indices,
    Tensor dense_indices,
    Tensor offsets,
    Tensor hash_table,
    Tensor hash_table_offsets) {
    TENSOR_ON_CPU(indices);
    TENSOR_ON_CPU(dense_indices);
    TENSOR_ON_CPU(offsets);
    TENSOR_ON_CPU(hash_table);
    TENSOR_ON_CPU(hash_table_offsets);

    int32_t T = hash_table_offsets.size(0) - 1;
    int32_t B = (offsets.size(0) - 1) / T;
    TORCH_CHECK(B > 0);
    const auto* indices_acc = indices.data_ptr<int32_t>();
    const auto* dense_indices_acc = dense_indices.data_ptr<int32_t>();

    const auto* offsets_acc = offsets.data_ptr<int32_t>();
    auto hash_table_acc = hash_table.accessor<int32_t, 2>();
    const auto hash_table_offsets_acc = hash_table_offsets.accessor<int64_t, 1>();


    for (int32_t t = 0; t < T; ++t) {
        int64_t table_start = hash_table_offsets_acc[t];
        int64_t table_end = hash_table_offsets_acc[t + 1];
        if (table_start == table_end) {
            continue;
        }
        int64_t capacity = table_end - table_start;
        for (int32_t b = 0; b < B; ++b) {
            int32_t indices_start = offsets_acc[t * B + b];
            int32_t indices_end = offsets_acc[t * B + b + 1];
            int32_t L = indices_end - indices_start;
            for (int32_t l = 0; l < L; ++l) {
                int32_t idx = indices_acc[indices_start + l];
                int32_t dense_idx = dense_indices_acc[indices_start + l];
                if (dense_idx == -1) {
                    // -1 means this row has been pruned, do not insert it.
                    continue;
                }

                uint32_t slot = pruned_hash_function(static_cast<uint32_t>(idx)) % capacity;
                while (true) {
                    int32_t slot_sparse_idx = hash_table_acc[table_start + static_cast<int64_t>(slot)][0];
                    // empty slot
                    if (slot_sparse_idx == -1) {
                        hash_table_acc[table_start + static_cast<int64_t>(slot)][0] = idx;
                        hash_table_acc[table_start + static_cast<int64_t>(slot)][1] = dense_idx;
                        break;
                    }
                    // already exists (shouldn't happen in practice)
                    if (slot_sparse_idx == idx) {
                        hash_table_acc[table_start + static_cast<int64_t>(slot)][1] = dense_idx;
                        break;
                    }
                    // linear probe
                    slot = (slot + 1) % capacity;
                }
            }
        }
    }
    return;
}

Tensor int_nbit_split_embedding_codegen_forward_{{ wdesc }}_cpu(
    Tensor dev_weights,
    Tensor uvm_weights,
    Tensor weights_placements,
    Tensor weights_offsets,
    Tensor weights_tys,
    Tensor D_offsets,
    int64_t total_D,
    Tensor indices,
    Tensor offsets,
    int64_t pooling_mode,
    int64_t row_alignment,
    {% if weighted %}
    Tensor indice_weights,
    {% endif %}
    int64_t output_dtype,
    int64_t unused
) {
    TENSOR_ON_CPU(dev_weights);
    TENSOR_ON_CPU(uvm_weights);
    TENSOR_ON_CPU(weights_placements);
    TENSOR_ON_CPU(weights_offsets);
    TENSOR_ON_CPU(weights_tys);
    TENSOR_ON_CPU(D_offsets);
    TENSOR_ON_CPU(indices);
    TENSOR_ON_CPU(offsets);
    {% if weighted %}
    TENSOR_EMPTY_OR_ON_CPU(indice_weights);
    {% endif %}

    int32_t T = D_offsets.numel() - 1;
    TORCH_CHECK(T > 0);
    // offsets = [B x T  + 1]
    int32_t B = (offsets.size(0) - 1) / T;
    TORCH_CHECK(B >= 0);
    TORCH_CHECK(total_D > 0);
    bool pinned_memory = false;
    if (at::Context::hasCUDA() && at::getNumGPUs() > 0) {
      pinned_memory = true;
    }

    Tensor output;
    const int kINT8QparamsBytes = 8;
    SparseType o_dtype = static_cast<SparseType>(output_dtype);
    TORCH_CHECK(o_dtype == SparseType::FP32 || o_dtype == SparseType::FP16 || o_dtype == SparseType::INT8);
    int64_t total_adjusted_D = total_D;
    if (o_dtype == SparseType::INT8) {
        total_adjusted_D += T * kINT8QparamsBytes;
    }
    output = at::empty({B, total_adjusted_D}, dev_weights.options().dtype(getScalarType(o_dtype)).pinned_memory(pinned_memory));

    if (B == 0) {
        return output;
    }

    const int32_t* weights_placements_ptr = weights_placements.data_ptr<int32_t>();
    const uint8_t* weights_acc;

    const auto* weights_tys_acc = weights_tys.data_ptr<uint8_t>();

    DISPATCH_OUTPUT_TYPES(output.type(), "intn_split_embedding_codegen_forward_kernel", [&] {
        auto* output_acc = output.data_ptr<output_t>();
        {% if weighted %}
        const float* indice_weights_acc = indice_weights.data_ptr<float>();
        {% endif %}

        AT_DISPATCH_INDEX_TYPES(indices.scalar_type(), "int_nbit_split_embedding_codegen_forward_", [&] {
            const auto* indices_acc = indices.data_ptr<index_t>();
            const auto* offsets_acc = offsets.data_ptr<index_t>();
            const auto* D_offsets_acc = D_offsets.data_ptr<int32_t>();
            const auto* weights_offsets_acc = weights_offsets.data_ptr<int64_t>();

            int32_t num_indices_m_1 = indices.numel() - 1;

            for (int32_t t = 0; t < T; ++t) {
                const int32_t D_start = D_offsets_acc[t];
                const int32_t D = D_offsets_acc[t+1] - D_offsets_acc[t];
                const auto placement = static_cast<PlacementType>(weights_placements_ptr[t]);
                TORCH_CHECK(placement != PlacementType::DEVICE);
                Tensor weight_tensor = (placement == PlacementType::HOST) ? dev_weights : uvm_weights;
                weights_acc = weight_tensor.data_ptr<uint8_t>();
                const uint8_t* weights = &weights_acc[weights_offsets_acc[t]];
                auto weight_ty = static_cast<SparseType>(weights_tys_acc[t]);
                // default to 1 byte alignment for CPU TBE
                const int32_t D_bytes = nbit::padded_row_size_in_bytes(D, weight_ty, row_alignment);

                int tt;
                for (tt = t + 1; tt < T && weights_offsets_acc[tt] == weights_offsets_acc[t]; ++tt);
                size_t num_rows = ((tt == T ? weight_tensor.numel() : weights_offsets_acc[tt]) - weights_offsets_acc[t]) / D_bytes;
                const index_t* offsets_begin_ptr = offsets_acc + t * B;

                using float16 = uint16_t;
                using fbgemm_out_t = typename std::conditional<
                    std::is_same<output_t, at::Half>::value,
                    float16,
                    float>::type;

                bool success = true;
                bool has_weight = {{ "true" if weighted else "false" }};
                bool normalize_by_lengths = static_cast<PoolingMode>(pooling_mode) == PoolingMode::MEAN;

                index_t index_size = offsets_acc[(t + 1) * B] - *offsets_begin_ptr;
                const float* indice_weights_ptr = nullptr;
                {% if weighted %}
                indice_weights_ptr = indice_weights_acc + *offsets_begin_ptr;
                {% endif %}
                if (weight_ty == SparseType::FP32) {
                    auto kernel = fbgemm::GenerateEmbeddingSpMDMWithStrides<float, index_t, index_t, fbgemm_out_t, /*THREAD_LOCAL=*/true>(
                        D,
                        has_weight,
                        normalize_by_lengths,
                        /*prefetch=*/16,
                        /*is_weight_positional=*/false,
                        /*use_offsets=*/true,
                        /*output_stride=*/total_D,
                        /*input_stride=*/D_bytes / sizeof(float),
                        /*scale_bias_last=*/false);
                    success = kernel(
                        B,
                        index_size,
                        num_rows,
                        reinterpret_cast<const float*>(weights),
                        indices_acc + *offsets_begin_ptr,
                        offsets_begin_ptr,
                        indice_weights_ptr,
                        reinterpret_cast<fbgemm_out_t*>(output_acc + D_start));
                } else if (weight_ty == SparseType::FP16) {
                    auto kernel = fbgemm::GenerateEmbeddingSpMDMWithStrides<float16, index_t, index_t, fbgemm_out_t, /*THREAD_LOCAL=*/true>(
                        D,
                        has_weight,
                        normalize_by_lengths,
                        /*prefetch=*/16,
                        /*is_weight_positional=*/false,
                        /*use_offsets=*/true,
                        /*output_stride=*/total_D,
                        /*input_stride=*/D_bytes / sizeof(float16),
                        /*scale_bias_last=*/false);
                    success = kernel(
                        B,
                        index_size,
                        num_rows,
                        reinterpret_cast<const float16*>(weights),
                        indices_acc + *offsets_begin_ptr,
                        offsets_begin_ptr,
                        indice_weights_ptr,
                        reinterpret_cast<fbgemm_out_t*>(output_acc + D_start));
                } else if (weight_ty == SparseType::INT8) {
                    auto kernel = fbgemm::GenerateEmbeddingSpMDMWithStrides<uint8_t, index_t, index_t, fbgemm_out_t, /*THREAD_LOCAL=*/true>(
                        D,
                        has_weight,
                        normalize_by_lengths,
                        /*prefetch=*/16,
                        /*is_weight_positional=*/false,
                        /*use_offsets=*/true,
                        /*output_stride=*/total_D,
                        /*input_stride=*/D_bytes / sizeof(uint8_t),
                        /*scale_bias_last=*/false);
                    success = kernel(
                        B,
                        index_size,
                        num_rows,
                        weights,
                        indices_acc + *offsets_begin_ptr,
                        offsets_begin_ptr,
                        indice_weights_ptr,
                        reinterpret_cast<fbgemm_out_t*>(output_acc + D_start));
                } else if (weight_ty == SparseType::INT4 || weight_ty == SparseType::INT2) {
                    int bit_rate;
                    switch (weight_ty) {
                        case SparseType::INT4 :
                          bit_rate = 4;
                          break;
                        case SparseType::INT2 :
                          bit_rate = 2;
                          break;
                        default:
                          throw std::logic_error("Unsupported SparseType: " + std::to_string(static_cast<int>(weight_ty)));
                    }
                    auto kernel = fbgemm::GenerateEmbeddingSpMDMNBitWithStrides<index_t, index_t, fbgemm_out_t, /*THREAD_LOCAL=*/true>(
                        /*bit_rate=*/bit_rate,
                        D,
                        has_weight,
                        normalize_by_lengths,
                        /*prefetch=*/16,
                        /*is_weight_positional=*/false,
                        /*use_offsets=*/true,
                        /*output_stride=*/total_D,
                        /*input_stride=*/D_bytes / sizeof(uint8_t),
                        /*scale_bias_last=*/false);
                    success = kernel(
                        B,
                        index_size,
                        num_rows,
                        weights,
                        indices_acc + *offsets_begin_ptr,
                        offsets_begin_ptr,
                        indice_weights_ptr,
                        reinterpret_cast<fbgemm_out_t*>(output_acc + D_start));
                } else {
                    throw std::logic_error("Unsupported SparseType: " + std::to_string(static_cast<int>(weight_ty)));
                }
                if (!success) {
                    fbgemm_gpu::report_embedding_error(
                        t,
                        B,
                        0,
                        B,
                        offsets_acc,
                        indices_acc,
                        num_rows,
                        /*allow_minus_one=*/true);
                }
            }
            return;
        });
    });
    return output;
}

Tensor pruned_hashmap_lookup_{{ wdesc }}_cpu(
    Tensor indices,
    Tensor offsets,
    Tensor hash_table,
    Tensor hash_table_offsets) {
    TENSOR_ON_CPU(indices);
    TENSOR_ON_CPU(offsets);
    TENSOR_ON_CPU(hash_table);
    TENSOR_ON_CPU(hash_table_offsets);

    int32_t T = hash_table_offsets.size(0) - 1;
    int32_t B = (offsets.size(0) - 1) / T;
    TORCH_CHECK(B > 0);
    auto dense_indices = empty_like(indices);
    const auto* indices_acc = indices.data_ptr<int32_t>();
    auto* dense_indices_acc = dense_indices.data_ptr<int32_t>();

    const auto* offsets_acc = offsets.data_ptr<int32_t>();
    const auto hash_table_acc = hash_table.accessor<int32_t, 2>();
    const auto hash_table_offsets_acc = hash_table_offsets.accessor<int64_t, 1>();

    for (int32_t t = 0; t < T; ++t) {
        int64_t table_start = hash_table_offsets_acc[t];
        int64_t table_end = hash_table_offsets_acc[t + 1];
        int64_t capacity = table_end - table_start;

        for (int32_t b = 0; b < B; ++b) {
            int32_t indices_start = offsets_acc[t * B + b];
            int32_t indices_end = offsets_acc[t * B + b + 1];
            int32_t L = indices_end - indices_start;

            if (table_start == table_end) {
                for (int32_t l = 0; l < L; ++l) {
                    dense_indices_acc[indices_start + l] = indices_acc[indices_start + l];
                }
            } else {
                for (int32_t l = 0; l < L; ++l) {
                    int32_t idx = indices_acc[indices_start + l];
                    uint32_t slot = pruned_hash_function(static_cast<uint32_t>(idx)) % capacity;
                    while (true) {
                        int32_t slot_sparse_idx = hash_table_acc[table_start + static_cast<int64_t>(slot)][0];

                        // empty slot
                        if (slot_sparse_idx == -1) {
                            dense_indices_acc[indices_start + l] = -1;
                            break;
                        }
                        // already exists
                        if (slot_sparse_idx == idx) {
                            dense_indices_acc[indices_start + l] = hash_table_acc[table_start + static_cast<int64_t>(slot)][1];
                            break;
                        }
                        // linear probe
                        slot = (slot + 1) % capacity;
                    }
                }
            }
        }
    }
    return dense_indices;
}

{% if not weighted %}
Tensor pruned_array_lookup_cpu(
    Tensor indices,
    Tensor offsets,
    Tensor index_remappings,
    Tensor index_remappings_offsets) {
    TENSOR_ON_CPU(indices);
    TENSOR_ON_CPU(offsets);
    TENSOR_ON_CPU(index_remappings);
    TENSOR_ON_CPU(index_remappings_offsets);

    int32_t T = index_remappings_offsets.size(0) - 1;
    int32_t B = (offsets.size(0) - 1) / T;
    TORCH_CHECK(B > 0);
    auto dense_indices = empty_like(indices);
    const auto* indices_acc = indices.data_ptr<int32_t>();
    auto* dense_indices_acc = dense_indices.data_ptr<int32_t>();
    const auto* offsets_acc = offsets.data_ptr<int32_t>();

    const auto index_remappings_acc = index_remappings.data_ptr<int32_t>();
    const auto index_remappings_offsets_acc = index_remappings_offsets.data_ptr<int64_t>();

    for (int32_t t = 0; t < T; ++t) {
        int64_t index_remappings_start = index_remappings_offsets_acc[t];
        int64_t index_remappings_end = index_remappings_offsets_acc[t + 1];
        int64_t capacity = index_remappings_end - index_remappings_start;
        int32_t indices_start = offsets_acc[t * B];
        int32_t indices_end = offsets_acc[(t + 1) * B];
        for (int32_t i = indices_start; i < indices_end; ++i) {
            int32_t idx = indices_acc[i];
            dense_indices[i] = capacity ? index_remappings_acc[index_remappings_start + idx] : idx;
        }
    }
    return dense_indices;
}
{% endif %}
// clang-format on
