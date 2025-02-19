/**
 * Copyright 2019-2021 Huawei Technologies Co., Ltd
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
#ifndef MINDSPORE_CCSRC_MINDDATA_DATASET_ENGINE_DATASETOPS_ZIP_OP_H_
#define MINDSPORE_CCSRC_MINDDATA_DATASET_ENGINE_DATASETOPS_ZIP_OP_H_

#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "minddata/dataset/core/tensor.h"
#include "minddata/dataset/engine/dataset_iterator.h"
#include "minddata/dataset/engine/datasetops/pipeline_op.h"
#include "minddata/dataset/util/status.h"

namespace mindspore {
namespace dataset {

class ZipOp : public PipelineOp {
 public:
  //  The nested builder class inside of the ZipOp is used to help manage all of
  //  the arguments for constructing it.  Use the builder by setting each argument
  //  with the provided set methods, and then finally call the build method to execute
  //  the actual construction.

  class Builder {
   public:
    // Builder constructor.  Creates the builder object.
    // @note No default args
    // @return This is a constructor.
    Builder();

    // Default destructor
    ~Builder() = default;

    // Setter method.
    // @return Builder setter method returns reference to the builder.
    Builder &SetRowsPerBuffer(int32_t rows_per_buffer) {
      builder_rows_per_buffer_ = rows_per_buffer;
      return *this;
    }

    // Setter method.
    // @return Builder setter method returns reference to the builder.
    Builder &SetOpConnectorSize(int32_t op_connector_size) {
      builder_op_connector_size_ = op_connector_size;
      return *this;
    }

    // The builder "build" method creates the ZipOp dataset Operator.
    // @return shared_ptr to the new ZipOp object
    Status Build(std::shared_ptr<ZipOp> *);

   private:
    int32_t builder_rows_per_buffer_;
    int32_t builder_op_connector_size_;

    Status SanityCheck() const;
  };

  // Constructor for ZipOp
  // @param op_connector_size - connector size
  explicit ZipOp(int32_t op_connector_size);

  // Destructor
  ~ZipOp();

  Status EofReceived(int32_t) override;

  Status EoeReceived(int32_t) override;

  // Print function for Zip
  // @param out - output stream to print to
  // @param show_all - if it should print everything
  void Print(std::ostream &out, bool show_all) const override;

  // Provide stream operator for displaying it
  friend std::ostream &operator<<(std::ostream &out, const ZipOp &zo) {
    zo.Print(out, false);
    return out;
  }

  // Class functor operator () override.
  // All dataset ops operate by launching a thread (see ExecutionTree). This class functor will
  // provide the master loop that drives the logic for performing the work
  // @return Status The status code returned
  Status operator()() override;

  // Op name getter
  // @return Name of the current Op
  std::string Name() const override { return kZipOp; }

  Status GetNextRow(TensorRow *row, int32_t worker_id, bool retry_if_eoe) override;
  int32_t num_consumers() const override;
  int32_t num_producers() const override;

 private:
  // Special handle case where an empty row has been received from child iterator
  // @note - we need to drain eoe signals from all children connectors.
  // @details - when this function is called, then we encountered eoe at child iterator
  // we have to drain rows from other child iterators until we hit eoe from all other child iterators
  Status drainPipeline(int32_t skip_child, int32_t worker_id, bool retry_if_eoe);

  // Merges 1 row from each childIterator together
  // @param new_zip_row - input and output, will be a non-empty row if all rows from childConnectors are non-empty
  // @param updateColumnMapping - generates a new column name to index mapping (mColNameIdMap) if set to true
  // @details merge rows from iterator together. This is the main functionality for ZipOp
  //          this function takes one row and fills it with tensors from rows fetched
  //          from childIterators.
  // @example:
  //   Zips multiple rows at a time, the output is store in newZipRow
  //       1    a     T
  //       \    |     /
  //         1, a, T
  Status getNextZippedRow(TensorRow *const new_zip_row, int32_t *skip_child, int32_t worker_id, bool retry_if_eoe);

  // Computing the assignment of the column name map.
  // @return - Status
  Status ComputeColMap() override;
};
}  // namespace dataset
}  // namespace mindspore

#endif  // MINDSPORE_CCSRC_MINDDATA_DATASET_ENGINE_DATASETOPS_ZIP_OP_H_
