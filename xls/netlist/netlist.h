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

// Data structure that represents netlists (e.g. ones that have been parsed in
// from the synthesis flow).

#ifndef XLS_NETLIST_NETLIST_H_
#define XLS_NETLIST_NETLIST_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/types/optional.h"
#include "xls/common/bits_util.h"
#include "xls/common/logging/logging.h"
#include "xls/netlist/cell_library.h"

namespace xls {
namespace netlist {
namespace rtl {

// Forward declaration for use in AbstractNetRef.
template <typename EvalT = bool>
class AbstractNetDef;

// Refers to an ID inside the module's wires_ array.
template <typename EvalT = bool>
using AbstractNetRef = AbstractNetDef<EvalT>*;

// The default and most common case is for bool.
using NetRef = AbstractNetRef<>;

// Represents a cell instantiated in the netlist.
template <typename EvalT = bool>
class AbstractCell {
 public:
  // Simple utility struct to capture data for a AbstractCell's input or output
  // pins.
  struct Pin {
    // Name of the pin in the cell's function description.
    std::string name;

    // The associated net from the netlist.
    AbstractNetRef<EvalT> netref;
  };

  // In this class, both "inputs" and "outputs" are maps of cell input/output
  // pin name to the AbstractNetDef/Ref used as that input in a given instance.
  // For outputs, if a pin isn't used, then it won't be present in the provided
  // map.
  // "dummy_net" is a ref to the "dummy" cell used by the containing module for
  // output wires that aren't connected to any cells.
  static absl::StatusOr<AbstractCell> Create(
      const AbstractCellLibraryEntry<EvalT>* cell_library_entry,
      absl::string_view name,
      const absl::flat_hash_map<std::string, AbstractNetRef<EvalT>>&
          named_parameter_assignments,
      absl::optional<AbstractNetRef<EvalT>> clock,
      AbstractNetRef<EvalT> dummy_net);

  const AbstractCellLibraryEntry<EvalT>* cell_library_entry() const {
    return cell_library_entry_;
  }
  const std::string& name() const { return name_; }
  CellKind kind() const { return cell_library_entry_->kind(); }

  absl::Span<const Pin> inputs() const { return inputs_; }
  absl::Span<const Pin> outputs() const { return outputs_; }
  absl::Span<const Pin> internal_pins() const { return internal_pins_; }
  const absl::optional<AbstractNetRef<EvalT>>& clock() const { return clock_; }

 private:
  AbstractCell(const AbstractCellLibraryEntry<EvalT>* cell_library_entry,
               absl::string_view name, const std::vector<Pin>& inputs,
               const std::vector<Pin>& outputs,
               const std::vector<Pin>& internal_pins,
               absl::optional<AbstractNetRef<EvalT>> clock)
      : cell_library_entry_(cell_library_entry),
        name_(name),
        inputs_(std::move(inputs)),
        outputs_(std::move(outputs)),
        internal_pins_(std::move(internal_pins)),
        clock_(clock) {}

  const AbstractCellLibraryEntry<EvalT>* cell_library_entry_;
  std::string name_;  // Instance name.
  std::vector<Pin> inputs_;
  std::vector<Pin> outputs_;
  std::vector<Pin> internal_pins_;
  absl::optional<AbstractNetRef<EvalT>> clock_;
};

using Cell = AbstractCell<>;

// Definition of a net. Note this may be augmented with a def/use chain in the
// future.
template <typename EvalT>
class AbstractNetDef {
 public:
  explicit AbstractNetDef(absl::string_view name) : name_(name) {}

  const std::string& name() const { return name_; }

  // Called to note that a cell is connected to this net.
  void NoteConnectedCell(AbstractCell<EvalT>* cell) {
    connected_cells_.push_back(cell);
  }

  absl::Span<AbstractCell<EvalT>* const> connected_cells() const {
    return connected_cells_;
  }

  // Helper for getting the connected cells without one that is known to be
  // connected (e.g. a driver). Note: could be optimized to give a smart
  // view/iterator object that filters out to_remove without instantiating
  // storage.
  absl::StatusOr<std::vector<AbstractCell<EvalT>*>> GetConnectedCellsSans(
      AbstractCell<EvalT>* to_remove) const;

 private:
  std::string name_;
  std::vector<AbstractCell<EvalT>*> connected_cells_;
};

using NetDef = AbstractNetDef<>;

// Kinds of wire declarations that can be made in the netlist module.
enum class NetDeclKind {
  kInput,
  kOutput,
  kWire,
};

// Represents the module containing the netlist info.
template <typename EvalT = bool>
class AbstractModule {
 public:
  explicit AbstractModule(absl::string_view name) : name_(name) {
    // Zero and one values are present in netlists as cell inputs (which we
    // interpret as wires), but aren't explicitly declared, so we create them as
    // wires here.
    zero_ = AddOrResolveNumber(0).value();
    one_ = AddOrResolveNumber(1).value();

    // We need a "dummy" wire to serve as the sink for any unused cell outputs.
    // Even if a cell output is unused, we need some dummy value there to
    // maintain the correspondance between a AbstractCellLibraryEntry's pinout
    // and that of a AbstractCell [object].
    // TODO(rspringer): Improve APIs so we don't have to match array indexes
    // between these types.
    constexpr const char kDummyName[] = "__dummy__net_decl__";
    XLS_CHECK_OK(AddNetDecl(NetDeclKind::kWire, kDummyName));
    dummy_ = ResolveNet(kDummyName).value();
  }

  const std::string& name() const { return name_; }

  // Returns a representation of this module as a AbstractCellLibraryEntry.
  // This does not currently support stateful modules, e.g., those with
  // "state_table"-like attributes.
  const AbstractCellLibraryEntry<EvalT>* AsCellLibraryEntry() const;

  absl::StatusOr<AbstractCell<EvalT>*> AddCell(AbstractCell<EvalT> cell);

  absl::Status AddNetDecl(NetDeclKind kind, absl::string_view name);

  absl::Status AddAssignDecl(absl::string_view name, bool bit);
  absl::Status AddAssignDecl(absl::string_view lhs_name,
                             absl::string_view rhs_name);

  // Returns a AbstractNetRef to the given number, creating a AbstractNetDef if
  // necessary.
  absl::StatusOr<AbstractNetRef<EvalT>> AddOrResolveNumber(int64_t number);

  absl::StatusOr<AbstractNetRef<EvalT>> ResolveNumber(int64_t number) const;

  absl::StatusOr<AbstractNetRef<EvalT>> ResolveNet(
      absl::string_view name) const;

  // Returns a reference to a "dummy" net - this is needed for cases where one
  // of a cell's output pins isn't actually used.
  AbstractNetRef<EvalT> GetDummyRef() const { return dummy_; }

  absl::StatusOr<AbstractCell<EvalT>*> ResolveCell(
      absl::string_view name) const;

  absl::Span<const std::unique_ptr<AbstractNetDef<EvalT>>> nets() const {
    return nets_;
  }
  absl::Span<const std::unique_ptr<AbstractCell<EvalT>>> cells() const {
    return cells_;
  }

  const std::vector<AbstractNetRef<EvalT>>& inputs() const { return inputs_; }
  const std::vector<AbstractNetRef<EvalT>>& outputs() const { return outputs_; }
  const absl::flat_hash_map<AbstractNetRef<EvalT>, AbstractNetRef<EvalT>>&
  assigns() const {
    return assigns_;
  }

  // Declares port order in the module() keyword.  For example, if a module
  // declaration starts with:
  //
  // module ifte(i, t, e, out);
  //     input [7:0] e;
  //     input i;
  //     output [7:0] out;
  //     input [7:0] t;
  //
  // You can invoke this method with the input { "i", "t", "e", "out" }
  //
  // If you construct a module programmatically then you do not need to invoke
  // this method, as you control the order of subsequent port declarations.
  // However, when parsing a module, it may be necessary to know the invocation
  // order in the module.
  void DeclarePortsOrder(absl::Span<const std::string> ports) {
    for (int i = 0; i < ports.size(); i++) {
      ports_.emplace_back(std::make_unique<Port>(ports[i]));
    }
  }

  // Declares an individual port with its direction and width.  For example, if
  // a module declaration starts with:
  //
  // module ifte(i, t, e, out);
  //     input [7:0] e;
  //     input i;
  //     output [7:0] out;
  //     input [7:0] t;
  //
  // You can invoke this method while parsing the module.  You would invoke it
  // each time you encounter the "input" or "output" keywords.
  //
  // Note that, as the example shows, the order of port declarations in the
  // source may be different from their order in the module keyword.
  //
  // An error status is returned if, for a given "input" or "output"
  // declaration, there no match in the parameter list.
  absl::Status DeclarePort(absl::string_view name, int64_t width,
                           bool is_output);

  // Returns the bit offset of a given input net in the parameter list.  For
  // example, if a module declaration starts with:
  //
  // module ifte(i, t, e, out);
  //     input [7:0] e;
  //     input i;
  //     output [7:0] out;
  //     input [7:0] t;
  // module ifte(i, t, e, out);
  //
  // When parsing a module invokation, you may want to assign input values to
  // the individual input ports.  As you will be working with individual wires
  // at that level (AbstractNetDef instances), you will want to know what is the
  // offset of e.g. AbstractNetDef "t[3]".  This method will compute that
  // offset.
  //
  // DeclarePortsOrder() needs to have been called previously.
  int64_t GetInputPortOffset(absl::string_view name) const;

 private:
  struct Port {
    explicit Port(std::string name) : name_(name) {}
    std::string name_;
    int64_t width_ = 1;
    bool is_output_ = false;
    bool is_declared_ = false;
  };

  std::string name_;
  std::vector<std::unique_ptr<Port>> ports_;
  std::vector<AbstractNetRef<EvalT>> inputs_;
  std::vector<AbstractNetRef<EvalT>> outputs_;
  std::vector<AbstractNetRef<EvalT>> wires_;
  absl::flat_hash_map<AbstractNetRef<EvalT>, AbstractNetRef<EvalT>> assigns_;
  std::vector<std::unique_ptr<AbstractNetDef<EvalT>>> nets_;
  std::vector<std::unique_ptr<AbstractCell<EvalT>>> cells_;
  AbstractNetRef<EvalT> zero_;
  AbstractNetRef<EvalT> one_;
  AbstractNetRef<EvalT> dummy_;

  mutable absl::optional<AbstractCellLibraryEntry<EvalT>> cell_library_entry_;
};

using Module = AbstractModule<>;

// An AbstractNetlist contains all modules present in a single file.
template <typename EvalT = bool>
class AbstractNetlist {
 public:
  void AddModule(std::unique_ptr<AbstractModule<EvalT>> module);
  absl::StatusOr<const AbstractModule<EvalT>*> GetModule(
      const std::string& module_name) const;
  const absl::Span<const std::unique_ptr<AbstractModule<EvalT>>> modules() {
    return modules_;
  }
  absl::StatusOr<const AbstractCellLibraryEntry<EvalT>*>
  GetOrCreateLut4CellEntry(int64_t lut_mask, EvalT zero, EvalT one);

  template <typename = std::is_constructible<EvalT, bool>>
  absl::StatusOr<const AbstractCellLibraryEntry<EvalT>*>
  GetOrCreateLut4CellEntry(int64_t lut_mask) {
    return GetOrCreateLut4CellEntry(lut_mask, EvalT{false}, EvalT{true});
  }

 private:
  // The AbstractNetlist itself manages the CellLibraryEntries corresponding to
  // the LUT4 cells that are used, which are identified by their LUT mask (i.e.
  // the 16 bit LUT_INIT parameter).
  absl::flat_hash_map<uint16_t, AbstractCellLibraryEntry<EvalT>> lut_cells_;
  std::vector<std::unique_ptr<AbstractModule<EvalT>>> modules_;
};

template <typename EvalT>
const AbstractCellLibraryEntry<EvalT>*
AbstractModule<EvalT>::AsCellLibraryEntry() const {
  if (!cell_library_entry_.has_value()) {
    std::vector<std::string> input_names;
    input_names.reserve(inputs_.size());
    for (const auto& input : inputs_) {
      input_names.push_back(input->name());
    }
    typename AbstractCellLibraryEntry<EvalT>::OutputPinToFunction output_pins;
    output_pins.reserve(outputs_.size());
    for (const auto& output : outputs_) {
      output_pins[output->name()] = "";
    }
    cell_library_entry_.emplace(AbstractCellLibraryEntry<EvalT>(
        CellKind::kOther, name_, input_names, output_pins, absl::nullopt));
  }
  return &cell_library_entry_.value();
}

template <typename EvalT>
absl::StatusOr<AbstractNetRef<EvalT>> AbstractModule<EvalT>::AddOrResolveNumber(
    int64_t number) {
  auto status_or_ref = ResolveNumber(number);
  if (status_or_ref.ok()) {
    return status_or_ref.value();
  }

  std::string wire_name = absl::StrFormat("<constant_%d>", number);
  XLS_RETURN_IF_ERROR(AddNetDecl(NetDeclKind::kWire, wire_name));
  return ResolveNet(wire_name);
}

template <typename EvalT>
absl::StatusOr<AbstractNetRef<EvalT>> AbstractModule<EvalT>::ResolveNumber(
    int64_t number) const {
  std::string wire_name = absl::StrFormat("<constant_%d>", number);
  return ResolveNet(wire_name);
}

template <typename EvalT>
absl::StatusOr<AbstractNetRef<EvalT>> AbstractModule<EvalT>::ResolveNet(
    absl::string_view name) const {
  for (const auto& net : nets_) {
    if (net->name() == name) {
      return net.get();
    }
  }

  return absl::NotFoundError(absl::StrCat("Could not find net: ", name));
}

template <typename EvalT>
absl::StatusOr<AbstractCell<EvalT>*> AbstractModule<EvalT>::ResolveCell(
    absl::string_view name) const {
  for (const auto& cell : cells_) {
    if (cell->name() == name) {
      return cell.get();
    }
  }
  return absl::NotFoundError(
      absl::StrCat("Could not find cell with name: ", name));
}

template <typename EvalT>
absl::StatusOr<AbstractCell<EvalT>*> AbstractModule<EvalT>::AddCell(
    AbstractCell<EvalT> cell) {
  auto status_or_cell = ResolveCell(cell.name());
  if (status_or_cell.status().ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Module already has a cell with name: ", cell.name()));
  }

  cells_.push_back(std::make_unique<AbstractCell<EvalT>>(cell));
  return cells_.back().get();
}

template <typename EvalT>
absl::Status AbstractModule<EvalT>::AddNetDecl(NetDeclKind kind,
                                               absl::string_view name) {
  auto status_or_net = ResolveNet(name);
  if (status_or_net.status().ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Module already has a net/wire decl with name: ", name));
  }

  nets_.emplace_back(std::make_unique<AbstractNetDef<EvalT>>(name));
  AbstractNetRef<EvalT> ref = nets_.back().get();
  switch (kind) {
    case NetDeclKind::kInput:
      inputs_.push_back(ref);
      break;
    case NetDeclKind::kOutput:
      outputs_.push_back(ref);
      break;
    case NetDeclKind::kWire:
      wires_.push_back(ref);
      break;
  }
  return absl::OkStatus();
}

template <typename EvalT>
absl::Status AbstractModule<EvalT>::AddAssignDecl(absl::string_view name,
                                                  bool bit) {
  XLS_ASSIGN_OR_RETURN(AbstractNetRef<EvalT> ref, ResolveNet(name));
  assigns_[ref] = bit ? one_ : zero_;
  return absl::OkStatus();
}

template <typename EvalT>
absl::Status AbstractModule<EvalT>::AddAssignDecl(absl::string_view lhs_name,
                                                  absl::string_view rhs_name) {
  XLS_ASSIGN_OR_RETURN(AbstractNetRef<EvalT> lhs, ResolveNet(lhs_name));
  XLS_ASSIGN_OR_RETURN(AbstractNetRef<EvalT> rhs, ResolveNet(rhs_name));
  assigns_[lhs] = rhs;
  return absl::OkStatus();
}

template <typename EvalT>
absl::Status AbstractModule<EvalT>::DeclarePort(absl::string_view name,
                                                int64_t width, bool is_output) {
  for (auto& port : ports_) {
    if (port->name_ == name) {
      if (port->is_declared_) {
        return absl::AlreadyExistsError(
            absl::StrFormat("Duplicate declaration of port '%s'.", name));
      }
      port->width_ = width;
      port->is_output_ = is_output;
      port->is_declared_ = true;
      return absl::OkStatus();
    }
  }
  return absl::NotFoundError(
      absl::StrFormat("No match for %s '%s' in parameter list.",
                      is_output ? "output" : "input", name));
}

template <typename EvalT>
int64_t AbstractModule<EvalT>::GetInputPortOffset(
    absl::string_view name) const {
  // The input is either a name, e.g. "a", or a name + subscript, e.g. "a[3]".
  std::vector<std::string> name_and_idx = absl::StrSplit(name, '[');
  XLS_CHECK(name_and_idx.size() <= 2);

  int i;
  int off = 0;
  for (i = 0; i < ports_.size(); i++) {
    if (ports_[i]->is_output_) {
      continue;
    }
    off += ports_[i]->width_;
    if (ports_[i]->name_ == name_and_idx[0]) {
      break;
    }
  }
  XLS_CHECK(i < ports_.size());

  if (name_and_idx.size() == 2) {
    absl::string_view idx = absl::StripSuffix(name_and_idx[1], "]");
    int64_t idx_out;
    XLS_CHECK(absl::SimpleAtoi(idx, &idx_out));
    off -= idx_out;
  }

  return off - 1;
}

template <typename EvalT>
absl::StatusOr<std::vector<AbstractCell<EvalT>*>>
AbstractNetDef<EvalT>::GetConnectedCellsSans(
    AbstractCell<EvalT>* to_remove) const {
  std::vector<AbstractCell<EvalT>*> new_cells;
  new_cells.reserve(connected_cells_.size() - 1);
  bool found = false;
  for (int i = 0; i < connected_cells_.size(); i++) {
    if (connected_cells_[i] == to_remove) {
      found = true;
    } else {
      new_cells.push_back(connected_cells_[i]);
    }
  }

  if (!found) {
    return absl::NotFoundError("Could not find cell in connected cell set: " +
                               to_remove->name());
  }
  return new_cells;
}

template <typename EvalT>
/* static */ absl::StatusOr<AbstractCell<EvalT>> AbstractCell<EvalT>::Create(
    const AbstractCellLibraryEntry<EvalT>* cell_library_entry,
    absl::string_view name,
    const absl::flat_hash_map<std::string, AbstractNetRef<EvalT>>&
        named_parameter_assignments,
    absl::optional<AbstractNetRef<EvalT>> clock,
    const AbstractNetRef<EvalT> dummy_net) {
  auto sorted_key_str = [named_parameter_assignments]() -> std::string {
    std::vector<std::string> keys;
    for (const auto& item : named_parameter_assignments) {
      keys.push_back(item.first);
    }
    std::sort(keys.begin(), keys.end());
    return "[" + absl::StrJoin(keys, ", ") + "]";
  };

  std::vector<Pin> cell_inputs;
  for (const std::string& input : cell_library_entry->input_names()) {
    auto it = named_parameter_assignments.find(input);
    if (it == named_parameter_assignments.end()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Missing named input parameter in instantiation: %s; got: %s", input,
          sorted_key_str()));
    }
    Pin cell_input;
    cell_input.name = input;
    cell_input.netref = it->second;
    cell_inputs.push_back(cell_input);
  }

  const typename AbstractCellLibraryEntry<EvalT>::OutputPinToFunction&
      output_pins = cell_library_entry->output_pin_to_function();
  std::vector<Pin> cell_outputs;
  for (const auto& kv : output_pins) {
    Pin cell_output;
    cell_output.name = kv.first;
    auto it = named_parameter_assignments.find(cell_output.name);
    if (it == named_parameter_assignments.end()) {
      cell_output.netref = dummy_net;
    } else {
      cell_output.netref = it->second;
    }
    cell_outputs.push_back(cell_output);
  }

  std::vector<Pin> internal_pins;
  if (cell_library_entry->state_table().has_value()) {
    const AbstractStateTable<EvalT>& state_table =
        cell_library_entry->state_table().value();
    for (const std::string& signal : state_table.internal_signals()) {
      Pin pin;
      pin.name = signal;
      // Synthesize "fake" wire?
      pin.netref = nullptr;
      internal_pins.push_back(pin);
    }
  }

  if (cell_library_entry->clock_name().has_value() && !clock.has_value()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Missing clock parameter %s in instantiation; got inputs: %s.",
        cell_library_entry->clock_name().value(), sorted_key_str()));
  }

  return AbstractCell<EvalT>(cell_library_entry, name, std::move(cell_inputs),
                             std::move(cell_outputs), std::move(internal_pins),
                             clock);
}

template <typename EvalT>
void AbstractNetlist<EvalT>::AddModule(
    std::unique_ptr<AbstractModule<EvalT>> module) {
  modules_.emplace_back(std::move(module));
}

template <typename EvalT>
absl::StatusOr<const AbstractModule<EvalT>*> AbstractNetlist<EvalT>::GetModule(
    const std::string& module_name) const {
  for (const auto& module : modules_) {
    if (module->name() == module_name) {
      return module.get();
    }
  }
  return absl::NotFoundError(
      absl::StrFormat("Module %s not found in netlist.", module_name));
}

template <typename EvalT>
absl::StatusOr<const AbstractCellLibraryEntry<EvalT>*>
AbstractNetlist<EvalT>::GetOrCreateLut4CellEntry(int64_t lut_mask, EvalT zero,
                                                 EvalT one) {
  if ((lut_mask & Mask(16)) != lut_mask) {
    return absl::InvalidArgumentError("Mask for LUT4 must be 16 bits");
  }
  uint16_t mask = static_cast<uint16_t>(lut_mask);
  auto it = lut_cells_.find(mask);
  if (it == lut_cells_.end()) {
    AbstractCellLibraryEntry<EvalT> entry(
        // The resulting LUT could represent a defined CellKind e.g. kXor
        // but since we currently don't "pattern match" the mask against known
        // functions, we just use kOther for every mask.
        CellKind::kOther,
        /*name*/ absl::StrFormat("<lut_0x%04x>", mask),
        /*input_names*/ std::vector<std::string>{"I0", "I1", "I3"},
        /*output_pin_to_function*/ {{"O", "X"}},
        AbstractStateTable<EvalT>::FromLutMask(mask, zero, one));
    auto result = lut_cells_.insert({mask, entry});
    return &(result.first->second);
  }
  return &it->second;
}

using Netlist = AbstractNetlist<>;

}  // namespace rtl
}  // namespace netlist
}  // namespace xls

#endif  // XLS_NETLIST_NETLIST_H_
