/**
 * Copyright 2021 Huawei Technologies Co., Ltd
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

#ifndef MINDSPORE_NNACL_NNACL_SPLIT_WITH_OVER_LAP_BASE_H_
#define MINDSPORE_NNACL_NNACL_SPLIT_WITH_OVER_LAP_BASE_H_

#include "nnacl/op_base.h"
#include "nnacl/split_parameter.h"

#ifdef __cplusplus
extern "C" {
#endif
int DoSplitWithOverlap(char *in_data, char **out_data, int num_split, int split_dim_size, int element_bytes,
                       int outer_total_dim, int inner_stride, const int *start_indices, const int *end_indices);

int DoSplitWithOverlapParallel(char *in_data, char **out_data, int slice_idx, int split_dim_size, int element_bytes,
                               int outer_total_dim, int inner_stride, const int *start_indices, const int *end_indices);

#ifdef __cplusplus
}
#endif

#endif  // MINDSPORE_NNACL_NNACL_SPLIT_WITH_OVER_LAP_BASE_H_
