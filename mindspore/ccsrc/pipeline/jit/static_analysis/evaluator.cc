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

#include "pipeline/jit/static_analysis/evaluator.h"

#include <algorithm>
#include <utility>
#include <unordered_set>

#include "ir/func_graph_cloner.h"
#include "abstract/utils.h"
#include "debug/trace.h"
#include "utils/ms_context.h"
#include "pipeline/jit/static_analysis/stack_frame.h"

namespace mindspore {
namespace abstract {
namespace {
string EvalEntryLogging(const EvaluatorPtr &evaluator, const AbstractBasePtrList &arg_spec_list,
                        const AnfNodeConfigPtr &out_conf) {
  MS_EXCEPTION_IF_NULL(evaluator);
  std::stringstream ss;
  if (out_conf != nullptr) {
    ss << "Evaluator " << evaluator->ToString() << " run for " << out_conf->node()->scope()->name();
  }
  for (size_t i = 0; i < arg_spec_list.size(); i++) {
    ss << evaluator->ToString() << " input[" << i << "] abstract value: " << arg_spec_list[i]->ToString();
  }
  return ss.str();
}

void EvalFailLogging(const EvaluatorPtr &evaluator, const AbstractBasePtrList &, const AnfNodeConfigPtr &out_conf) {
  MS_EXCEPTION_IF_NULL(evaluator);
  if (out_conf != nullptr) {
    auto node = out_conf->node();
    if (IsValueNode<Primitive>(node)) {
      MS_LOG(ERROR) << "Evaluator " << evaluator->ToString() << " run failed for node " << node->fullname_with_scope()
                    << ", with debug info: " << trace::GetDebugInfo(node->debug_info());
    } else {
      MS_LOG(ERROR) << "Evaluator " << evaluator->ToString() << " run failed for node " << node->DebugString()
                    << ", with debug info: " << trace::GetDebugInfo(node->debug_info());
    }
  }
}
}  // namespace

AnalysisContextPtr BaseFuncGraphEvaluator::MakeContext(const AnalysisEnginePtr &engine,
                                                       const AbstractBasePtrList &args_spec_list) {
  AbstractBasePtrList normalized_args_spec_list = NormalizeArgs(args_spec_list);
  normalized_args_spec_list = BroadenUndeterminedArgs(normalized_args_spec_list);
  FuncGraphPtr fg = GetFuncGraph(engine, normalized_args_spec_list);
  MS_EXCEPTION_IF_NULL(parent_context_);
  AnalysisContextPtr context = parent_context_->NewFuncGraphContext(fg, normalized_args_spec_list);
  return context;
}

void BaseFuncGraphEvaluator::EnterStackFrame(const AnalysisEnginePtr &engine, const StackFramePtr &current_stack_frame,
                                             const StackFramePtr &new_stack_frame) {
  // Enter new func graph.
  auto &current_node = current_stack_frame->CurrentNode();
  auto current_context = current_stack_frame->current_context();
  AnfNodeConfigPtr call_conf = engine->MakeConfig(current_node, current_context);
  auto evaluator = new_stack_frame->evaluator();
  MS_EXCEPTION_IF_NULL(evaluator);
  trace::TraceGraphEvalEnter(evaluator, call_conf);

  // Increase & Check the func graph call depth.
  engine->IncreaseFunctionCallDepth();
  engine->IncreaseStackFrameDepth();
  if (engine->function_call_depth() - engine->stack_frame_depth() >
      MsContext::GetInstance()->get_param<uint32_t>(MS_CTX_MAX_CALL_DEPTH)) {
    MS_LOG(EXCEPTION) << "Exceed function call depth limit "
                      << MsContext::GetInstance()->get_param<uint32_t>(MS_CTX_MAX_CALL_DEPTH)
                      << ", (function call depth: " << engine->function_call_depth()
                      << ", simulate call depth: " << engine->stack_frame_depth()
                      << "), please call 'context.set_context(max_call_depth=value)' to adjust this value.";
  }
  MS_LOG(DEBUG) << evaluator << "(" << evaluator->type_name() << "/" << evaluator->ToString()
                << "), enter, function call depth: " << engine->function_call_depth() << " - "
                << engine->stack_frame_depth();
}

void BaseFuncGraphEvaluator::LeaveStackFrame(const AnalysisEnginePtr &engine,
                                             const StackFramePtr &current_stack_frame) {
  // Leave current func graph.
  auto evaluator = current_stack_frame->evaluator();
  MS_EXCEPTION_IF_NULL(evaluator);
  trace::TraceGraphEvalLeave(evaluator);

  // Decrease the func graph call depth.
  engine->DecreaseFunctionCallDepth();
  engine->DecreaseStackFrameDepth();
  MS_LOG(DEBUG) << evaluator << "(" << evaluator->type_name() << "/" << evaluator->ToString()
                << "), leave, function call depth: " << engine->function_call_depth() << " - "
                << engine->stack_frame_depth();
}

// Start running stack frames in a Evaluator.
AbstractBasePtr BaseFuncGraphEvaluator::LaunchStackFrame(const AnalysisEnginePtr &engine, const FuncGraphPtr &fg) {
  EvalResultPtr eval_result = nullptr;
  AbstractBasePtr res_base = nullptr;
  std::stack<StackFramePtr> stack_frames;
  auto current_stack_frame = std::make_shared<StackFrame>(shared_from_base<Evaluator>(), fg, context_, parent_context_);
  MS_LOG(DEBUG) << "[" << this << "/StackFrame] Start at func graph, " << current_stack_frame;
  stack_frames.push(current_stack_frame);
  while (1) {
    current_stack_frame = stack_frames.top();
    if (current_stack_frame->Done()) {
      MS_EXCEPTION_IF_NULL(res_base);
      MS_LOG(DEBUG) << "[" << this << "/StackFrame] Leave from func graph, " << current_stack_frame;
      stack_frames.pop();
      if (stack_frames.empty()) {
        MS_LOG(DEBUG) << "[" << this << "/StackFrame] Finish at func graph, " << current_stack_frame
                      << ", res_base: " << res_base->ToString();
        break;
      }

      // Overwrite the result if func graph is stub.
      if (current_stack_frame->func_graph()->stub()) {
        eval_result = std::make_shared<EvalResult>(std::make_shared<AbstractUndetermined>(), nullptr);
      }
      // Save func graph eval result for specialize.
      auto evaluator = current_stack_frame->evaluator();
      MS_EXCEPTION_IF_NULL(evaluator);
      EvaluatorCacheMap &evaluator_cache_map = *evaluator->evaluator_cache_map();
      evaluator_cache_map[current_stack_frame->args_abs_list()] = eval_result;

      // Leave current func graph.
      LeaveStackFrame(engine, current_stack_frame);
      // Switch the stack frame.
      current_stack_frame = stack_frames.top();
      MS_LOG(DEBUG) << "[" << this << "/StackFrame] Back to func graph, " << current_stack_frame;
      current_stack_frame->Back(engine, eval_result);
      continue;
    }

    auto new_stack_frame = current_stack_frame->Jump(engine);
    if (new_stack_frame != nullptr) {
      // Enter new func graph.
      EnterStackFrame(engine, current_stack_frame, new_stack_frame);
      // Update current stack frame.
      stack_frames.push(new_stack_frame);
      current_stack_frame = new_stack_frame;
      MS_LOG(DEBUG) << "[" << this << "/StackFrame] Jump to new func graph, " << new_stack_frame;
      continue;
    }

    eval_result = current_stack_frame->Step(engine);
    MS_EXCEPTION_IF_NULL(eval_result);
    res_base = eval_result->abstract();
  }
  return res_base;
}

AbstractBasePtr BaseFuncGraphEvaluator::LaunchRecursiveEval(const AnalysisEnginePtr &engine, const FuncGraphPtr &fg) {
  const AnfNodePtr &func_node = fg->get_return();
  const auto &all_nodes = TopoSort(func_node, SuccIncoming, [&fg](const AnfNodePtr &node) -> IncludeType {
    if (node->isa<ValueNode>() || node->isa<Parameter>()) {
      return EXCLUDE;
    }
    return FOLLOW;
  });
  AbstractBasePtr res_base = nullptr;
  for (const auto &node : all_nodes) {
    AnfNodeConfigPtr node_conf = engine->MakeConfig(node, context_);
    MS_LOG(DEBUG) << "Analysis node begin, func graph: " << fg << "/" << fg->ToString()
                  << ", node_conf: " << node_conf->ToString();
    auto node_eval_result = engine->ObtainEvalResultWithCache(node_conf);
    res_base = node_eval_result->abstract();
    MS_LOG(DEBUG) << "Analysis node end, func graph: " << fg << "/" << fg->ToString()
                  << ", node_conf: " << node_conf->ToString() << ", abstract: " << res_base->ToString();
  }
  MS_EXCEPTION_IF_NULL(res_base);
  return res_base;
}

EvalResultPtr BaseFuncGraphEvaluator::Eval(AnalysisEnginePtr engine, const AbstractBasePtrList &args_abs_list) {
  MS_EXCEPTION_IF_NULL(engine);
  engine->IncreaseFunctionCallDepth();
  if (engine->function_call_depth() - engine->stack_frame_depth() >
      MsContext::GetInstance()->get_param<uint32_t>(MS_CTX_MAX_CALL_DEPTH)) {
    MS_LOG(EXCEPTION) << "Exceed function call depth limit "
                      << MsContext::GetInstance()->get_param<uint32_t>(MS_CTX_MAX_CALL_DEPTH)
                      << ", (function call depth: " << engine->function_call_depth()
                      << ", simulate call depth: " << engine->stack_frame_depth()
                      << "), please call 'context.set_context(max_call_depth=value)' to adjust this value.";
  }
  MS_LOG(DEBUG) << this << "(" << type_name() << "/" << ToString()
                << "), enter, function call depth: " << engine->function_call_depth() << " - "
                << engine->stack_frame_depth();

  // Initialize evaluator starter with args_abs_list.
  FuncGraphPtr fg = GetFuncGraph(engine, args_abs_list);
  MS_EXCEPTION_IF_NULL(fg);
  std::size_t nargs = fg->parameters().size();
  if (args_abs_list.size() != nargs) {
    MS_EXCEPTION(TypeError) << "For function " << fg->ToString() << ", the number of parameters of this function is "
                            << fg->parameters().size() << ", but the number of provided arguments is "
                            << args_abs_list.size() << ".";
  }
  MS_EXCEPTION_IF_NULL(parent_context_);
  context_ = parent_context_->NewFuncGraphContext(fg, args_abs_list);
  const auto &parameters = fg->parameters();
  for (size_t i = 0; i < nargs; i++) {
    const auto &arg = args_abs_list[i];
    const auto &node = parameters[i];
    AnfNodeConfigPtr conf = engine->MakeConfig(node, context_);
    engine->SaveEvalResultInCache(conf, std::make_shared<EvalResult>(arg, nullptr));
  }
  MS_LOG(DEBUG) << "Analysis FuncGraph begin, func graph: " << fg << "/" << fg->ToString()
                << ", context: " << context_->ToString() << ", return node: " << fg->get_return()->DebugString()
                << ", parent: " << (parent_context_->func_graph() ? parent_context_->func_graph()->ToString() : "NULL")
                << ", current function call depth: " << engine->function_call_depth();
  AbstractBasePtr res_base = nullptr;
  if (engine->enable_recursive_eval()) {
    res_base = LaunchRecursiveEval(engine, fg);
  } else {
    res_base = LaunchStackFrame(engine, fg);
  }

  MS_EXCEPTION_IF_NULL(res_base);
  MS_LOG(DEBUG) << "Analysis FuncGraph end, " << fg << "/" << fg->ToString()
                << ", evaluated abstract: " << res_base->ToString() << ", is stub: " << fg->stub();
  if (fg->stub()) {
    res_base = std::make_shared<AbstractUndetermined>();
  }

  engine->DecreaseFunctionCallDepth();
  MS_LOG(DEBUG) << this << "(" << type_name() << "/" << ToString()
                << "), leave, function call depth: " << engine->function_call_depth() << " - "
                << engine->stack_frame_depth();
  return std::make_shared<EvalResult>(res_base, nullptr);
}

AbstractBasePtrList FuncGraphEvaluator::NormalizeArgs(const AbstractBasePtrList &args_spec_list) const {
  MS_EXCEPTION_IF_NULL(func_graph_);
  if (func_graph_->has_flag(FUNC_GRAPH_FLAG_IGNORE_VALUES)) {
    AbstractBasePtrList broaded_list;
    (void)std::transform(args_spec_list.begin(), args_spec_list.end(), std::back_inserter(broaded_list),
                         [](const AbstractBasePtr &arg) -> AbstractBasePtr {
                           MS_EXCEPTION_IF_NULL(arg);
                           if (arg->isa<AbstractScalar>()) {
                             auto config = abstract::AbstractBase::kBroadenScalarParameterOnly;
                             return arg->Broaden(config);
                           } else if (arg->GetValueTrack() != kAnyValue) {
                             return arg->Broaden();
                           }
                           return arg;
                         });
    if (func_graph_->joined_shapes_.size() == broaded_list.size()) {
      for (size_t i = 0; i < broaded_list.size(); ++i) {
        broaded_list[i]->set_shape(func_graph_->joined_shapes_[i]);
      }
    }

    MS_LOG(DEBUG) << func_graph_->ToString() << " original: " << mindspore::ToString(args_spec_list)
                  << ", broaded: " << mindspore::ToString(broaded_list);
    return broaded_list;
  }
  return args_spec_list;
}

AbstractBasePtrList FuncGraphEvaluator::BroadenUndeterminedArgs(const AbstractBasePtrList &args_spec_list) {
  MS_EXCEPTION_IF_NULL(func_graph_);
  if (func_graph_->has_flag(FUNC_GRAPH_FLAG_IGNORE_VALUES)) {
    return args_spec_list;
  }
  if (func_graph_->has_flag(kFuncGraphFlagUndetermined)) {
    if (parent_context_) {
      MS_LOG(DEBUG) << "Undeterminate FuncGraphEvaluator " << ToString()
                    << ", context: " << parent_context_->ToString();
      auto last_context = parent_context_->FindParentContext(func_graph_);
      if (last_context && last_context->func_graph() == func_graph_) {
        MS_LOG(DEBUG) << "Find last eval context: " << last_context->ToString();
        MS_LOG(DEBUG) << "Current eval args: " << ::mindspore::ToString(args_spec_list);
        MS_LOG(DEBUG) << "Last eval args: " << ::mindspore::ToString(last_context->args_spec_list());
        // Join the last eval arguments and current arguments to check if there are loop variant.
        auto joined_args_spec_list_1 = AbstractJoin(args_spec_list, last_context->args_spec_list());
        MS_LOG(DEBUG) << "Joined args: " << ::mindspore::ToString(joined_args_spec_list_1);
        // If there is loop variant, all arguments need to be broaden to avoid wrong constant propagation.
        if (!(joined_args_spec_list_1 == args_spec_list)) {
          func_graph_->set_flag(FUNC_GRAPH_FLAG_IGNORE_VALUES, true);
          func_graph_->joined_shapes_.clear();
          std::transform(joined_args_spec_list_1.begin(), joined_args_spec_list_1.end(),
                         std::back_inserter(func_graph_->joined_shapes_),
                         [](const AbstractBasePtr &arg_spec) { return arg_spec->GetShapeTrack(); });
          joined_args_spec_list_1 = NormalizeArgs(joined_args_spec_list_1);
          MS_LOG(DEBUG) << "Set " << func_graph_->ToString() << " with IGNORE_VALUES flag.";
        }
        return joined_args_spec_list_1;
      }
    }
    if (trace_.size() != 0) {
      MS_LOG(DEBUG) << "Current eval args: " << ::mindspore::ToString(args_spec_list);
      MS_LOG(DEBUG) << "Last eval args: " << ::mindspore::ToString(trace_.back());
      // Join the last eval arguments and current arguments to check if there are loop variant.
      auto joined_args_spec_list_2 = AbstractJoin(args_spec_list, trace_.back());
      // If there is loop variant, all arguments need to be broaden to avoid wrong constant propagation.
      if (!(joined_args_spec_list_2 == args_spec_list)) {
        trace_.push_back(joined_args_spec_list_2);
        func_graph_->set_flag(FUNC_GRAPH_FLAG_IGNORE_VALUES, true);
        func_graph_->joined_shapes_.clear();
        std::transform(joined_args_spec_list_2.begin(), joined_args_spec_list_2.end(),
                       std::back_inserter(func_graph_->joined_shapes_),
                       [](const AbstractBasePtr &arg_spec) { return arg_spec->GetShapeTrack(); });
        joined_args_spec_list_2 = NormalizeArgs(joined_args_spec_list_2);
        MS_LOG(DEBUG) << "Set " << func_graph_->ToString() << " with IGNORE_VALUES flag.";
      }
      MS_LOG(DEBUG) << "Joined eval args: " << ::mindspore::ToString(joined_args_spec_list_2);
      return joined_args_spec_list_2;
    } else {
      trace_.push_back(args_spec_list);
    }
  }
  return args_spec_list;
}

FuncGraphPtr FuncGraphEvaluator::GetFuncGraph(AnalysisEnginePtr engine, const AbstractBasePtrList &args_spec_list) {
  auto iter = func_graph_cache_.find(args_spec_list);
  FuncGraphPtr ret = nullptr;
  if (iter == func_graph_cache_.end()) {
    auto fg = func_graph();
    MS_EXCEPTION_IF_NULL(fg);
    TraceGuard guard(std::make_shared<TraceEvaluatorGenGraph>(fg->debug_info()));
    FuncGraphPtr generated_graph = fg->GenerateGraph(args_spec_list);
    func_graph_cache_[args_spec_list] = generated_graph;
    MS_EXCEPTION_IF_NULL(engine);
    engine->func_graph_manager()->AddFuncGraph(generated_graph);
    ret = generated_graph;
  } else {
    ret = iter->second;
  }

  // For the top graph, if it is replaced by generated graph, update the top graph to the new one.
  if (parse::Parser::GetTopFuncGraph() == func_graph()) {
    if (ret != func_graph()) {
      parse::Parser::UpdateTopFuncGraph(ret);
    }
  }
  return ret;
}

FuncGraphPtr MetaFuncGraphEvaluator::GetFuncGraph(AnalysisEnginePtr engine, const AbstractBasePtrList &args_spec_list) {
  auto iter = func_graph_cache_.find(args_spec_list);
  if (iter != func_graph_cache_.end()) {
    return iter->second;
  }

  MS_EXCEPTION_IF_NULL(meta_func_graph_);
  FuncGraphPtr generated_func_graph = nullptr;
  if (this->bound_node() != nullptr) {
    TraceGuard trace_guard(std::make_shared<TraceGenMetaFuncGraph>(bound_node()->debug_info()));
    generated_func_graph = meta_func_graph_->GenerateFuncGraph(args_spec_list);
  } else {
    generated_func_graph = meta_func_graph_->GenerateFuncGraph(args_spec_list);
  }

  FuncGraphPtr cloned_func_graph = BasicClone(generated_func_graph);
  func_graph_cache_[args_spec_list] = cloned_func_graph;
  MS_EXCEPTION_IF_NULL(engine);
  engine->func_graph_manager()->AddFuncGraph(cloned_func_graph);
  return cloned_func_graph;
}

EvalResultPtr Evaluator::Run(AnalysisEnginePtr engine, const ConfigPtrList &args_conf_list, AnfNodeConfigPtr out_conf) {
  const std::string &evaluator_name = ToString();

  AbstractBasePtrList args_spec_list;
  (void)std::transform(args_conf_list.begin(), args_conf_list.end(), std::back_inserter(args_spec_list),
                       [](const ConfigPtr &conf) -> AbstractBasePtr {
                         MS_EXCEPTION_IF_NULL(conf);
                         return conf->ObtainEvalResult()->abstract();
                       });
  args_spec_list = NormalizeArgs(args_spec_list);
  args_spec_list = BroadenUndeterminedArgs(args_spec_list);
  trace::TraceGraphEvalEnter(shared_from_base<Evaluator>(), out_conf);
  MS_LOG(DEBUG) << EvalEntryLogging(shared_from_base<Evaluator>(), args_spec_list, out_conf);
  MS_EXCEPTION_IF_NULL(evaluator_cache_map_);
  auto iter = evaluator_cache_map_->find(args_spec_list);
  if (iter == evaluator_cache_map_->end()) {
    MS_LOG(DEBUG) << evaluator_name << " cache miss, call Eval().";
    EvalResultPtr ret = Eval(engine, args_spec_list);
    if (ret->abstract() == nullptr) {
      EvalFailLogging(shared_from_base<Evaluator>(), args_spec_list, out_conf);
      MS_LOG(EXCEPTION) << "Evaluator " << evaluator_name << " result is nullptr.";
    }
    MS_LOG(DEBUG) << evaluator_name << " set cache. return: " << ret->abstract()->ToString() << ".";
    (*evaluator_cache_map_)[args_spec_list] = ret;
    trace::TraceGraphEvalLeave(shared_from_base<Evaluator>());
    return ret;
  } else {
    MS_EXCEPTION_IF_NULL(iter->second);
    MS_EXCEPTION_IF_NULL(iter->second->abstract());
    MS_LOG(DEBUG) << evaluator_name << " cache hit. return: " << iter->second->abstract()->ToString() << ".";
    trace::TraceGraphEvalLeave(shared_from_base<Evaluator>());
    return iter->second;
  }
}

EvalResultPtr TrivialPrimEvaluator::Run(AnalysisEnginePtr engine, const ConfigPtrList &args_conf_list,
                                        AnfNodeConfigPtr) {
  AbstractBasePtrList args_spec_list;
  auto is_py_eval = (identifier_ == "PythonPrimEvaluator");
  (void)std::transform(args_conf_list.begin(), args_conf_list.end(), std::back_inserter(args_spec_list),
                       [is_py_eval](const ConfigPtr &conf) -> AbstractBasePtr {
                         MS_EXCEPTION_IF_NULL(conf);
                         auto abstract = conf->ObtainEvalResult()->abstract();
                         // broaden the ref_key, while infer python prim for cache
                         if (is_py_eval && abstract->isa<AbstractRef>()) {
                           auto abs_ref = abstract->cast<AbstractRefPtr>();
                           abstract = std::make_shared<AbstractRef>(abs_ref->ref_key()->Broaden(), abs_ref);
                         }
                         return abstract;
                       });
  EvalResultPtr ret = EvalPrim(engine, args_spec_list);
  return ret;
}

EvalResultPtr TransitionPrimEvaluator::Run(AnalysisEnginePtr engine, const ConfigPtrList &args_conf_list,
                                           AnfNodeConfigPtr out_conf) {
  AbstractBasePtrList args_spec_list;
  (void)std::transform(args_conf_list.begin(), args_conf_list.end(), std::back_inserter(args_spec_list),
                       [](const ConfigPtr &conf) -> AbstractBasePtr {
                         MS_EXCEPTION_IF_NULL(conf);
                         return conf->ObtainEvalResult()->abstract();
                       });
  if (args_conf_list.size() == 0) {
    MS_LOG(EXCEPTION) << "Size should greater than 0";
  }
  EvalResultPtr ret = EvalPrim(engine, args_spec_list, args_conf_list[0], out_conf);
  // No need to cache.
  return ret;
}

EvalResultPtr SymbolicPrimEvaluator::Run(AnalysisEnginePtr, const ConfigPtrList &args_conf_list, AnfNodeConfigPtr) {
  EvalResultPtr ret = EvalPrim(args_conf_list);
  return ret;
}

EvalResultPtr TrackedEvaluator::Run(AnalysisEnginePtr engine, const ConfigPtrList &args_conf_list,
                                    AnfNodeConfigPtr out_conf) {
  AbstractBasePtrList args_spec_list;
  (void)std::transform(args_conf_list.begin(), args_conf_list.end(), std::back_inserter(args_spec_list),
                       [](const ConfigPtr &conf) -> AbstractBasePtr {
                         MS_EXCEPTION_IF_NULL(conf);
                         return conf->ObtainEvalResult()->abstract();
                       });
  EvalResultPtr ret = sub_evaluator_->Run(engine, args_conf_list, out_conf);
  // Don't lookup from cache, as different out_conf with same node but different context
  // may add different entry to anfnode_config_map_, like getattr primitive.
  (*evaluator_cache_map_)[args_spec_list] = ret;
  return ret;
}

EvalResultPtr PartialAppEvaluator::Run(AnalysisEnginePtr engine, const ConfigPtrList &args_conf_list,
                                       AnfNodeConfigPtr out_conf) {
  AbstractBasePtrList args_spec_list;
  (void)std::transform(args_conf_list.begin(), args_conf_list.end(), std::back_inserter(args_spec_list),
                       [](const ConfigPtr &conf) -> AbstractBasePtr {
                         MS_EXCEPTION_IF_NULL(conf);
                         return conf->ObtainEvalResult()->abstract();
                       });
  MS_EXCEPTION_IF_NULL(evaluator_cache_map_);
  auto iter = evaluator_cache_map_->find(args_spec_list);
  if (iter != evaluator_cache_map_->end()) {
    return iter->second;
  }

  ConfigPtrList partial_args_conf_list;
  // Join arguments in partial and the rest arguments from args_conf_list.
  (void)std::transform(args_spec_list_.begin(), args_spec_list_.end(), std::back_inserter(partial_args_conf_list),
                       [](const AbstractBasePtr &arg) -> ConfigPtr { return std::make_shared<VirtualConfig>(arg); });

  (void)std::transform(args_spec_list.begin(), args_spec_list.end(), std::back_inserter(partial_args_conf_list),
                       [](const AbstractBasePtr &arg) -> ConfigPtr { return std::make_shared<VirtualConfig>(arg); });
  EvalResultPtr ret = evaluator_->Run(engine, partial_args_conf_list, out_conf);

  (*evaluator_cache_map_)[args_spec_list] = ret;
  return ret;
}

EvalResultPtr JEvaluator::Run(AnalysisEnginePtr engine, const ConfigPtrList &args_conf_list, AnfNodeConfigPtr) {
  AbstractBasePtrList args_spec_list;
  (void)std::transform(args_conf_list.begin(), args_conf_list.end(), std::back_inserter(args_spec_list),
                       [](const ConfigPtr &conf) -> AbstractBasePtr {
                         MS_EXCEPTION_IF_NULL(conf);
                         return conf->ObtainEvalResult()->abstract();
                       });
  MS_EXCEPTION_IF_NULL(evaluator_cache_map_);
  auto iter = evaluator_cache_map_->find(args_spec_list);
  if (iter != evaluator_cache_map_->end()) {
    return iter->second;
  }

  // Call the original evaluator, get the result: y = f(x)
  EvalResultPtr result = evaluator_->Run(engine, args_conf_list, nullptr);
  // Build a virtual function: bprop_f which use sense of y as input, return sense of function free variable and input
  // parameters. (sense_f, sense_x, ...)(*bpro_f) (sense_y)
  AbstractBasePtrList bparams;
  bparams.push_back(SensitivityTransform(orig_func_));
  auto context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context);
  bool enable_sparse = context->get_param<bool>(MS_CTX_ENABLE_SPARSE);
  (void)std::transform(args_spec_list.begin(), args_spec_list.end(), std::back_inserter(bparams),
                       [&enable_sparse](const AbstractBasePtr &arg_spec) -> AbstractBasePtr {
                         if (enable_sparse && arg_spec->isa<AbstractTensor>()) {
                           return std::make_shared<AbstractUndetermined>();
                         }
                         return SensitivityTransform(arg_spec);
                       });
  AbstractBasePtr bparams_final = std::make_shared<AbstractTuple>(bparams);
  AbstractFunctionPtr bprop =
    std::make_shared<VirtualAbstractClosure>(SensitivityTransform(result->abstract()), bparams_final);

  // J(f)(J(x)) return a tuple (y, bprop_f)
  AbstractBasePtrList jargs = {result->abstract(), bprop};
  AbstractBasePtr jtuple = std::make_shared<AbstractTuple>(jargs);
  auto infer_reuslt = std::make_shared<EvalResult>(jtuple, std::make_shared<AttrValueMap>());
  (*evaluator_cache_map_)[args_spec_list] = infer_reuslt;
  return infer_reuslt;
}

EvalResultPtr VirtualEvaluator::Eval(AnalysisEnginePtr, const AbstractBasePtrList &args_spec_list) {
  if (args_spec_list.size() != args_spec_list_.size()) {
    MS_LOG(EXCEPTION) << "Arguments mismatch, parameters no: " << args_spec_list_.size()
                      << ", arguments no: " << args_spec_list.size();
  }
  // Check each parameter and argument match;
  for (std::size_t i = 0; i < args_spec_list.size(); i++) {
    MS_EXCEPTION_IF_NULL(args_spec_list[i]);
    (void)args_spec_list[i]->Join(args_spec_list_[i]);
  }
  return std::make_shared<EvalResult>(output_, std::make_shared<AttrValueMap>());
}
}  // namespace abstract
}  // namespace mindspore
