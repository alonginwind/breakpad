// Copyright 2013 Google LLC
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

// stackwalker_address_list.cc: a pseudo stack walker.
//
// See stackwalker_address_list.h for documentation.
//
// Author: Chris Hamilton <chrisha@chromium.org>

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <assert.h>

#include <vector>

#include "google_breakpad/processor/call_stack.h"
#include "google_breakpad/processor/memory_region.h"
#include "google_breakpad/processor/source_line_resolver_interface.h"
#include "google_breakpad/processor/stack_frame.h"
#include "processor/logging.h"
#include "processor/stackwalker_address_list.h"

namespace google_breakpad {

StackwalkerAddressList::StackwalkerAddressList(
    const uint64_t* frames,
    size_t frame_count,
    const CodeModules* modules,
    StackFrameSymbolizer* frame_symbolizer)
    : Stackwalker(nullptr, nullptr, modules, frame_symbolizer),
      frames_(frames),
      frame_count_(frame_count),
      next_frame_index_(0) {
  assert(frames);
  assert(frame_symbolizer);
}

StackFrame* StackwalkerAddressList::GetContextFrame() {
  if (frame_count_ == 0)
    return nullptr;

  StackFrame* frame = new StackFrame();
  frame->instruction = frames_[0];
  frame->trust = StackFrame::FRAME_TRUST_PREWALKED;

  next_frame_index_ = 1;

  return frame;
}

StackFrame* StackwalkerAddressList::GetCallerFrame(const CallStack*,
                                                   bool stack_scan_allowed) {
  // There are no more frames to fetch.
  if (next_frame_index_ >= frame_count_)
    return nullptr;

  // All frames have the highest level of trust because they were
  // explicitly provided.
  StackFrame* frame = new StackFrame();
  frame->instruction = frames_[next_frame_index_++];
  frame->trust = StackFrame::FRAME_TRUST_PREWALKED;
  return frame;
}

}  // namespace google_breakpad
