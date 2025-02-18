/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file src/contrib/msc/core/ir/graph_builder.cc
 */

#include "graph_builder.h"

#include <set>

namespace tvm {
namespace contrib {
namespace msc {

const std::string GetScalarStr(const runtime::NDArray& data, int float_precision) {
  std::string scalar_str;
  if (data->dtype.code == kDLFloat) {
    const float val = ExprUtils::GetScalar<float>(data);
    std::stringstream stream;
    stream << std::fixed << std::setprecision(float_precision) << val;
    scalar_str = stream.str();
  } else {
    const int val = ExprUtils::GetScalar<int>(data);
    scalar_str = std::to_string(val);
  }
  return scalar_str;
}

void RelaxFuncAttrGetter::VisitExpr_(const relax::CallNode* op) {
  if (op->attrs.defined()) {
    Map<String, String> attrs;
    AttrGetter getter(&attrs);
    const_cast<BaseAttrsNode*>(op->attrs.get())->VisitAttrs(&getter);
    for (const auto& pair : attrs) {
      if (attrs_.count(pair.first)) {
        int cnt = 1;
        String rep_key = pair.first;
        while (attrs_.count(rep_key + "_" + std::to_string(cnt))) {
          cnt++;
        }
        attrs_.Set(pair.first + "_" + std::to_string(cnt), pair.second);
      } else {
        attrs_.Set(pair.first, pair.second);
      }
    }
  }
}

void RelaxFuncAttrGetter::VisitExpr_(const relax::TupleGetItemNode* op) {
  attrs_.Set("index", std::to_string(op->index));
}

void RelaxFuncValueGetter::VisitExpr_(const relax::CallNode* op) {
  for (const auto& arg : op->args) {
    if (const auto* s_node = arg.as<relax::PrimValueNode>()) {
      values_.push_back(StringUtils::ToString(s_node->value));
    }
  }
}

void RelaxFuncParamsFinder::VisitBinding_(const relax::VarBindingNode* binding,
                                          const relax::FunctionNode* val) {
  local_funcs_.Set(binding->var, GetRef<relax::Function>(val));
}

void RelaxFuncParamsFinder::VisitExpr_(const relax::CallNode* call_node) {
  RelaxExprVisitor::VisitExpr_(call_node);
  relax::Function func;
  if (const auto* v_node = call_node->op.as<GlobalVarNode>()) {
    func = Downcast<relax::Function>(ref_module_->Lookup(v_node->name_hint));
  } else if (call_node->op->IsInstance<relax::VarNode>()) {
    ICHECK(local_funcs_.count(call_node->op)) << "Can not find local func " << call_node->op;
    func = local_funcs_[call_node->op];
  }
  if (func.defined()) {
    for (size_t i = 0; i < call_node->args.size(); i++) {
      const auto& arg = call_node->args[i];
      if (arg->IsInstance<relax::VarNode>() && params_.count(Downcast<relax::Var>(arg))) {
        params_.Set(func->params[i], params_[Downcast<relax::Var>(arg)]);
      } else {
        params_.Set(func->params[i], arg);
      }
    }
  }
}

const MSCGraph RelaxGraphBuilder::Build(const relax::Function& func) {
  // Add input nodes and record inputs;
  Array<String> input_names, output_names;
  std::set<String> added_inputs;
  for (const auto& p : func->params) {
    if (expr_tensor_map_.count(p)) {
      continue;
    }
    if (func_params_.count(p) && func_params_[p]->IsInstance<relax::TupleNode>()) {
      const auto& tuple = Downcast<relax::Tuple>(func_params_[p]);
      Array<String> tuple_names;
      for (const auto& f : tuple->fields) {
        if (expr_tensor_map_.count(f)) {
          LOG_INFO << "Replica tuple input " << f;
        } else if (const auto* f_node = f.as<relax::VarNode>()) {
          AddNode(f, NullOpt, f_node->name_hint());
        } else {
          LOG_FATAL << "Unexpected tuple input " << f << "(" << f->GetTypeKey() << ")";
        }
        ICHECK(expr_tensor_map_.count(f)) << "Can not find func param from tuple " << f;
        for (const auto& name : expr_tensor_map_[f]) {
          tuple_names.push_back(name);
        }
      }
      expr_tensor_map_.Set(p, tuple_names);
    } else {
      AddNode(p, NullOpt, p->name_hint());
    }
    ICHECK(expr_tensor_map_.count(p)) << "Can not find func param " << p;
    for (const auto& name : expr_tensor_map_[p]) {
      if (!added_inputs.count(name)) {
        input_names.push_back(name);
        added_inputs.insert(name);
      }
    }
  }
  VisitExpr(func);
  if (const auto* b_node = func->body.as<relax::SeqExprNode>()) {
    ICHECK(expr_tensor_map_.count(b_node->body)) << "Can not find seqexpr body " << b_node->body;
    output_names = expr_tensor_map_[b_node->body];
  } else {
    LOG(FATAL) << "Function body should be SeqExpr, get " << func->body;
  }
  // remove const nodes as weights
  Array<MSCJoint> valid_nodes;
  std::set<String> ignore_inputs;
  for (const auto& n : nodes_) {
    if (weights_.count(n->name) || ignore_nodes_.count(n->name)) {
      for (const auto& o : n->outputs) {
        ignore_inputs.insert(o->name);
      }
    } else {
      n->index = valid_nodes.size();
      valid_nodes.push_back(n);
      if (n->optype != "input") {
        for (const auto& o : n->outputs) {
          ignore_inputs.insert(o->name);
        }
      }
    }
  }
  // remove uselese inputs
  Array<String> valid_inputs;
  for (const auto& i : input_names) {
    if (!ignore_inputs.count(i)) {
      valid_inputs.push_back(i);
    }
  }
  // build graph
  const auto& graph = MSCGraph(name_, valid_nodes, valid_inputs, output_names);
  // set inputs and outputs alias
  if (config_.input_aliases.size() == valid_inputs.size()) {
    for (size_t i = 0; i < valid_inputs.size(); i++) {
      graph->FindTensor(valid_inputs[i])->alias = config_.input_aliases[i];
    }
  } else {
    for (size_t i = 0; i < valid_inputs.size(); i++) {
      graph->FindTensor(valid_inputs[i])->alias = graph->FindProducer(valid_inputs[i])->name;
    }
  }
  if (config_.output_aliases.size() == output_names.size()) {
    for (size_t i = 0; i < output_names.size(); i++) {
      graph->FindTensor(output_names[i])->alias = config_.output_aliases[i];
    }
  } else {
    for (size_t i = 0; i < output_names.size(); i++) {
      const auto& output = graph->FindTensor(output_names[i]);
      if (output->alias.size() > 0) {
        continue;
      }
      const auto& producer = graph->FindProducer(output_names[i]);
      output->alias = producer->outputs.size() == 1
                          ? producer->name
                          : StringUtils::Replace(output_names[i], ":", "_");
    }
  }
  return graph;
}

const MSCJoint RelaxGraphBuilder::AddNode(const Expr& expr, const Optional<Expr>& binding_var,
                                          const String& name) {
  String node_name = name.size() > 0 ? name : SpanUtils::GetAttr(expr->span, "name");
  const auto& shared_ref = SpanUtils::GetAttr(expr->span, "shared_ref");

  // Get optype
  String optype;
  if (expr->IsInstance<relax::VarNode>()) {
    if (func_params_.count(expr) && func_params_[expr]->IsInstance<relax::ConstantNode>()) {
      optype = "constant";
      node_name = SpanUtils::GetAttr(func_params_[expr]->span, "name");
    } else {
      optype = "input";
    }
  } else if (expr->IsInstance<relax::ConstantNode>()) {
    optype = "constant";
  } else if (expr->IsInstance<relax::ShapeExprNode>()) {
    optype = "shape";
  } else if (expr->IsInstance<relax::TupleGetItemNode>()) {
    optype = "get_item";
  } else if (expr->IsInstance<relax::TupleNode>()) {
    optype = "tuple";
  } else if (const auto* call_node = expr.as<relax::CallNode>()) {
    if (const auto* op_node = call_node->op.as<OpNode>()) {
      optype = StringUtils::Replace(op_node->name, "relax.", "");
    } else if (const auto* v_node = call_node->op.as<GlobalVarNode>()) {
      const auto& func = Downcast<relax::Function>(ref_module_->Lookup(v_node->name_hint));
      const auto& name_opt = func->GetAttr<runtime::String>(relax::attr::kComposite);
      ICHECK(name_opt.defined()) << "Unexpected global func without composite";
      optype = name_opt.value();
    } else if (call_node->op->IsInstance<relax::VarNode>()) {
      ICHECK(target_funcs_.count(call_node->op)) << "Can not find target func: " << call_node->op;
      const auto& func = target_funcs_[call_node->op];
      const auto& name_opt = func->GetAttr<runtime::String>(relax::attr::kComposite);
      optype = StringUtils::Replace(name_opt.value(), config_.target + ".", "");
    } else if (const auto* f_node = call_node->op.as<relax::FunctionNode>()) {
      const auto& name_opt = f_node->GetAttr<runtime::String>(relax::attr::kComposite);
      ICHECK(name_opt.defined()) << "Unexpected func without composite";
      optype = name_opt.value();
    } else {
      optype = "unknown_op";
    }
  } else {
    optype = "unknown_expr";
  }

  // Extract attributes
  Map<String, String> attrs;
  if (const auto* call_node = expr.as<relax::CallNode>()) {
    if (const auto* v_node = call_node->op.as<GlobalVarNode>()) {
      const auto& func = Downcast<relax::Function>(ref_module_->Lookup(v_node->name_hint));
      attrs = RelaxFuncAttrGetter().GetAttrs(func);
    } else if (call_node->op->IsInstance<relax::VarNode>()) {
      ICHECK(target_funcs_.count(call_node->op)) << "Can not find target func: " << call_node->op;
      attrs = RelaxFuncAttrGetter().GetAttrs(target_funcs_[call_node->op]);
    } else if (call_node->op->IsInstance<relax::FunctionNode>()) {
      attrs = RelaxFuncAttrGetter().GetAttrs(call_node->op);
    } else if (call_node->attrs.defined()) {
      AttrGetter getter(&attrs);
      const_cast<BaseAttrsNode*>(call_node->attrs.get())->VisitAttrs(&getter);
    }
  } else if (const auto* const_node = expr.as<relax::ConstantNode>()) {
    if (const_node->is_scalar()) {
      attrs.Set("scalar", GetScalarStr(const_node->data, config_.float_precision));
    }
  } else if (const auto* shape_node = expr.as<relax::ShapeExprNode>()) {
    attrs.Set("shape", StringUtils::ToString(shape_node->values));
  } else if (const auto* get_node = expr.as<relax::TupleGetItemNode>()) {
    attrs.Set("index", std::to_string(get_node->index));
  }

  // Get scope
  Array<String> scope;
  if (optype != "input" && optype != "constant") {
    scope = StringUtils::Split(scope_name_, ".");
  }
  // Build inputs and weights
  Array<String> input_names;
  Map<String, MSCTensor> node_weights;
  if (const auto* call_node = expr.as<relax::CallNode>()) {
    Array<String> prim_values;
    if (call_node->op->IsInstance<relax::VarNode>()) {
      ICHECK(target_funcs_.count(call_node->op)) << "Can not find target func: " << call_node->op;
      prim_values = RelaxFuncValueGetter().GetValues(target_funcs_[call_node->op]);
    }
    const auto& input_types =
        ExprUtils::GetInputTypes(optype, call_node->args.size() + prim_values.size(), true);
    for (size_t i = 0; i < call_node->args.size(); i++) {
      const auto& arg = call_node->args[i];
      if (const auto* s_node = arg.as<relax::ShapeExprNode>()) {
        attrs.Set(input_types[i], StringUtils::ToString(s_node->values));
        continue;
      }
      if (func_params_.count(arg) && func_params_[arg]->IsInstance<relax::ShapeExprNode>()) {
        const auto* s_node = func_params_[arg].as<relax::ShapeExprNode>();
        attrs.Set(input_types[i], StringUtils::ToString(s_node->values));
        ignore_nodes_.insert(Downcast<relax::Var>(arg)->name_hint());
        continue;
      }
      if (const auto* s_node = arg.as<relax::PrimValueNode>()) {
        ICHECK(input_types[i] != "input") << i << " th PrimValue of " << optype
                                          << " should has special type, get " << input_types;
        attrs.Set(input_types[i], StringUtils::ToString(s_node->value));
        continue;
      }
      Array<String> arg_names;
      if (expr_tensor_map_.count(arg)) {
        arg_names = expr_tensor_map_[arg];
      } else if (const auto* tuple_node = arg.as<relax::TupleNode>()) {
        for (const auto& f : tuple_node->fields) {
          ICHECK(expr_tensor_map_.count(f)) << "Can not find tuple field " << f;
          for (const auto& in_name : expr_tensor_map_[f]) {
            arg_names.push_back(in_name);
          }
        }
      }
      String weight_name;
      if (input_types[i] != "input" && arg->IsInstance<relax::ConstantNode>()) {
        weight_name = SpanUtils::GetAttr(arg->span, "name");
      } else if (input_types[i] != "input" && func_params_.count(arg) &&
                 func_params_[arg]->IsInstance<relax::ConstantNode>()) {
        weight_name = SpanUtils::GetAttr(func_params_[arg]->span, "name");
        ignore_nodes_.insert(Downcast<relax::Var>(arg)->name_hint());
      }
      // set weights or inputs
      if (weight_name.size() > 0) {
        const auto& t_name = arg_names[0];
        const auto& pair = tensor_input_map_[t_name];
        const auto& producer = Downcast<MSCJoint>(pair.first);
        if (!weights_.count(weight_name)) {
          const auto& ref = producer->OutputAt(pair.second);
          MSCTensor weight;
          if (input_types[i] == "bias") {
            weight = MSCTensor(weight_name, ref->dtype, "O", Array<Integer>{ref->GetSize()});
          } else if (input_types[i] == "weight" &&
                     (optype == "msc.linear" || optype == "msc.linear_bias")) {
            if (ref->layout.name() == "IO") {
              String valid_layout = ref->layout[1].name() + ref->layout[0].name();
              const auto& valid_shape = Array<Integer>({ref->shape[1], ref->shape[0]});
              weight = MSCTensor(weight_name, ref->dtype, valid_layout, valid_shape);
            } else {
              weight = MSCTensor(weight_name, ref->dtype, ref->layout.name(), ref->shape);
            }
          } else {
            weight = MSCTensor(weight_name, ref->dtype, ref->layout.name(), ref->shape);
          }
          weights_.Set(weight_name, weight);
        }
        if (producer->HasAttr("scalar")) {
          attrs.Set(input_types[i], producer->GetTypeAttr<std::string>("scalar"));
        }
        node_weights.Set(input_types[i], weights_[weight_name]);
      } else {
        for (const auto& in_name : arg_names) {
          input_names.push_back(in_name);
        }
      }
    }
    // add prim values to attributes
    for (size_t i = call_node->args.size(); i < input_types.size(); i++) {
      attrs.Set(input_types[i], prim_values[i - call_node->args.size()]);
    }
  } else if (const auto* tuple_node = expr.as<relax::TupleNode>()) {
    for (const auto& f : tuple_node->fields) {
      ICHECK(expr_tensor_map_.count(f)) << "Can not find tuple field " << f;
      for (const auto& in_name : expr_tensor_map_[f]) {
        input_names.push_back(in_name);
      }
    }
  } else if (const auto* getitem_node = expr.as<relax::TupleGetItemNode>()) {
    ICHECK(expr_tensor_map_.count(getitem_node->tuple))
        << "Can not find tuple " << getitem_node->tuple;
    input_names = expr_tensor_map_[getitem_node->tuple];
  } else if (optype == "constant") {
    const auto& t_info = Downcast<relax::TensorStructInfo>(relax::GetStructInfo(expr));
    const auto& opt_shape = t_info->GetShape();
    ICHECK(opt_shape.defined()) << "Constant shape is not defined";
    const auto& layout = SpanUtils::GetAttr(expr->span, "layout");
    const auto& weight =
        MSCTensor(node_name, t_info->dtype, layout, ArrayUtils::Cast<Integer>(opt_shape.value()));
    node_weights.Set("const", weight);
  }
  std::vector<std::pair<BaseJoint, size_t>> inputs;
  for (const auto& i : input_names) {
    inputs.push_back(tensor_input_map_[i]);
  }

  // Build outputs
  Array<MSCTensor> outputs;
  const auto& layout = SpanUtils::GetAttr(expr->span, "layout");
  const auto& sinfo = relax::GetStructInfo(expr);
  if (const auto* t_info = sinfo.as<relax::TensorStructInfoNode>()) {
    const auto& opt_shape = t_info->GetShape();
    const auto& shape =
        opt_shape.defined() ? ArrayUtils::Cast<Integer>(opt_shape.value()) : Array<Integer>();
    const auto& output =
        MSCTensor(node_name + ":" + std::to_string(0), t_info->dtype, layout, shape);
    outputs.push_back(output);
  } else if (const auto* s_sinfo = sinfo.as<relax::ShapeStructInfoNode>()) {
    Array<Integer> shape{s_sinfo->ndim};
    const auto& output = MSCTensor(node_name + ":" + std::to_string(0),
                                   DataType(runtime::String2DLDataType("int32")), layout, shape);
    outputs.push_back(output);
  } else if (const auto* tuple_sinfo = sinfo.as<relax::TupleStructInfoNode>()) {
    Array<String> layouts = StringUtils::Split(layout, ",");
    if (layouts.size() == 0) {
      layouts = Array<String>(tuple_sinfo->fields.size(), "");
    }
    ICHECK_EQ(layouts.size(), tuple_sinfo->fields.size())
        << "Layout " << layout << " msimatch with fileds size " << tuple_sinfo->fields.size();
    size_t field_size = tuple_sinfo->fields.size();
    if (optype == "nn.batch_norm") {
      field_size = 1;
    }
    for (size_t i = 0; i < field_size; i++) {
      const auto& t_info = Downcast<relax::TensorStructInfo>(tuple_sinfo->fields[i]);
      const auto& opt_shape = t_info->GetShape();
      const auto& shape =
          opt_shape.defined() ? ArrayUtils::Cast<Integer>(opt_shape.value()) : Array<Integer>();
      const auto& output =
          MSCTensor(node_name + ":" + std::to_string(i), t_info->dtype, layouts[i], shape);
      outputs.push_back(output);
    }
  } else {
    LOG(FATAL) << "Unexpected struct info (" << sinfo->GetTypeKey() << ")" << sinfo;
  }

  // Build node
  const auto& node = MSCJoint(nodes_.size(), node_name, shared_ref, optype, attrs, scope, inputs,
                              outputs, node_weights);
  Array<String> output_names;
  for (size_t i = 0; i < outputs.size(); i++) {
    output_names.push_back(outputs[i]->name);
    tensor_input_map_[outputs[i]->name] = std::make_pair(node, i);
  }
  nodes_.push_back(node);
  const auto& ref_expr = binding_var.defined() ? binding_var.value() : expr;
  expr_tensor_map_.Set(ref_expr, output_names);
  return node;
}

void RelaxGraphBuilder::VisitBindingBlock(const relax::BindingBlock& block) {
  scope_name_ = SpanUtils::GetAttr(block->span, "name");
  RelaxExprVisitor::VisitBindingBlock(block);
}

void RelaxGraphBuilder::VisitExpr_(const relax::ConstantNode* op) {
  AddNode(GetRef<relax::Constant>(op));
}

void RelaxGraphBuilder::VisitBinding_(const relax::VarBindingNode* binding,
                                      const relax::ConstantNode* val) {
  const String& name = config_.use_var_name ? binding->var->name_hint() : "";
  AddNode(GetRef<relax::Constant>(val), binding->var, name);
}

void RelaxGraphBuilder::VisitBinding_(const relax::VarBindingNode* binding,
                                      const relax::ShapeExprNode* val) {
  const String& name = config_.use_var_name ? binding->var->name_hint() : "";
  AddNode(GetRef<relax::ShapeExpr>(val), binding->var, name);
}

void RelaxGraphBuilder::VisitBinding_(const relax::VarBindingNode* binding,
                                      const relax::CallNode* call_node) {
  RelaxExprVisitor::VisitBinding_(binding, call_node);
  const String& name = config_.use_var_name ? binding->var->name_hint() : "";
  try {
    AddNode(GetRef<relax::Call>(call_node), binding->var, name);
  } catch (runtime::InternalError& err) {
    LOG(WARNING) << "Failed to add node from " << binding->var << " : " << binding->value
                 << ", reason: " << err.message();
    throw err;
  }
}

void RelaxGraphBuilder::VisitBinding_(const relax::VarBindingNode* binding,
                                      const relax::TupleNode* val) {
  RelaxExprVisitor::VisitBinding_(binding, val);
  const String& name = config_.use_var_name ? binding->var->name_hint() : "";
  AddNode(GetRef<relax::Tuple>(val), binding->var, name);
}

void RelaxGraphBuilder::VisitBinding_(const relax::VarBindingNode* binding,
                                      const relax::TupleGetItemNode* val) {
  RelaxExprVisitor::VisitBinding_(binding, val);
  const String& name = config_.use_var_name ? binding->var->name_hint() : "";
  AddNode(GetRef<relax::TupleGetItem>(val), binding->var, name);
}

void RelaxGraphBuilder::VisitBinding_(const relax::VarBindingNode* binding,
                                      const relax::VarNode* val) {
  RelaxExprVisitor::VisitBinding_(binding, val);
  const auto& output = GetRef<relax::Var>(val);
  ICHECK(expr_tensor_map_.count(output)) << "Can not find var " << output;
  expr_tensor_map_.Set(binding->var, expr_tensor_map_[output]);
}

void RelaxGraphBuilder::VisitBinding_(const relax::VarBindingNode* binding,
                                      const relax::DataflowVarNode* val) {
  RelaxExprVisitor::VisitBinding_(binding, val);
  const auto& output = GetRef<relax::DataflowVar>(val);
  ICHECK(expr_tensor_map_.count(output)) << "Can not find dataflow var " << output;
  expr_tensor_map_.Set(binding->var, expr_tensor_map_[output]);
}

void RelaxGraphBuilder::VisitBinding_(const relax::VarBindingNode* binding,
                                      const relax::FunctionNode* val) {
  const auto& name_opt = val->GetAttr<runtime::String>(relay::attr::kComposite);
  ICHECK(name_opt.defined()) << "Unexpected target func without composite";
  ICHECK(config_.target.size() > 0 && StringUtils::StartsWith(name_opt.value(), config_.target))
      << "Target should be given for target function";
  target_funcs_.Set(binding->var, GetRef<relax::Function>(val));
}

Map<MSCTensor, NDArray> RelaxWeightsExtractor::GetWeights(const relax::Function& func) {
  VisitExpr(func);
  return weights_;
}

void RelaxWeightsExtractor::VisitExpr_(const relax::ConstantNode* op) {
  const auto& name = SpanUtils::GetAttr(op->span, "name");
  const auto& layout = SpanUtils::GetAttr(op->span, "layout");
  const auto& sinfo = relax::GetStructInfo(GetRef<relax::Constant>(op));
  ICHECK(sinfo->IsInstance<relax::TensorStructInfoNode>())
      << "Constant StrcutInfo should be TensorStructInfo";
  const auto& t_info = Downcast<relax::TensorStructInfo>(sinfo);
  const auto& opt_shape = t_info->GetShape();
  const auto& shape =
      opt_shape.defined() ? ArrayUtils::Cast<Integer>(opt_shape.value()) : Array<Integer>();
  const auto& weight = MSCTensor(name, t_info->dtype, layout, shape);
  weights_.Set(weight, op->data);
}

void RelayFuncAttrGetter::VisitExpr_(const relay::CallNode* op) {
  RelayExprVisitor::VisitExpr_(op);
  if (op->attrs.defined()) {
    Map<String, String> attrs;
    AttrGetter getter(&attrs);
    const_cast<BaseAttrsNode*>(op->attrs.get())->VisitAttrs(&getter);
    for (const auto& pair : attrs) {
      if (attrs_.count(pair.first)) {
        int cnt = 1;
        String rep_key = pair.first;
        while (attrs_.count(rep_key + "_" + std::to_string(cnt))) {
          cnt++;
        }
        attrs_.Set(pair.first + "_" + std::to_string(cnt), pair.second);
      } else {
        attrs_.Set(pair.first, pair.second);
      }
    }
  }
}

MSCGraph RelayGraphBuilder::Build(const relay::Function& func) {
  // Add input nodes and record inputs;
  Array<String> input_names, output_names;
  for (const auto& p : func->params) {
    AddNode(p, p->name_hint());
    ICHECK(expr_tensor_map_.count(p)) << "Can not find func param " << p;
    input_names.push_back(expr_tensor_map_[p][0]);
  }
  VisitExpr(func);
  ICHECK(expr_tensor_map_.count(func->body)) << "Can not find func body " << func->body;
  output_names = expr_tensor_map_[func->body];
  // remove const nodes as weights
  Array<MSCJoint> valid_nodes;
  for (const auto& n : nodes_) {
    if (!weights_.count(n->name)) {
      n->index = valid_nodes.size();
      valid_nodes.push_back(n);
    }
  }
  const auto& graph = MSCGraph(name_, valid_nodes, input_names, output_names);
  // set inputs and outputs alias
  if (config_.input_aliases.size() == input_names.size()) {
    for (size_t i = 0; i < input_names.size(); i++) {
      graph->FindTensor(input_names[i])->alias = config_.input_aliases[i];
    }
  } else {
    for (size_t i = 0; i < input_names.size(); i++) {
      graph->FindTensor(input_names[i])->alias = graph->FindProducer(input_names[i])->name;
    }
  }
  if (config_.output_aliases.size() == output_names.size()) {
    for (size_t i = 0; i < output_names.size(); i++) {
      graph->FindTensor(output_names[i])->alias = config_.output_aliases[i];
    }
  } else {
    for (size_t i = 0; i < output_names.size(); i++) {
      const auto& output = graph->FindTensor(output_names[i]);
      if (output->alias.size() > 0) {
        continue;
      }
      const auto& producer = graph->FindProducer(output_names[i]);
      output->alias = producer->outputs.size() == 1
                          ? producer->name
                          : StringUtils::Replace(output_names[i], ":", "_");
    }
  }
  return graph;
}

MSCJoint RelayGraphBuilder::AddNode(const Expr& expr, const String& name) {
  const auto& node_name = name.size() > 0 ? name : SpanUtils::GetAttr(expr->span, "name");
  const auto& shared_ref = SpanUtils::GetAttr(expr->span, "shared_ref");

  // Get optype
  String optype;
  if (expr->IsInstance<relay::VarNode>()) {
    optype = "input";
  } else if (expr->IsInstance<relay::ConstantNode>()) {
    optype = "constant";
  } else if (expr->IsInstance<relay::TupleGetItemNode>()) {
    optype = "get_item";
  } else if (expr->IsInstance<relay::TupleNode>()) {
    optype = "tuple";
  } else if (const auto* call_node = expr.as<relay::CallNode>()) {
    if (const auto* op_node = call_node->op.as<OpNode>()) {
      optype = StringUtils::Replace(op_node->name, "relay.", "");
    } else {
      optype = "unknown_op";
    }
  } else if (const auto* f_node = expr.as<relay::FunctionNode>()) {
    const auto& name_opt = f_node->GetAttr<runtime::String>(relay::attr::kComposite);
    ICHECK(name_opt.defined()) << "Unexpected func without composite";
    optype = name_opt.value();
  } else {
    optype = "unknown_expr";
  }

  // Extract attributes
  Map<String, String> attrs;
  if (const auto* call_node = expr.as<relay::CallNode>()) {
    if (call_node->attrs.defined()) {
      AttrGetter getter(&attrs);
      const_cast<BaseAttrsNode*>(call_node->attrs.get())->VisitAttrs(&getter);
    }
  } else if (expr->IsInstance<relay::FunctionNode>()) {
    attrs = RelayFuncAttrGetter().GetAttrs(expr);
  } else if (const auto* const_node = expr.as<relay::ConstantNode>()) {
    if (const_node->is_scalar()) {
      attrs.Set("scalar", GetScalarStr(const_node->data, config_.float_precision));
    }
  } else if (const auto* get_node = expr.as<relay::TupleGetItemNode>()) {
    attrs.Set("index", std::to_string(get_node->index));
  }

  // Get scope
  Array<String> scope;
  if (optype != "input" && optype != "constant") {
    scope.push_back("block");
  }

  // Build inputs and weights
  Array<String> input_names;
  Map<String, MSCTensor> node_weights;
  if (const auto* call_node = expr.as<relay::CallNode>()) {
    const auto& input_types = ExprUtils::GetInputTypes(optype, call_node->args.size(), false);
    for (size_t i = 0; i < call_node->args.size(); i++) {
      const auto& arg = call_node->args[i];
      ICHECK(expr_tensor_map_.count(arg)) << "Missing argument " << arg;
      if (input_types[i] != "input" && arg->IsInstance<relay::ConstantNode>()) {
        const auto& t_name = expr_tensor_map_[arg][0];
        const auto& weight_name = SpanUtils::GetAttr(arg->span, "name");
        const auto& pair = tensor_input_map_[t_name];
        const auto& producer = Downcast<MSCJoint>(pair.first);
        if (!weights_.count(weight_name)) {
          const auto& ref = producer->OutputAt(pair.second);
          MSCTensor weight;
          if (input_types[i] == "bias") {
            weight = MSCTensor(weight_name, ref->dtype, "O", Array<Integer>{ref->GetSize()});
          } else {
            weight = MSCTensor(weight_name, ref->dtype, ref->layout.name(), ref->shape);
          }
          weights_.Set(weight_name, weight);
        }
        if (producer->HasAttr("scalar")) {
          attrs.Set(input_types[i], producer->GetTypeAttr<std::string>("scalar"));
        }
        node_weights.Set(input_types[i], weights_[weight_name]);
      } else {
        for (const auto& in_name : expr_tensor_map_[arg]) {
          input_names.push_back(in_name);
        }
      }
    }
  } else if (const auto* f_node = expr.as<relay::FunctionNode>()) {
    for (const auto& p : f_node->params) {
      for (const auto& in_name : expr_tensor_map_[p]) {
        input_names.push_back(in_name);
      }
    }
    ICHECK(HasFuncScope()) << "Function without func scope " << relay::PrettyPrint(expr);
    const auto& weight_names = func_scopes_.top().GetFuncWeights();
    const auto& input_types =
        ExprUtils::GetInputTypes(optype, f_node->params.size() + weight_names.size(), false);
    for (size_t i = 0; i < weight_names.size(); i++) {
      const auto& pair = tensor_input_map_[weight_names[i]];
      const auto& producer = Downcast<MSCJoint>(pair.first);
      if (!weights_.count(producer->name)) {
        const auto& ref = producer->OutputAt(pair.second);
        const auto& weight = MSCTensor(producer->name, ref->dtype, ref->layout.name(), ref->shape);
        weights_.Set(producer->name, weight);
      }
      if (producer->HasAttr("scalar")) {
        attrs.Set(input_types[i], producer->GetTypeAttr<std::string>("scalar"));
      }
      node_weights.Set(input_types[i + f_node->params.size()], weights_[producer->name]);
    }
  } else if (const auto* tuple_node = expr.as<relay::TupleNode>()) {
    for (const auto& f : tuple_node->fields) {
      ICHECK(expr_tensor_map_.count(f)) << "Can not find tuple field " << f;
      for (const auto& in_name : expr_tensor_map_[f]) {
        input_names.push_back(in_name);
      }
    }
  } else if (const auto* getitem_node = expr.as<relay::TupleGetItemNode>()) {
    ICHECK(expr_tensor_map_.count(getitem_node->tuple))
        << "Can not find tuple " << getitem_node->tuple;
    input_names = expr_tensor_map_[getitem_node->tuple];
  } else if (optype == "constant") {
    Type checked_type = expr->checked_type_;
    ICHECK(checked_type.defined() && checked_type->IsInstance<relay::TensorTypeNode>())
        << "Constant checked_type is not defined";
    const auto& t_info = Downcast<TensorType>(checked_type);
    const auto& layout = SpanUtils::GetAttr(expr->span, "layout");
    const auto& weight =
        MSCTensor(node_name, t_info->dtype, layout, ArrayUtils::Cast<Integer>(t_info->shape));
    node_weights.Set("const", weight);
  }
  std::vector<std::pair<BaseJoint, size_t>> inputs;
  for (const auto& i : input_names) {
    inputs.push_back(tensor_input_map_[i]);
  }

  // Build outputs
  Array<MSCTensor> outputs;
  const auto& layout = SpanUtils::GetAttr(expr->span, "layout");
  Type checked_type = expr->checked_type_;
  if (checked_type.defined() && checked_type->IsInstance<relay::FuncTypeNode>()) {
    checked_type = Downcast<FuncType>(checked_type)->ret_type;
  }
  if (checked_type.defined()) {
    if (const auto* t_info = checked_type.as<relay::TensorTypeNode>()) {
      const auto& shape = ArrayUtils::Cast<Integer>(t_info->shape);
      const auto& output =
          MSCTensor(node_name + ":" + std::to_string(0), t_info->dtype, layout, shape);
      outputs.push_back(output);
    } else if (const auto* tuple_info = checked_type.as<relay::TupleTypeNode>()) {
      Array<String> layouts = StringUtils::Split(layout, ",");
      if (layouts.size() == 0) {
        layouts = Array<String>(tuple_info->fields.size(), "");
      }
      ICHECK_EQ(layouts.size(), tuple_info->fields.size())
          << "Layout " << layout << " msimatch with fileds size " << tuple_info->fields.size();
      size_t field_size = tuple_info->fields.size();
      if (optype == "nn.batch_norm") {
        field_size = 1;
      }
      for (size_t i = 0; i < field_size; i++) {
        const auto& t_info = Downcast<relay::TensorType>(tuple_info->fields[i]);
        const auto& shape = ArrayUtils::Cast<Integer>(t_info->shape);
        const auto& output =
            MSCTensor(node_name + ":" + std::to_string(i), t_info->dtype, layouts[i], shape);
        outputs.push_back(output);
      }
    } else {
      LOG(FATAL) << "Unexpected checked_type " << checked_type;
    }
  }

  // Build node
  const auto& node = MSCJoint(nodes_.size(), node_name, shared_ref, optype, attrs, scope, inputs,
                              outputs, node_weights);
  Array<String> output_names;
  for (size_t i = 0; i < outputs.size(); i++) {
    output_names.push_back(outputs[i]->name);
    tensor_input_map_[outputs[i]->name] = std::make_pair(node, i);
  }
  nodes_.push_back(node);
  expr_tensor_map_.Set(expr, output_names);
  return node;
}

void RelayGraphBuilder::VisitExpr_(const relay::ConstantNode* op) {
  const auto& node = AddNode(GetRef<relay::Constant>(op));
  if (HasFuncScope()) {
    func_scopes_.top().AddFuncWeight(node->OutputAt(0)->name);
  }
}

void RelayGraphBuilder::VisitExpr_(const relay::FunctionNode* op) {
  const auto& name_opt = op->GetAttr<runtime::String>(relay::attr::kComposite);
  if (name_opt.defined()) {
    StartFuncScope(SpanUtils::GetAttr(op->span, "name"));
  }
  RelayExprVisitor::VisitExpr_(op);
  if (HasFuncScope()) {
    AddNode(GetRef<relay::Function>(op));
    EndFuncScope();
  }
}

void RelayGraphBuilder::VisitExpr_(const relay::CallNode* op) {
  if (const auto* f_node = op->op.as<relay::FunctionNode>()) {
    const auto& name_opt = f_node->GetAttr<runtime::String>(relay::attr::kComposite);
    if (name_opt.defined()) {
      for (size_t i = 0; i < op->args.size(); i++) {
        if (!expr_tensor_map_.count(op->args[i])) {
          RelayExprVisitor::VisitExpr(op->args[i]);
        }
        ICHECK(expr_tensor_map_.count(op->args[i]))
            << "Can not find argument " << relay::PrettyPrint(op->args[i]);
        expr_tensor_map_.Set(f_node->params[i], expr_tensor_map_[op->args[i]]);
      }
    }
  }
  RelayExprVisitor::VisitExpr_(op);
  if (!HasFuncScope() && op->op->IsInstance<OpNode>()) {
    try {
      AddNode(GetRef<relay::Call>(op));
    } catch (runtime::InternalError& err) {
      LOG(WARNING) << "Failed to add node from " << relay::PrettyPrint(GetRef<relay::Call>(op))
                   << " : " << err.message();
      throw err;
    }
  }
  if (op->op->IsInstance<relay::FunctionNode>() && expr_tensor_map_.count(op->op)) {
    expr_tensor_map_.Set(GetRef<relay::Call>(op), expr_tensor_map_[op->op]);
  }
}

void RelayGraphBuilder::VisitExpr_(const relay::TupleNode* val) {
  RelayExprVisitor::VisitExpr_(val);
  AddNode(GetRef<relay::Tuple>(val));
}

void RelayGraphBuilder::VisitExpr_(const relay::TupleGetItemNode* val) {
  RelayExprVisitor::VisitExpr_(val);
  AddNode(GetRef<relay::TupleGetItem>(val));
}

void RelayGraphBuilder::StartFuncScope(const String& name) {
  RelayFuncScope func_scope = RelayFuncScope(name);
  func_scopes_.push(func_scope);
}
void RelayGraphBuilder::EndFuncScope() {
  ICHECK(HasFuncScope()) << "No FuncScope found";
  func_scopes_.pop();
}

bool RelayGraphBuilder::HasFuncScope() { return func_scopes_.size() > 0; }

Map<MSCTensor, NDArray> RelayWeightsExtractor::GetWeights(const relay::Function& func) {
  VisitExpr(func);
  return weights_;
}

void RelayWeightsExtractor::VisitExpr_(const relay::ConstantNode* op) {
  const auto& name = SpanUtils::GetAttr(op->span, "name");
  const auto& layout = SpanUtils::GetAttr(op->span, "layout");
  const auto& t_info = op->tensor_type();
  const auto& shape = ArrayUtils::Cast<Integer>(t_info->shape);
  const auto& weight = MSCTensor(name, t_info->dtype, layout, shape);
  weights_.Set(weight, op->data);
}

TVM_REGISTER_GLOBAL("msc.core.BuildFromRelax")
    .set_body_typed([](const IRModule& relax_module, const String& entry_name,
                       const String& options) -> MSCGraph {
      auto builder = RelaxGraphBuilder(relax_module, entry_name, options);
      const auto& func_name =
          builder.config().byoc_entry.size() > 0 ? String(builder.config().byoc_entry) : entry_name;
      const auto& func = Downcast<relax::Function>(relax_module->Lookup(func_name));
      return builder.Build(func);
    });

TVM_REGISTER_GLOBAL("msc.core.GetRelaxWeights")
    .set_body_typed([](const IRModule& relax_module,
                       const String& entry_name) -> Map<MSCTensor, NDArray> {
      const auto& func = Downcast<relax::Function>(relax_module->Lookup(entry_name));
      return RelaxWeightsExtractor().GetWeights(func);
    });

TVM_REGISTER_GLOBAL("msc.core.BuildFromRelay")
    .set_body_typed([](const IRModule& relay_module, const String& entry_name,
                       const String& options) -> MSCGraph {
      const auto& func = Downcast<relay::Function>(relay_module->Lookup(entry_name));
      return RelayGraphBuilder(relay_module, entry_name, options).Build(func);
    });

TVM_REGISTER_GLOBAL("msc.core.GetRelayWeights")
    .set_body_typed([](const IRModule& relay_module,
                       const String& entry_name) -> Map<MSCTensor, NDArray> {
      const auto& func = Downcast<relay::Function>(relay_module->Lookup(entry_name));
      return RelayWeightsExtractor().GetWeights(func);
    });

}  // namespace msc
}  // namespace contrib
}  // namespace tvm
