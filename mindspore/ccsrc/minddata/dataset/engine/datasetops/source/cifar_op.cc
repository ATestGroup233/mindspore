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
#include "minddata/dataset/engine/datasetops/source/cifar_op.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <set>
#include <utility>

#include "minddata/dataset/core/config_manager.h"
#include "minddata/dataset/core/tensor_shape.h"
#include "minddata/dataset/engine/datasetops/source/sampler/sequential_sampler.h"
#include "minddata/dataset/engine/db_connector.h"
#include "minddata/dataset/engine/execution_tree.h"
#include "utils/ms_utils.h"

namespace mindspore {
namespace dataset {
constexpr uint32_t kCifarImageHeight = 32;
constexpr uint32_t kCifarImageWidth = 32;
constexpr uint32_t kCifarImageChannel = 3;
constexpr uint32_t kCifarBlockImageNum = 5;
constexpr uint32_t kCifarImageSize = kCifarImageHeight * kCifarImageWidth * kCifarImageChannel;

CifarOp::Builder::Builder() : sampler_(nullptr), usage_("") {
  std::shared_ptr<ConfigManager> cfg = GlobalContext::config_manager();
  num_workers_ = cfg->num_parallel_workers();
  op_connect_size_ = cfg->op_connector_size();
  cifar_type_ = kCifar10;
}

Status CifarOp::Builder::Build(std::shared_ptr<CifarOp> *ptr) {
  RETURN_IF_NOT_OK(SanityCheck());
  if (sampler_ == nullptr) {
    const int64_t num_samples = 0;
    const int64_t start_index = 0;
    sampler_ = std::make_shared<SequentialSamplerRT>(start_index, num_samples);
  }
  schema_ = std::make_unique<DataSchema>();
  TensorShape scalar = TensorShape::CreateScalar();
  RETURN_IF_NOT_OK(schema_->AddColumn(ColDescriptor("image", DataType(DataType::DE_UINT8), TensorImpl::kFlexible, 1)));
  if (cifar_type_ == kCifar10) {
    RETURN_IF_NOT_OK(
      schema_->AddColumn(ColDescriptor("label", DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 0, &scalar)));
  } else {
    RETURN_IF_NOT_OK(schema_->AddColumn(
      ColDescriptor("coarse_label", DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 0, &scalar)));
    TensorShape another_scalar = TensorShape::CreateScalar();
    RETURN_IF_NOT_OK(schema_->AddColumn(
      ColDescriptor("fine_label", DataType(DataType::DE_UINT32), TensorImpl::kFlexible, 0, &another_scalar)));
  }

  *ptr = std::make_shared<CifarOp>(cifar_type_, usage_, num_workers_, dir_, op_connect_size_, std::move(schema_),
                                   std::move(sampler_));
  return Status::OK();
}

Status CifarOp::Builder::SanityCheck() {
  const std::set<std::string> valid = {"test", "train", "all", ""};
  Path dir(dir_);
  std::string err_msg;
  err_msg +=
    dir.IsDirectory() == false ? "Invalid parameter, Cifar path is invalid or not set, path: " + dir_ + ".\n" : "";
  err_msg += num_workers_ <= 0 ? "Invalid parameter, num_parallel_workers must be greater than 0, but got " +
                                   std::to_string(num_workers_) + ".\n"
                               : "";
  err_msg += valid.find(usage_) == valid.end()
               ? "Invalid parameter, usage must be 'train','test' or 'all', but got " + usage_ + ".\n"
               : "";
  return err_msg.empty() ? Status::OK() : Status(StatusCode::kMDUnexpectedError, __LINE__, __FILE__, err_msg);
}

CifarOp::CifarOp(CifarType type, const std::string &usage, int32_t num_works, const std::string &file_dir,
                 int32_t queue_size, std::unique_ptr<DataSchema> data_schema, std::shared_ptr<SamplerRT> sampler)
    : MappableLeafOp(num_works, queue_size, std::move(sampler)),
      cifar_type_(type),
      usage_(usage),
      folder_path_(file_dir),
      data_schema_(std::move(data_schema)) {
  constexpr uint64_t kUtilQueueSize = 512;
  cifar_raw_data_block_ = std::make_unique<Queue<std::vector<unsigned char>>>(kUtilQueueSize);
  io_block_queues_.Init(num_workers_, queue_size);
}

Status CifarOp::LaunchThreadsAndInitOp() {
  if (tree_ == nullptr) {
    RETURN_STATUS_UNEXPECTED("Pipeline init failed, Execution tree not set.");
  }
  RETURN_IF_NOT_OK(io_block_queues_.Register(tree_->AllTasks()));
  RETURN_IF_NOT_OK(wait_for_workers_post_.Register(tree_->AllTasks()));
  RETURN_IF_NOT_OK(tree_->AllTasks()->CreateAsyncTask(
    "Get cifar data block", std::bind(&CifarOp::ReadCifarBlockDataAsync, this), nullptr, id()));
  RETURN_IF_NOT_OK(
    tree_->LaunchWorkers(num_workers_, std::bind(&CifarOp::WorkerEntry, this, std::placeholders::_1), "", id()));
  TaskManager::FindMe()->Post();
  // The order of the following 2 functions must not be changed!
  RETURN_IF_NOT_OK(ParseCifarData());  // Parse cifar data and get num rows, blocking
  RETURN_IF_NOT_OK(InitSampler());     // Pass numRows to Sampler
  return Status::OK();
}

// Load 1 TensorRow (image,label). 1 function call produces 1 TensorTow
Status CifarOp::LoadTensorRow(row_id_type index, TensorRow *trow) {
  std::shared_ptr<Tensor> label;
  std::shared_ptr<Tensor> fine_label;
  std::shared_ptr<Tensor> ori_image = cifar_image_label_pairs_[index].first;
  std::shared_ptr<Tensor> copy_image;
  uint64_t path_index = std::ceil(index / kCifarBlockImageNum);
  RETURN_IF_NOT_OK(Tensor::CreateFromTensor(ori_image, &copy_image));
  RETURN_IF_NOT_OK(Tensor::CreateScalar(cifar_image_label_pairs_[index].second[0], &label));

  if (cifar_image_label_pairs_[index].second.size() > 1) {
    RETURN_IF_NOT_OK(Tensor::CreateScalar(cifar_image_label_pairs_[index].second[1], &fine_label));
    (*trow) = TensorRow(index, {copy_image, std::move(label), std::move(fine_label)});
    // Add file path info
    trow->setPath({path_record_[path_index], path_record_[path_index], path_record_[path_index]});
  } else {
    (*trow) = TensorRow(index, {copy_image, std::move(label)});
    // Add file path info
    trow->setPath({path_record_[path_index], path_record_[path_index]});
  }

  return Status::OK();
}

void CifarOp::Print(std::ostream &out, bool show_all) const {
  if (!show_all) {
    // Call the super class for displaying any common 1-liner info
    ParallelOp::Print(out, show_all);
    // Then show any custom derived-internal 1-liner info for this op
    out << "\n";
  } else {
    // Call the super class for displaying any common detailed info
    ParallelOp::Print(out, show_all);
    // Then show any custom derived-internal stuff
    out << "\nNumber of rows:" << num_rows_ << "\nCifar directory: " << folder_path_ << "\n\n";
  }
}

Status CifarOp::ReadCifarBlockDataAsync() {
  TaskManager::FindMe()->Post();
  RETURN_IF_NOT_OK(GetCifarFiles());
  if (cifar_type_ == kCifar10) {
    RETURN_IF_NOT_OK(ReadCifar10BlockData());
  } else {
    RETURN_IF_NOT_OK(ReadCifar100BlockData());
  }

  return Status::OK();
}

Status CifarOp::ReadCifar10BlockData() {
  // CIFAR 10 has 6 bin files. data_batch_1.bin ... data_batch_5.bin and 1 test_batch.bin file
  // each of the file has exactly 10K images and labels and size is 30,730 KB
  // each image has the dimension of 32 x 32 x 3 = 3072 plus 1 label (label has 10 classes) so each row has 3073 bytes
  constexpr uint32_t num_cifar10_records = 10000;
  uint32_t block_size = (kCifarImageSize + 1) * kCifarBlockImageNum;  // about 2M
  std::vector<unsigned char> image_data(block_size * sizeof(unsigned char), 0);
  for (auto &file : cifar_files_) {
    // check the validity of the file path
    Path file_path(file);
    CHECK_FAIL_RETURN_UNEXPECTED(file_path.Exists() && !file_path.IsDirectory(),
                                 "Invalid file, failed to find cifar10 file: " + file);
    std::string file_name = file_path.Basename();

    if (usage_ == "train") {
      if (file_name.find("data_batch") == std::string::npos) continue;
    } else if (usage_ == "test") {
      if (file_name.find("test_batch") == std::string::npos) continue;
    } else {  // get all the files that contain the word batch, aka any cifar 100 files
      if (file_name.find("batch") == std::string::npos) continue;
    }

    std::ifstream in(file, std::ios::binary);
    CHECK_FAIL_RETURN_UNEXPECTED(in.is_open(), "Invalid file, failed to open cifar10 file: " + file);

    for (uint32_t index = 0; index < num_cifar10_records / kCifarBlockImageNum; ++index) {
      (void)in.read(reinterpret_cast<char *>(&(image_data[0])), block_size * sizeof(unsigned char));
      CHECK_FAIL_RETURN_UNEXPECTED(!in.fail(), "Invalid data, failed to read data from cifar10 file: " + file);
      (void)cifar_raw_data_block_->EmplaceBack(image_data);
      // Add file path info
      path_record_.push_back(file);
    }
    in.close();
  }
  (void)cifar_raw_data_block_->EmplaceBack(std::vector<unsigned char>());  // end block

  return Status::OK();
}

Status CifarOp::ReadCifar100BlockData() {
  // CIFAR 100 has 2 bin files. train.bin (60K imgs)  153,700KB and test.bin (30,740KB) (10K imgs)
  // each img has two labels. Each row then is 32 * 32 *5 + 2 = 3,074 Bytes
  uint32_t num_cifar100_records = 0;  // test:10000, train:50000
  constexpr uint32_t num_cifar100_test_records = 10000;
  constexpr uint32_t num_cifar100_train_records = 50000;
  uint32_t block_size = (kCifarImageSize + 2) * kCifarBlockImageNum;  // about 2M
  std::vector<unsigned char> image_data(block_size * sizeof(unsigned char), 0);
  for (auto &file : cifar_files_) {
    // check the validity of the file path
    Path file_path(file);
    CHECK_FAIL_RETURN_UNEXPECTED(file_path.Exists() && !file_path.IsDirectory(),
                                 "Invalid file, failed to find cifar100 file: " + file);
    std::string file_name = file_path.Basename();

    // if usage is train/test, get only these 2 files
    if (usage_ == "train" && file_name.find("train") == std::string::npos) continue;
    if (usage_ == "test" && file_name.find("test") == std::string::npos) continue;

    if (file_name.find("test") != std::string::npos) {
      num_cifar100_records = num_cifar100_test_records;
    } else if (file_name.find("train") != std::string::npos) {
      num_cifar100_records = num_cifar100_train_records;
    } else {
      RETURN_STATUS_UNEXPECTED("Invalid file, Cifar100 train/test file not found in: " + file_name);
    }

    std::ifstream in(file, std::ios::binary);
    CHECK_FAIL_RETURN_UNEXPECTED(in.is_open(), "Invalid file, failed to open cifar100 file: " + file);

    for (uint32_t index = 0; index < num_cifar100_records / kCifarBlockImageNum; index++) {
      (void)in.read(reinterpret_cast<char *>(&(image_data[0])), block_size * sizeof(unsigned char));
      CHECK_FAIL_RETURN_UNEXPECTED(!in.fail(), "Invalid data, failed to read data from cifar100 file: " + file);
      (void)cifar_raw_data_block_->EmplaceBack(image_data);
      // Add file path info
      path_record_.push_back(file);
    }
    in.close();
  }
  (void)cifar_raw_data_block_->EmplaceBack(std::vector<unsigned char>());  // block end
  return Status::OK();
}

Status CifarOp::GetCifarFiles() {
  const std::string kExtension = ".bin";
  Path dir_path(folder_path_);
  auto dirIt = Path::DirIterator::OpenDirectory(&dir_path);
  if (dirIt) {
    while (dirIt->hasNext()) {
      Path file = dirIt->next();
      if (file.Extension() == kExtension) {
        cifar_files_.push_back(file.toString());
      }
    }
  } else {
    RETURN_STATUS_UNEXPECTED("Invalid file, failed to open directory: " + dir_path.toString());
  }
  CHECK_FAIL_RETURN_UNEXPECTED(!cifar_files_.empty(), "Invalid file, no .bin files found under " + folder_path_);
  std::sort(cifar_files_.begin(), cifar_files_.end());
  return Status::OK();
}

Status CifarOp::ParseCifarData() {
  std::vector<unsigned char> block;
  RETURN_IF_NOT_OK(cifar_raw_data_block_->PopFront(&block));
  uint32_t cur_block_index = 0;
  while (!block.empty()) {
    for (uint32_t index = 0; index < kCifarBlockImageNum; ++index) {
      std::vector<uint32_t> labels;
      uint32_t label = block[cur_block_index++];
      labels.push_back(label);
      if (cifar_type_ == kCifar100) {
        uint32_t fine_label = block[cur_block_index++];
        labels.push_back(fine_label);
      }

      std::shared_ptr<Tensor> image_tensor;
      RETURN_IF_NOT_OK(Tensor::CreateEmpty(TensorShape({kCifarImageHeight, kCifarImageWidth, kCifarImageChannel}),
                                           data_schema_->column(0).type(), &image_tensor));
      auto itr = image_tensor->begin<uint8_t>();
      uint32_t total_pix = kCifarImageHeight * kCifarImageWidth;
      for (int pix = 0; pix < total_pix; ++pix) {
        for (int ch = 0; ch < kCifarImageChannel; ++ch) {
          *itr = block[cur_block_index + ch * total_pix + pix];
          itr++;
        }
      }
      cur_block_index += total_pix * kCifarImageChannel;
      cifar_image_label_pairs_.emplace_back(std::make_pair(image_tensor, labels));
    }
    RETURN_IF_NOT_OK(cifar_raw_data_block_->PopFront(&block));
    cur_block_index = 0;
  }
  cifar_image_label_pairs_.shrink_to_fit();
  num_rows_ = cifar_image_label_pairs_.size();
  if (num_rows_ == 0) {
    std::string api = cifar_type_ == kCifar10 ? "Cifar10Dataset" : "Cifar100Dataset";
    RETURN_STATUS_UNEXPECTED("Invalid data, no valid data matching the dataset API " + api +
                             ". Please check file path or dataset API.");
  }
  cifar_raw_data_block_->Reset();
  return Status::OK();
}

// Derived from RandomAccessOp
Status CifarOp::GetClassIds(std::map<int32_t, std::vector<int64_t>> *cls_ids) const {
  if (cls_ids == nullptr || !cls_ids->empty()) {
    RETURN_STATUS_UNEXPECTED(
      "Map for storaging image-index pair is nullptr or has been set in other place,"
      "it must be empty before using GetClassIds.");
  }

  for (uint64_t index = 0; index < cifar_image_label_pairs_.size(); ++index) {
    uint32_t label = (cifar_image_label_pairs_[index].second)[0];
    (*cls_ids)[label].push_back(index);
  }

  for (auto &pair : (*cls_ids)) {
    pair.second.shrink_to_fit();
  }
  return Status::OK();
}

Status CifarOp::CountTotalRows(const std::string &dir, const std::string &usage, bool isCIFAR10, int64_t *count) {
  // the logic of counting the number of samples is copied from ReadCifar100Block() and ReadCifar10Block()
  std::shared_ptr<CifarOp> op;
  *count = 0;
  RETURN_IF_NOT_OK(Builder().SetCifarDir(dir).SetCifarType(isCIFAR10).SetUsage(usage).Build(&op));
  RETURN_IF_NOT_OK(op->GetCifarFiles());
  if (op->cifar_type_ == kCifar10) {
    constexpr int64_t num_cifar10_records = 10000;
    for (auto &file : op->cifar_files_) {
      Path file_path(file);
      CHECK_FAIL_RETURN_UNEXPECTED(file_path.Exists() && !file_path.IsDirectory(),
                                   "Invalid file, failed to open cifar10 file: " + file);
      std::string file_name = file_path.Basename();

      if (op->usage_ == "train") {
        if (file_name.find("data_batch") == std::string::npos) continue;
      } else if (op->usage_ == "test") {
        if (file_name.find("test_batch") == std::string::npos) continue;
      } else {  // get all the files that contain the word batch, aka any cifar 100 files
        if (file_name.find("batch") == std::string::npos) continue;
      }

      std::ifstream in(file, std::ios::binary);

      CHECK_FAIL_RETURN_UNEXPECTED(in.is_open(), "Invalid file, failed to open cifar10 file: " + file);
      *count = *count + num_cifar10_records;
    }
    return Status::OK();
  } else {
    int64_t num_cifar100_records = 0;
    for (auto &file : op->cifar_files_) {
      Path file_path(file);
      std::string file_name = file_path.Basename();

      CHECK_FAIL_RETURN_UNEXPECTED(file_path.Exists() && !file_path.IsDirectory(),
                                   "Invalid file, failed to find cifar100 file: " + file);

      if (op->usage_ == "train" && file_path.Basename().find("train") == std::string::npos) continue;
      if (op->usage_ == "test" && file_path.Basename().find("test") == std::string::npos) continue;

      if (file_name.find("test") != std::string::npos) {
        num_cifar100_records += 10000;
      } else if (file_name.find("train") != std::string::npos) {
        num_cifar100_records += 50000;
      }
      std::ifstream in(file, std::ios::binary);
      CHECK_FAIL_RETURN_UNEXPECTED(in.is_open(), "Invalid file, failed to open cifar100 file: " + file);
    }
    *count = num_cifar100_records;
    return Status::OK();
  }
}

Status CifarOp::ComputeColMap() {
  // set the column name map (base class field)
  if (column_name_id_map_.empty()) {
    for (uint32_t i = 0; i < data_schema_->NumColumns(); ++i) {
      column_name_id_map_[data_schema_->column(i).name()] = i;
    }
  } else {
    MS_LOG(WARNING) << "Column name map is already set!";
  }
  return Status::OK();
}

}  // namespace dataset
}  // namespace mindspore
