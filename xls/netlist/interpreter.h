// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef XLS_NETLIST_INTERPRETER_H_
#define XLS_NETLIST_INTERPRETER_H_

#include <deque>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"
#include "xls/netlist/function_parser.h"
#include "xls/netlist/netlist.h"

namespace xls {
namespace netlist {

template <typename EvalT>
using AbstractNetRef2Value =
    absl::flat_hash_map<const rtl::AbstractNetRef<EvalT>, EvalT>;

// Interprets Netlists/Modules given a set of input values and returns the
// resulting value.
template <typename EvalT = bool>
class AbstractInterpreter {
 public:
  explicit AbstractInterpreter(rtl::AbstractNetlist<EvalT>* netlist, EvalT zero,
                               EvalT one)
      : netlist_(netlist), zero_(std::move(zero)), one_(std::move(one)) {}

  template <typename = std::is_constructible<EvalT, bool>>
  explicit AbstractInterpreter(rtl::AbstractNetlist<EvalT>* netlist)
      : netlist_(netlist),
        zero_(std::move(EvalT{false})),
        one_(std::move(EvalT{true})) {}

  // Interprets the given module with the given input mapping.
  //  - inputs: Mapping of module input wire to value. Must have the same size
  //    as module->inputs();
  //  - dump_cells: List of cells whose inputs and outputs should be dumped
  //    on evaluation.
  absl::StatusOr<AbstractNetRef2Value<EvalT>> InterpretModule(
      const rtl::AbstractModule<EvalT>* module,
      const AbstractNetRef2Value<EvalT>& inputs,
      absl::Span<const std::string> dump_cells = {});

 private:
  // Returns true if the specified AbstractNetRef is an output of the given
  // cell.
  bool IsCellOutput(const rtl::AbstractCell<EvalT>& cell,
                    const rtl::AbstractNetRef<EvalT> ref);

  absl::Status InterpretCell(const rtl::AbstractCell<EvalT>& cell,
                             AbstractNetRef2Value<EvalT>& processed_wires);

  absl::StatusOr<EvalT> InterpretFunction(
      const rtl::AbstractCell<EvalT>& cell, const function::Ast& ast,
      const AbstractNetRef2Value<EvalT>& processed_wires);

  // Returns the value of the internal/output pin from the cell (defined by a
  // "statetable" attribute under the conditions defined in "processed_wires".
  absl::StatusOr<EvalT> InterpretStateTable(
      const rtl::AbstractCell<EvalT>& cell, const std::string& pin_name,
      const AbstractNetRef2Value<EvalT>& processed_wires);

  rtl::AbstractNetlist<EvalT>* netlist_;
  EvalT zero_;
  EvalT one_;
};

using Interpreter = AbstractInterpreter<>;

template <typename EvalT>
absl::StatusOr<AbstractNetRef2Value<EvalT>>
AbstractInterpreter<EvalT>::InterpretModule(
    const rtl::AbstractModule<EvalT>* module,
    const AbstractNetRef2Value<EvalT>& inputs,
    absl::Span<const std::string> dump_cells) {
  // Do a topological sort through all cells, evaluating each as its inputs are
  // fully satisfied, and store those results with each output wire.

  // The list of "unsatisfied" cells.
  absl::flat_hash_map<rtl::AbstractCell<EvalT>*,
                      absl::flat_hash_set<rtl::AbstractNetRef<EvalT>>>
      cell_inputs;

  // The set of wires that have been "activated" (whose source cells have been
  // processed) but not yet processed.
  std::deque<rtl::AbstractNetRef<EvalT>> active_wires;

  // Holds the [EvalT] value of a wire that's been processed.
  AbstractNetRef2Value<EvalT> processed_wires;

  // First, populate the unsatisfied cell list.
  for (const auto& cell : module->cells()) {
    // if a cell has no inputs, it's active, so process it now.
    if (cell->inputs().empty()) {
      XLS_RETURN_IF_ERROR(InterpretCell(*cell, processed_wires));
      for (const auto& output : cell->outputs()) {
        active_wires.push_back(output.netref);
      }
    } else {
      absl::flat_hash_set<rtl::AbstractNetRef<EvalT>> inputs;
      for (const auto& input : cell->inputs()) {
        inputs.insert(input.netref);
      }
      cell_inputs[cell.get()] = std::move(inputs);
    }
  }

  absl::flat_hash_set<std::string> dump_cell_set(dump_cells.begin(),
                                                 dump_cells.end());

  // Set all inputs as "active".
  for (const rtl::AbstractNetRef<EvalT> ref : module->inputs()) {
    active_wires.push_back(ref);
  }
  XLS_ASSIGN_OR_RETURN(rtl::AbstractNetRef<EvalT> net_0,
                       module->ResolveNumber(0));
  XLS_ASSIGN_OR_RETURN(rtl::AbstractNetRef<EvalT> net_1,
                       module->ResolveNumber(1));
  active_wires.push_back(net_0);
  active_wires.push_back(net_1);

  for (const auto& input : inputs) {
    processed_wires.emplace(input.first, std::move(input.second));
    if constexpr (std::is_convertible<EvalT, int>()) {
      XLS_VLOG(2) << "Input : " << input.first->name() << " : "
                  << static_cast<int>(input.second);
    }
  }
  processed_wires.insert({net_0, zero_});
  processed_wires.insert({net_1, one_});

  // Process all active wires : see if this wire satisfies all of a cell's
  // inputs. If so, interpret the cell, and place its outputs on the active wire
  // list.
  while (!active_wires.empty()) {
    rtl::AbstractNetRef<EvalT> wire = active_wires.front();
    active_wires.pop_front();
    XLS_VLOG(2) << "Processing wire: " << wire->name();

    for (const auto& cell : wire->connected_cells()) {
      if (IsCellOutput(*cell, wire)) {
        continue;
      }

      cell_inputs[cell].erase(wire);
      if (cell_inputs[cell].empty()) {
        XLS_VLOG(2) << "Processing cell: " << cell->name();
        XLS_RETURN_IF_ERROR(InterpretCell(*cell, processed_wires));
        for (const auto& output : cell->outputs()) {
          active_wires.push_back(output.netref);
        }

        if (dump_cell_set.contains(cell->name())) {
          XLS_LOG(INFO) << "Cell " << cell->name() << " inputs:";
          if constexpr (std::is_convertible<EvalT, int>()) {
            for (const auto& input : cell->inputs()) {
              XLS_LOG(INFO) << "   " << input.netref->name() << " : "
                            << static_cast<int>(processed_wires[input.netref]);
            }

            XLS_LOG(INFO) << "Cell " << cell->name() << " outputs:";
            for (const auto& output : cell->outputs()) {
              XLS_LOG(INFO) << "   " << output.netref->name() << " : "
                            << static_cast<int>(processed_wires[output.netref]);
            }
          } else {
            XLS_LOG(INFO) << "Cell " << cell->name()
                          << " inputs are not printable.";
          }
        }
      } else if (XLS_VLOG_IS_ON(2)) {
        XLS_VLOG(2) << "Cell remaining: " << cell->name();
        for (const auto& remaining : cell_inputs[cell]) {
          XLS_VLOG(2) << " - " << remaining->name();
        }
      }
    }
  }

  // Soundness check that we've processed all cells (i.e., that there aren't
  // unsatisfiable cells).
  for (const auto& cell : module->cells()) {
    for (const auto& output : cell->outputs()) {
      if (!processed_wires.contains(output.netref)) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Netlist contains unconnected subgraphs and cannot be translated. "
            "Example: cell %s, output %s.",
            cell->name(), output.netref->name()));
      }
    }
  }

  AbstractNetRef2Value<EvalT> outputs;
  outputs.reserve(module->outputs().size());
  for (const rtl::AbstractNetRef<EvalT> output : module->outputs()) {
    if (processed_wires.contains(output)) {
      outputs.emplace(output, std::move(processed_wires.at(output)));
    } else {
      const absl::flat_hash_map<rtl::AbstractNetRef<EvalT>,
                                rtl::AbstractNetRef<EvalT>>& assigns =
          module->assigns();
      XLS_RET_CHECK(assigns.contains(output));
      rtl::AbstractNetRef<EvalT> net_value = assigns.at(output);
      if (net_value == net_0) {
        outputs.insert({output, zero_});
      } else if (net_value == net_1) {
        outputs.insert({output, one_});
      } else {
        XLS_RET_CHECK(inputs.contains(net_value));
        outputs.insert({output, inputs.at(net_value)});
      }
    }
  }

  return outputs;
}

template <typename EvalT>
absl::Status AbstractInterpreter<EvalT>::InterpretCell(
    const rtl::AbstractCell<EvalT>& cell,
    AbstractNetRef2Value<EvalT>& processed_wires) {
  const AbstractCellLibraryEntry<EvalT>* entry = cell.cell_library_entry();
  absl::StatusOr<const rtl::AbstractModule<EvalT>*> status_or_module =
      netlist_->GetModule(entry->name());
  if (status_or_module.ok()) {
    // If this "cell" is actually a module defined in the netlist,
    // then recursively evaluate it.
    AbstractNetRef2Value<EvalT> inputs;
    // who's input/output name - needs to be internal
    // need to map cell inputs to module inputs?
    auto module = status_or_module.value();
    const std::vector<rtl::AbstractNetRef<EvalT>>& module_input_refs =
        module->inputs();
    const absl::Span<const std::string> module_input_names =
        module->AsCellLibraryEntry()->input_names();

    for (const auto& input : cell.inputs()) {
      // We need to match the inputs - from the NetRefs in this module to the
      // NetRefs in the child module. In AbstractModule, the order of inputs
      // (as NetRefs) is the same as the input names in its
      // AbstractCellLibraryEntry. That means, for each input (in this module):
      //  - Find the child module input pin/AbstractNetRef with the same name.
      //  - Assign the corresponding child module input AbstractNetRef to have
      //  the value
      //    of the wire in this module.
      // If ever an input isn't found, that's bad. Abort.
      bool input_found = false;
      for (int i = 0; i < module_input_names.size(); i++) {
        if (module_input_names[i] == input.name) {
          inputs.emplace(module_input_refs[i],
                         std::move(processed_wires.at(input.netref)));
          input_found = true;
          break;
        }
      }

      XLS_RET_CHECK(input_found) << absl::StrFormat(
          "Could not find input pin \"%s\" in module \"%s\", referenced in "
          "cell \"%s\"!",
          input.name, module->name(), cell.name());
    }

    XLS_ASSIGN_OR_RETURN(AbstractNetRef2Value<EvalT> child_outputs,
                         InterpretModule(module, inputs));
    // We need to do the same here - map the NetRefs in the module's output
    // to the NetRefs in this module, using pin names as the matching keys.
    for (const auto& child_output : child_outputs) {
      bool output_found = false;
      for (const auto& cell_output : cell.outputs()) {
        if (child_output.first->name() == cell_output.name) {
          processed_wires.insert({cell_output.netref, child_output.second});
          output_found = true;
          break;
        }
      }
      XLS_RET_CHECK(output_found);
      XLS_RET_CHECK(output_found) << absl::StrFormat(
          "Could not find cell output pin \"%s\" in cell \"%s\", referenced in "
          "child module \"%s\"!",
          child_output.first->name(), cell.name(), module->name());
    }

    return absl::OkStatus();
  }

  const auto& pins = entry->output_pin_to_function();
  for (int i = 0; i < cell.outputs().size(); i++) {
    XLS_ASSIGN_OR_RETURN(
        function::Ast ast,
        function::Parser::ParseFunction(pins.at(cell.outputs()[i].name)));
    XLS_ASSIGN_OR_RETURN(EvalT value,
                         InterpretFunction(cell, ast, processed_wires));
    processed_wires.insert({cell.outputs()[i].netref, value});
  }

  return absl::OkStatus();
}

template <typename EvalT>
absl::StatusOr<EvalT> AbstractInterpreter<EvalT>::InterpretFunction(
    const rtl::AbstractCell<EvalT>& cell, const function::Ast& ast,
    const AbstractNetRef2Value<EvalT>& processed_wires) {
  switch (ast.kind()) {
    case function::Ast::Kind::kAnd: {
      XLS_ASSIGN_OR_RETURN(EvalT lhs, InterpretFunction(cell, ast.children()[0],
                                                        processed_wires));
      XLS_ASSIGN_OR_RETURN(EvalT rhs, InterpretFunction(cell, ast.children()[1],
                                                        processed_wires));
      return lhs & rhs;
    }
    case function::Ast::Kind::kIdentifier: {
      rtl::AbstractNetRef<EvalT> ref = nullptr;
      for (const auto& input : cell.inputs()) {
        if (input.name == ast.name()) {
          ref = input.netref;
        }
      }

      if (ref == nullptr) {
        for (const auto& internal : cell.internal_pins()) {
          if (internal.name == ast.name()) {
            return InterpretStateTable(cell, internal.name, processed_wires);
          }
        }
      }

      if (ref == nullptr) {
        return absl::NotFoundError(
            absl::StrFormat("Identifier \"%s\" not found in cell %s's inputs "
                            "or internal signals.",
                            ast.name(), cell.name()));
      }

      return processed_wires.at(ref);
    }
    case function::Ast::Kind::kLiteralOne:
      return one_;
    case function::Ast::Kind::kLiteralZero:
      return zero_;
    case function::Ast::Kind::kNot: {
      XLS_ASSIGN_OR_RETURN(
          EvalT value,
          InterpretFunction(cell, ast.children()[0], processed_wires));
      return !value;
    }
    case function::Ast::Kind::kOr: {
      XLS_ASSIGN_OR_RETURN(EvalT lhs, InterpretFunction(cell, ast.children()[0],
                                                        processed_wires));
      XLS_ASSIGN_OR_RETURN(EvalT rhs, InterpretFunction(cell, ast.children()[1],
                                                        processed_wires));
      return lhs | rhs;
    }
    case function::Ast::Kind::kXor: {
      XLS_ASSIGN_OR_RETURN(EvalT lhs, InterpretFunction(cell, ast.children()[0],
                                                        processed_wires));
      XLS_ASSIGN_OR_RETURN(EvalT rhs, InterpretFunction(cell, ast.children()[1],
                                                        processed_wires));
      return lhs ^ rhs;
    }
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("Unknown AST element type: ", ast.kind()));
  }
}

template <typename EvalT>
absl::StatusOr<EvalT> AbstractInterpreter<EvalT>::InterpretStateTable(
    const rtl::AbstractCell<EvalT>& cell, const std::string& pin_name,
    const AbstractNetRef2Value<EvalT>& processed_wires) {
  XLS_RET_CHECK(cell.cell_library_entry()->state_table());
  const AbstractStateTable<EvalT>& state_table =
      cell.cell_library_entry()->state_table().value();

  typename AbstractStateTable<EvalT>::InputStimulus stimulus;
  for (const auto& input : cell.inputs()) {
    stimulus.emplace(input.name, std::move(processed_wires.at(input.netref)));
  }

  for (const auto& pin : cell.internal_pins()) {
    if (pin.name == pin_name) {
      return state_table.GetSignalValue(stimulus, pin.name);
    }
  }

  return absl::NotFoundError(
      absl::StrFormat("Signal %s not found in state table!", pin_name));
}

template <typename EvalT>
bool AbstractInterpreter<EvalT>::IsCellOutput(
    const rtl::AbstractCell<EvalT>& cell,
    const rtl::AbstractNetRef<EvalT> ref) {
  for (const auto& output : cell.outputs()) {
    if (ref == output.netref) {
      return true;
    }
  }

  return false;
}

}  // namespace netlist
}  // namespace xls

#endif  // XLS_NETLIST_INTERPRETER_H_
