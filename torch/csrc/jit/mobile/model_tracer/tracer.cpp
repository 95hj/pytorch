#include <iostream>
#include <string>

/**
 * The tracer.cpp generates a binary that accepts a TorchScript model or a
 * Torch Mobile Model (with bytecode.pkl) which has at least 1 bundled
 * input. This binary then feeds the bundled input(s) into the model
 * and executes using the lite interpreter.
 *
 * Both root operators as well as called operators are recorded and saved
 * into a YAML file (whose path is provided on the command line).
 *
 * Note: Root operators may include primary and other operators that
 * are not invoked using the dispatcher, and hence they may not show
 * up in the Traced Operator list.
 *
 */

#include <ATen/core/dispatch/ObservedOperators.h>
#include <torch/csrc/autograd/grad_mode.h>
#include <torch/csrc/jit/mobile/import.h>
#include <torch/csrc/jit/mobile/model_tracer/KernelDTypeTracer.h>
#include <torch/csrc/jit/mobile/model_tracer/MobileModelRunner.h>
#include <torch/csrc/jit/mobile/model_tracer/OperatorCallTracer.h>
#include <torch/csrc/jit/mobile/model_tracer/TensorUtils.h>
#include <torch/csrc/jit/mobile/model_tracer/TracerRunner.h>
#include <torch/csrc/jit/mobile/module.h>
#include <torch/csrc/jit/mobile/parse_operators.h>
#include <torch/script.h>

typedef std::map<std::string, std::set<std::string>> kt_type;

C10_DEFINE_string(
    model_input_path,
    "",
    "The path of the input model file (.ptl).");

C10_DEFINE_string(
    build_yaml_path,
    "",
    "The path of the output YAML file containing traced operator information.");

#define REQUIRE_STRING_ARG(name)                            \
  if (FLAGS_##name.empty()) {                               \
    std::cerr << "You must specify the flag --" #name "\n"; \
    return 1;                                               \
  }

#define REQUIRE_INT_ARG(name)                               \
  if (FLAGS_##name == -1) {                                 \
    std::cerr << "You must specify the flag --" #name "\n"; \
    return 1;                                               \
  }

void printOpYAML(
    std::ostream& out,
    int indent,
    const std::string& op_name,
    bool is_used_for_training,
    bool is_root_operator,
    bool include_all_overloads) {
  out << std::string(indent, ' ') << op_name << ":" << std::endl;
  out << std::string(indent + 2, ' ')
      << "is_used_for_training: " << (is_used_for_training ? "true" : "false")
      << std::endl;
  out << std::string(indent + 2, ' ')
      << "is_root_operator: " << (is_root_operator ? "true" : "false")
      << std::endl;
  out << std::string(indent + 2, ' ')
      << "include_all_overloads: " << (include_all_overloads ? "true" : "false")
      << std::endl;
}

void printOpsYAML(
    std::ostream& out,
    const std::set<std::string>& operator_list,
    bool is_used_for_training,
    bool is_root_operator,
    bool include_all_overloads) {
  for (auto& it : operator_list) {
    printOpYAML(out, 2, it, false, is_root_operator, false);
  }
}

void printDTypeYAML(
    std::ostream& out,
    int indent,
    const std::string& kernel_tag_name,
    const std::set<std::string> dtypes) {
  std::string indent_str = std::string(indent, ' ');
  out << indent_str << kernel_tag_name << ":" << std::endl;
  for (auto& dtype : dtypes) {
    out << indent_str << "- " << dtype << std::endl;
  }
}

void printDTypesYAML(
    std::ostream& out,
    const torch::jit::mobile::KernelDTypeTracer::kernel_tags_type&
        kernel_tags) {
  for (auto& it : kernel_tags) {
    printDTypeYAML(out, 2, it.first, it.second);
  }
}

void printCustomClassesYAML(
    std::ostream& out,
    const torch::jit::mobile::CustomClassTracer::custom_classes_type&
        loaded_classes) {
  for (auto& class_name : loaded_classes) {
    out << "- " << class_name << std::endl;
  }
}

/**
 * Converts a pytorch model (full/lite) to lite interpreter model for
 * mobile, and additionally writes out a list of root and called
 * operators.
 */
int main(int argc, char* argv[]) {
  if (!c10::ParseCommandLineFlags(&argc, &argv)) {
    std::cerr << "Failed to parse command line flags!" << std::endl;
    return 1;
  }

  REQUIRE_STRING_ARG(model_input_path);
  REQUIRE_STRING_ARG(build_yaml_path);

  const std::string input_module_path = FLAGS_model_input_path;

  std::ofstream yaml_out(FLAGS_build_yaml_path);

  std::cout << "Processing: " << input_module_path << std::endl;
  std::cout << "Output: " << FLAGS_build_yaml_path << std::endl;
  torch::jit::mobile::TracerResult tracer_result;
  try {
    tracer_result = torch::jit::mobile::trace_run(FLAGS_model_input_path);
  } catch (std::exception& ex) {
    std::cerr
        << "ModelTracer has not been able to load the module for the following reasons:\n"
        << ex.what()
        << "\nPlease consider posting to the PyTorch with the error message."
        << std::endl;

    throw ex;
  }

  if (tracer_result.traced_operators.size() <=
      torch::jit::mobile::always_included_traced_ops.size()) {
    std::cerr
        << c10::str(
               "Error traced_operators size: ",
               tracer_result.traced_operators.size(),
               ". Expected the traced operator list to be bigger then the default size ",
               torch::jit::mobile::always_included_traced_ops.size(),
               ". Please report a bug in PyTorch.")
        << std::endl;
  }

  // If the op exist in both traced_ops and root_ops, leave it in root_ops only
  for (const auto& root_op : tracer_result.root_ops) {
    if (tracer_result.traced_operators.find(root_op) !=
        tracer_result.traced_operators.end()) {
      tracer_result.traced_operators.erase(root_op);
    }
  }

  yaml_out << "include_all_non_op_selectives: false" << std::endl;
  yaml_out << "build_features: []" << std::endl;
  yaml_out << "operators:" << std::endl;
  printOpsYAML(
      yaml_out,
      tracer_result.root_ops,
      false /* is_used_for_training */,
      true /* is_root_operator */,
      false /* include_all_overloads */);
  printOpsYAML(
      yaml_out,
      tracer_result.traced_operators,
      false /* is_used_for_training */,
      false /* is_root_operator */,
      false /* include_all_overloads */);

  yaml_out << "kernel_metadata:";
  if (tracer_result.called_kernel_tags.empty()) {
    yaml_out << " []";
  }
  yaml_out << std::endl;
  printDTypesYAML(yaml_out, tracer_result.called_kernel_tags);

  yaml_out << "custom_classes:";
  if (tracer_result.loaded_classes.empty()) {
    yaml_out << " []";
  }
  yaml_out << std::endl;
  printCustomClassesYAML(yaml_out, tracer_result.loaded_classes);

  return 0;
}
