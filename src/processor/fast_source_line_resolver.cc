// Copyright 2010 Google LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// fast_source_line_resolver.cc: FastSourceLineResolver is a concrete class that
// implements SourceLineResolverInterface.  Both FastSourceLineResolver and
// BasicSourceLineResolver inherit from SourceLineResolverBase class to reduce
// code redundancy.
//
// See fast_source_line_resolver.h and fast_source_line_resolver_types.h
// for more documentation.
//
// Author: Siyang Xie (lambxsy@google.com)

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include "google_breakpad/processor/fast_source_line_resolver.h"

#include <assert.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "common/using_std_string.h"
#include "processor/fast_source_line_resolver_types.h"
#include "processor/logging.h"
#include "processor/module_factory.h"
#include "processor/simple_serializer-inl.h"

using std::deque;
using std::unique_ptr;

namespace google_breakpad {

FastSourceLineResolver::FastSourceLineResolver()
  : SourceLineResolverBase(new FastModuleFactory) { }

bool FastSourceLineResolver::ShouldDeleteMemoryBufferAfterLoadModule() {
  return false;
}

void FastSourceLineResolver::Module::LookupAddress(
    StackFrame* frame,
    std::deque<std::unique_ptr<StackFrame>>* inlined_frames) const {
  MemAddr address = frame->instruction - frame->module->base_address();

  // First, look for a FUNC record that covers address. Use
  // RetrieveNearestRange instead of RetrieveRange so that, if there
  // is no such function, we can use the next function to bound the
  // extent of the PUBLIC symbol we find, below. This does mean we
  // need to check that address indeed falls within the function we
  // find; do the range comparison in an overflow-friendly way.
  std::unique_ptr<Function> func(new Function);
  const Function* func_ptr = 0;
  std::unique_ptr<PublicSymbol> public_symbol(new PublicSymbol);
  const PublicSymbol* public_symbol_ptr = 0;
  MemAddr function_base;
  MemAddr function_size;
  MemAddr public_address;

  if (functions_.RetrieveNearestRange(address, func_ptr,
                                      &function_base, &function_size) &&
      address >= function_base && address - function_base < function_size) {
    func->CopyFrom(func_ptr);
    frame->function_name = func->name;
    frame->function_base = frame->module->base_address() + function_base;
    frame->is_multiple = func->is_multiple;

    std::unique_ptr<Line> line(new Line);
    const Line* line_ptr = 0;
    MemAddr line_base;
    if (func->lines.RetrieveRange(address, line_ptr, &line_base, nullptr)) {
      line->CopyFrom(line_ptr);
      FileMap::iterator it = files_.find(line->source_file_id);
      if (it != files_.end()) {
        frame->source_file_name =
            files_.find(line->source_file_id).GetValuePtr();
      }
      frame->source_line = line->line;
      frame->source_line_base = frame->module->base_address() + line_base;
    }
    // Check if this is inlined function call.
    if (inlined_frames) {
      ConstructInlineFrames(frame, address, func->inlines, inlined_frames);
    }
  } else if (public_symbols_.Retrieve(address,
                                      public_symbol_ptr, &public_address) &&
             (!func_ptr || public_address > function_base)) {
    public_symbol->CopyFrom(public_symbol_ptr);
    frame->function_name = public_symbol->name;
    frame->function_base = frame->module->base_address() + public_address;
    frame->is_multiple = public_symbol->is_multiple;
  }
}

void FastSourceLineResolver::Module::ConstructInlineFrames(
    StackFrame* frame,
    MemAddr address,
    const StaticContainedRangeMap<MemAddr, char>& inline_map,
    std::deque<std::unique_ptr<StackFrame>>* inlined_frames) const {
  std::vector<const char*> inline_ptrs;
  if (!inline_map.RetrieveRanges(address, inline_ptrs)) {
    return;
  }

  for (const char* inline_ptr : inline_ptrs) {
    std::unique_ptr<Inline> in(new Inline);
    in->CopyFrom(inline_ptr);
    unique_ptr<StackFrame> new_frame =
        unique_ptr<StackFrame>(new StackFrame(*frame));
    auto origin_iter = inline_origins_.find(in->origin_id);
    if (origin_iter != inline_origins_.end()) {
      std::unique_ptr<InlineOrigin> origin(new InlineOrigin);
      origin->CopyFrom(origin_iter.GetValuePtr());
      new_frame->function_name = origin->name;
    } else {
      new_frame->function_name = "<name omitted>";
    }

    // Store call site file and line in current frame, which will be updated
    // later.
    new_frame->source_line = in->call_site_line;
    if (in->has_call_site_file_id) {
      auto file_iter = files_.find(in->call_site_file_id);
      if (file_iter != files_.end()) {
        new_frame->source_file_name = file_iter.GetValuePtr();
      }
    }

    // Use the starting address of the inlined range as inlined function base.
    new_frame->function_base = new_frame->module->base_address();
    for (const auto& range : in->inline_ranges) {
      if (address >= range.first && address < range.first + range.second) {
        new_frame->function_base += range.first;
        break;
      }
    }
    new_frame->trust = StackFrame::FRAME_TRUST_INLINE;

    // The inlines vector has an order from innermost entry to outermost entry.
    // By push_back, we will have inlined_frames with the same order.
    inlined_frames->push_back(std::move(new_frame));
  }

  // Update the source file and source line for each inlined frame.
  if (!inlined_frames->empty()) {
    string parent_frame_source_file_name = frame->source_file_name;
    int parent_frame_source_line = frame->source_line;
    frame->source_file_name = inlined_frames->back()->source_file_name;
    frame->source_line = inlined_frames->back()->source_line;
    for (unique_ptr<StackFrame>& inlined_frame : *inlined_frames) {
      std::swap(inlined_frame->source_file_name, parent_frame_source_file_name);
      std::swap(inlined_frame->source_line, parent_frame_source_line);
    }
  }
}

// WFI: WindowsFrameInfo.
// Returns a WFI object reading from a raw memory chunk of data
WindowsFrameInfo FastSourceLineResolver::CopyWFI(const char* raw) {
  const WindowsFrameInfo::StackInfoTypes type =
     static_cast<const WindowsFrameInfo::StackInfoTypes>(
         *reinterpret_cast<const int32_t*>(raw));

  // The first 8 bytes of int data are unused.
  // They correspond to "StackInfoTypes type_;" and "int valid;"
  // data member of WFI.
  const uint32_t* para_uint32 = reinterpret_cast<const uint32_t*>(
      raw + 2 * sizeof(int32_t));

  uint32_t prolog_size = para_uint32[0];;
  uint32_t epilog_size = para_uint32[1];
  uint32_t parameter_size = para_uint32[2];
  uint32_t saved_register_size = para_uint32[3];
  uint32_t local_size = para_uint32[4];
  uint32_t max_stack_size = para_uint32[5];
  const char* boolean = reinterpret_cast<const char*>(para_uint32 + 6);
  bool allocates_base_pointer = (*boolean != 0);
  string program_string = boolean + 1;

  return WindowsFrameInfo(type,
                          prolog_size,
                          epilog_size,
                          parameter_size,
                          saved_register_size,
                          local_size,
                          max_stack_size,
                          allocates_base_pointer,
                          program_string);
}

// Loads a map from the given buffer in char* type.
// Does NOT take ownership of mem_buffer.
// In addition, treat mem_buffer as const char*.
bool FastSourceLineResolver::Module::LoadMapFromMemory(
    char* memory_buffer,
    size_t memory_buffer_size) {
  if (!memory_buffer) return false;

  // Read the "is_corrupt" flag.
  const char* mem_buffer = memory_buffer;
  mem_buffer = SimpleSerializer<bool>::Read(mem_buffer, &is_corrupt_);

  const uint64_t* map_sizes = reinterpret_cast<const uint64_t*>(mem_buffer);

  unsigned int header_size = kNumberMaps_ * sizeof(uint64_t);

  // offsets[]: an array of offset addresses (with respect to mem_buffer),
  // for each "Static***Map" component of Module.
  // "Static***Map": static version of std::map or map wrapper, i.e., StaticMap,
  // StaticAddressMap, StaticContainedRangeMap, and StaticRangeMap.
  uint64_t offsets[kNumberMaps_];
  offsets[0] = header_size;
  for (int i = 1; i < kNumberMaps_; ++i) {
    offsets[i] = offsets[i - 1] + map_sizes[i - 1];
  }
  size_t expected_size = sizeof(bool) + offsets[kNumberMaps_ - 1] +
                         map_sizes[kNumberMaps_ - 1] + 1;
  if (expected_size != memory_buffer_size &&
      // Allow for having an extra null terminator.
      expected_size != memory_buffer_size - 1) {
    // This could either be a random corruption or the serialization format was
    // changed without updating the version in kSerializedBreakpadFileExtension.
    BPLOG(ERROR) << "Memory buffer is either corrupt or an unsupported version"
                 << ", expected size: " << expected_size
                 << ", actual size: " << memory_buffer_size;
    return false;
  }
  BPLOG(INFO) << "Memory buffer size looks good, size: " << memory_buffer_size;

  // Use pointers to construct Static*Map data members in Module:
  int map_id = 0;
  files_ = StaticMap<int, char>(mem_buffer + offsets[map_id++]);
  functions_ =
      StaticRangeMap<MemAddr, Function>(mem_buffer + offsets[map_id++]);
  public_symbols_ =
      StaticAddressMap<MemAddr, PublicSymbol>(mem_buffer + offsets[map_id++]);
  for (int i = 0; i < WindowsFrameInfo::STACK_INFO_LAST; ++i) {
    windows_frame_info_[i] =
        StaticContainedRangeMap<MemAddr, char>(mem_buffer + offsets[map_id++]);
  }

  cfi_initial_rules_ =
      StaticRangeMap<MemAddr, char>(mem_buffer + offsets[map_id++]);
  cfi_delta_rules_ = StaticMap<MemAddr, char>(mem_buffer + offsets[map_id++]);
  inline_origins_ = StaticMap<int, char>(mem_buffer + offsets[map_id++]);
  return true;
}

WindowsFrameInfo* FastSourceLineResolver::Module::FindWindowsFrameInfo(
    const StackFrame* frame) const {
  MemAddr address = frame->instruction - frame->module->base_address();
  std::unique_ptr<WindowsFrameInfo> result(new WindowsFrameInfo());

  // We only know about WindowsFrameInfo::STACK_INFO_FRAME_DATA and
  // WindowsFrameInfo::STACK_INFO_FPO. Prefer them in this order.
  // WindowsFrameInfo::STACK_INFO_FRAME_DATA is the newer type that
  // includes its own program string.
  // WindowsFrameInfo::STACK_INFO_FPO is the older type
  // corresponding to the FPO_DATA struct. See stackwalker_x86.cc.
  const char* frame_info_ptr;
  if ((windows_frame_info_[WindowsFrameInfo::STACK_INFO_FRAME_DATA]
       .RetrieveRange(address, frame_info_ptr))
      || (windows_frame_info_[WindowsFrameInfo::STACK_INFO_FPO]
          .RetrieveRange(address, frame_info_ptr))) {
    result->CopyFrom(CopyWFI(frame_info_ptr));
    return result.release();
  }

  // Even without a relevant STACK line, many functions contain
  // information about how much space their parameters consume on the
  // stack. Use RetrieveNearestRange instead of RetrieveRange, so that
  // we can use the function to bound the extent of the PUBLIC symbol,
  // below. However, this does mean we need to check that ADDRESS
  // falls within the retrieved function's range; do the range
  // comparison in an overflow-friendly way.
  std::unique_ptr<Function> function(new Function);
  const Function* function_ptr = 0;
  MemAddr function_base, function_size;
  if (functions_.RetrieveNearestRange(address, function_ptr,
                                      &function_base, &function_size) &&
      address >= function_base && address - function_base < function_size) {
    function->CopyFrom(function_ptr);
    result->parameter_size = function->parameter_size;
    result->valid |= WindowsFrameInfo::VALID_PARAMETER_SIZE;
    return result.release();
  }

  // PUBLIC symbols might have a parameter size. Use the function we
  // found above to limit the range the public symbol covers.
  std::unique_ptr<PublicSymbol> public_symbol(new PublicSymbol);
  const PublicSymbol* public_symbol_ptr = 0;
  MemAddr public_address;
  if (public_symbols_.Retrieve(address, public_symbol_ptr, &public_address) &&
      (!function_ptr || public_address > function_base)) {
    public_symbol->CopyFrom(public_symbol_ptr);
    result->parameter_size = public_symbol->parameter_size;
  }

  return nullptr;
}

CFIFrameInfo* FastSourceLineResolver::Module::FindCFIFrameInfo(
    const StackFrame* frame) const {
  MemAddr address = frame->instruction - frame->module->base_address();
  MemAddr initial_base, initial_size;
  const char* initial_rules = nullptr;

  // Find the initial rule whose range covers this address. That
  // provides an initial set of register recovery rules. Then, walk
  // forward from the initial rule's starting address to frame's
  // instruction address, applying delta rules.
  if (!cfi_initial_rules_.RetrieveRange(address, initial_rules,
                                        &initial_base, &initial_size)) {
    return nullptr;
  }

  // Create a frame info structure, and populate it with the rules from
  // the STACK CFI INIT record.
  std::unique_ptr<CFIFrameInfo> rules(new CFIFrameInfo());
  if (!ParseCFIRuleSet(initial_rules, rules.get()))
    return nullptr;

  // Find the first delta rule that falls within the initial rule's range.
  StaticMap<MemAddr, char>::iterator delta =
    cfi_delta_rules_.lower_bound(initial_base);

  // Apply delta rules up to and including the frame's address.
  while (delta != cfi_delta_rules_.end() && delta.GetKey() <= address) {
    ParseCFIRuleSet(delta.GetValuePtr(), rules.get());
    delta++;
  }

  return rules.release();
}

}  // namespace google_breakpad
