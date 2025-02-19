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

#ifndef MINDSPORE_LITE_INCLUDE_REGISTRY_MODEL_PARSER_REGISTRY_H
#define MINDSPORE_LITE_INCLUDE_REGISTRY_MODEL_PARSER_REGISTRY_H
#include <string>
#include <memory>
#include <unordered_map>
#include "include/lite_utils.h"

namespace mindspore::lite {
/// \brief ModelParser defined a model parser
class MS_API ModelParser;

/// \brief ModelParserCreator defined function pointer to get a ModelParser class.
typedef ModelParser *(*ModelParserCreator)();

/// \brief ModelParserRegistry defined registration and storage of ModelParser.
class MS_API ModelParserRegistry {
 public:
  /// \brief Constructor of ModelParserRegistry.
  ModelParserRegistry() = default;

  /// \brief Destructor of ModelParserRegistry.
  ~ModelParserRegistry() = default;

  /// \brief Static method to get a single instance.
  ///
  /// \return Pointer of ModelParserRegistry.
  static ModelParserRegistry *GetInstance();

  /// \brief Method to get a model parser.
  ///
  /// \param[in] fmk Define identification of a certain framework.
  ///
  /// \return Pointer of ModelParser.
  ModelParser *GetModelParser(const std::string &fmk);

  /// \brief Method to register model parser.
  ///
  /// \param[in] fmk Define identification of a certain framework.
  /// \param[in] creator Define function pointer of creating ModelParser.
  void RegParser(const std::string &fmk, ModelParserCreator creator);

  std::unordered_map<std::string, ModelParserCreator> parsers_;
};

/// \brief ModelRegistrar defined registration class of ModelParser.
class MS_API ModelRegistrar {
 public:
  /// \brief Constructor of ModelRegistrar to register ModelParser.
  ///
  /// \param[in] fmk Define identification of a certain framework.
  /// \param[in] creator Define function pointer of creating ModelParser.
  ModelRegistrar(const std::string &fmk, ModelParserCreator creator) {
    ModelParserRegistry::GetInstance()->RegParser(fmk, creator);
  }

  /// \brief Destructor of ModelRegistrar.
  ~ModelRegistrar() = default;
};

/// \brief Defined registering macro to register ModelParser, which called by user directly.
///
/// \param[in] fmk Define identification of a certain framework.
/// \param[in] parserCreator Define function pointer of creating ModelParser.
#define REG_MODEL_PARSER(fmk, parserCreator) static ModelRegistrar g_##type##fmk##ModelParserReg(#fmk, parserCreator);
}  // namespace mindspore::lite

#endif  // MINDSPORE_LITE_INCLUDE_REGISTRY_MODEL_PARSER_REGISTRY_H
