// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <fstream>

#include "model_builder.h"
#include "helper.h"
#include "op_builder_factory.h"
#include "core/providers/coreml/model/model.h"
#include "core/providers/coreml/model/host_utils.h"

namespace onnxruntime {
namespace coreml {

ModelBuilder::ModelBuilder(const GraphViewer& graph_viewer)
    : graph_viewer_(graph_viewer),
      coreml_model_(onnxruntime::make_unique<CoreML::Specification::Model>()) {
  (void)graph_viewer_;
}

Status ModelBuilder::Prepare() {
  {  // initialize CoreML model
    // We support CorelML Specification Version 4 (Core ML 3)
    coreml_model_->set_specificationversion(4);
    auto* neural_network = coreml_model_->mutable_neuralnetwork();
    neural_network->set_arrayinputshapemapping(::CoreML::Specification::NeuralNetworkMultiArrayShapeMapping::EXACT_ARRAY_MAPPING);
  }

  PreprocessInitializers();
  ORT_RETURN_IF_ERROR(RegisterInitializers());
  ORT_RETURN_IF_ERROR(RegisterModelInputs());
  ORT_RETURN_IF_ERROR(AddOperations());
  ORT_RETURN_IF_ERROR(RegisterModelOutputs());

  return Status::OK();
}

/* static */ const IOpBuilder* ModelBuilder::GetOpBuilder(const Node& node) {
  const auto& op_builders = GetOpBuilders();
  const auto it = op_builders.find(node.OpType());
  if (it != op_builders.cend())
    return op_builders.at(node.OpType());

  return nullptr;
}

void ModelBuilder::PreprocessInitializers() {
  const auto& node_indices = graph_viewer_.GetNodesInTopologicalOrder();
  for (size_t i = 0; i < node_indices.size(); i++) {
    const auto* node(graph_viewer_.GetNode(node_indices[i]));
    if (const auto* op_builder = GetOpBuilder(*node)) {
      op_builder->AddInitializersToSkip(*this, *node);
    }
  }
}

Status ModelBuilder::RegisterInitializers() {
  // TODO, create LoadConstantNDLayer(s) for initializers
  return Status::OK();
}

Status ModelBuilder::RegisterModelInputOutput(COREML_SPEC::FeatureDescription& input_output,
                                              const NodeArg& node_arg, bool is_input) {
  const auto& name = node_arg.Name();
  const std::string input_output_type = is_input ? "input" : "output";

  // input should not be an initializer
  if (is_input && Contains(GetInitializerTensors(), name)) {
    return Status::OK();
  }

  input_output.set_name(name);
  auto* multi_array = input_output.mutable_type()->mutable_multiarraytype();
  std::vector<int64_t> shape;

  {  // input_output shape
    const auto* shape_proto = node_arg.Shape();
    ORT_RETURN_IF(shape_proto == nullptr,
                  "shape_proto cannot be null for ", input_output_type, ": ", name);
    const auto& dims = shape_proto->dim();
    if (dims.empty()) {
      // If we have an empty shape, this is a scalar input,
      // Since all the input output of CoreML EP is MultiArray, we will make the scalar input output as a {1} MultiArray
      shape.push_back(1);

      // we need to change the shapes of these scalar outputs back to {} when NNAPI EP returns these values to ORT
      if (!is_input) {
        AddScalarOutput(name);
      }
    } else {
      shape.reserve(dims.size());
      for (const auto& dim : dims) {
        ORT_RETURN_IF_NOT(dim.has_dim_value(),
                          "Dynamic shape is not supported yet, for ", input_output_type, ": ", name);
        shape.push_back(dim.dim_value());
      }
    }
  }

  *multi_array->mutable_shape() = {shape.cbegin(), shape.cend()};

  int32_t data_type;
  {  // type
    const auto* type_proto = node_arg.TypeAsProto();
    if (!type_proto || !type_proto->tensor_type().has_elem_type()) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "The  ", input_output_type, " of graph doesn't have elem_type: ", name);
    }

    data_type = type_proto->tensor_type().elem_type();
    switch (data_type) {
      case ONNX_NAMESPACE::TensorProto_DataType_FLOAT:
        multi_array->set_datatype(COREML_SPEC::ArrayFeatureType::FLOAT32);
        break;
      default: {
        // TODO: support other type
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                               "The  ", input_output_type, " of graph doesn't have valid type, name: ", name,
                               " type: ", type_proto->tensor_type().elem_type());
      }
    }
  }

  input_output_info_.emplace(name, OnnxTensorInfo{data_type, shape});

  return Status::OK();
}

Status ModelBuilder::RegisterModelInputs() {
  auto* model_description = coreml_model_->mutable_description();
  for (const auto* node_arg : graph_viewer_.GetInputs()) {
    const auto& input_name = node_arg->Name();

    {  // input should not be an initializer
      if (Contains(GetInitializerTensors(), input_name))
        continue;
    }

    auto& input = *model_description->mutable_output()->Add();
    input.set_name(input_name);
    auto* multi_array = input.mutable_type()->mutable_multiarraytype();
    std::vector<int64_t> shape;

    {  // input shape
      const auto* shape_proto = node_arg->Shape();
      ORT_RETURN_IF_NOT(shape_proto != nullptr, "shape_proto cannot be null for input: ", input_name);
      const auto& dims = shape_proto->dim();
      if (dims.empty()) {
        // If we have an empty shape, this is a scalar input,
        // Since all the input output of CoreML EP is MultiArray, we will make the scalar input as a {1} MultiArray
        shape.push_back(1);
      } else {
        shape.reserve(dims.size());
        for (const auto& dim : dims) {
          ORT_RETURN_IF_NOT(dim.has_dim_value(), "Dynamic shape is not supported yet, for input, ", node_arg->Name());
          shape.push_back(dim.dim_value());
        }
      }
    }

    *multi_array->mutable_shape() = {shape.cbegin(), shape.cend()};

    int32_t data_type;
    {  // input type
      const auto* type_proto = node_arg->TypeAsProto();
      if (!type_proto || !type_proto->tensor_type().has_elem_type()) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                               "The input of graph doesn't have elem_type: ", input_name);
      }

      data_type = type_proto->tensor_type().elem_type();
      switch (data_type) {
        case ONNX_NAMESPACE::TensorProto_DataType_FLOAT:
          multi_array->set_datatype(COREML_SPEC::ArrayFeatureType::FLOAT32);
          break;
        default: {
          // TODO: support other type
          return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                                 "The input of graph doesn't have valid type, name: ", input_name,
                                 " type: ", type_proto->tensor_type().elem_type());
        }
      }
    }

    input_output_info_.emplace(input_name, OnnxTensorInfo{data_type, shape});
  }

  return Status::OK();
}

Status ModelBuilder::AddOperations() {
  const auto& node_indices = graph_viewer_.GetNodesInTopologicalOrder();
  for (size_t i = 0; i < node_indices.size(); i++) {
    const auto* node(graph_viewer_.GetNode(node_indices[i]));
    if (const auto* op_builder = GetOpBuilder(*node)) {
      ORT_RETURN_IF_ERROR(op_builder->AddToModelBuilder(*this, *node));
    } else {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Node [", node->Name(), "], type [", node->OpType(), "] is not supported");
    }
  }

  return Status::OK();
}

Status ModelBuilder::RegisterModelOutputs() {
  auto* model_description = coreml_model_->mutable_description();
  for (const auto* node_arg : graph_viewer_.GetOutputs()) {
    const auto& output_name = node_arg->Name();

    auto& output = *model_description->mutable_output()->Add();
    output.set_name(output_name);
    auto* multi_array = output.mutable_type()->mutable_multiarraytype();

    {  // output shape
       // Since for now all the shapes are deterministic for CoreML, it's impossible we can have unknown output shape
      const auto* shape_proto = node_arg->Shape();
      ORT_RETURN_IF(shape_proto == nullptr, "shape_proto cannot be null for output: ", output_name);
      const auto& dims = shape_proto->dim();
      if (dims.empty()) {
        // In CoreML scalar output will be a {1} MultiArray
        // we need to change the shapes of these scalar outputs back to {} when NNAPI EP returns these values to ORT
        AddScalarOutput(output_name);
        multi_array->mutable_shape()->Add(1);
      } else {
        for (const auto& dim : dims) {
          ORT_RETURN_IF_NOT(dim.has_dim_value(), "Dynamic shape is not supported yet, for output, ", node_arg->Name());
          multi_array->mutable_shape()->Add(dim.dim_value());
        }
      }
    }

    {  // output type
      const auto* type_proto = node_arg->TypeAsProto();
      if (!type_proto || !type_proto->tensor_type().has_elem_type()) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                               "The output of graph doesn't have elem_type: ", output_name);
      }

      switch (type_proto->tensor_type().elem_type()) {
        case ONNX_NAMESPACE::TensorProto_DataType_FLOAT:
          multi_array->set_datatype(COREML_SPEC::ArrayFeatureType::FLOAT32);
          break;
        default: {
          // TODO: support other type
          return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                                 "The output of graph doesn't have valid type, name: ", output_name,
                                 " type: ", type_proto->tensor_type().elem_type());
        }
      }
    }
  }

  return Status::OK();
}

Status ModelBuilder::Compile(std::unique_ptr<Model>& model, const std::string& path) {
  ORT_RETURN_IF_ERROR(Prepare());
  ORT_RETURN_IF_ERROR(SaveCoreMLModel(path));
  model = onnxruntime::make_unique<Model>(path);
  model->SetScalarOutputs(std::move(scalar_outputs_));
  return Status::OK();
}

Status ModelBuilder::SaveCoreMLModel(const std::string& path) {
  std::ofstream stream(path, std::ofstream::out | std::ofstream::binary);
  ORT_RETURN_IF_NOT(coreml_model_->SerializeToOstream(&stream), "Save the CoreML model failed");
  return Status::OK();
}

void ModelBuilder::AddScalarOutput(const std::string& output_name) {
  scalar_outputs_.insert(output_name);
}

void ModelBuilder::AddLayer(COREML_SPEC::NeuralNetworkLayer* layer) {
  auto* neural_network = coreml_model_->mutable_neuralnetwork();
  neural_network->mutable_layers()->AddAllocated(layer);
}

}  // namespace coreml
}  // namespace onnxruntime
