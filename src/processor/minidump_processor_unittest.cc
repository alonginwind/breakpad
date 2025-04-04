// Copyright 2006 Google LLC
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

// Unit test for MinidumpProcessor.  Uses a pre-generated minidump and
// corresponding symbol file, and checks the stack frames for correctness.

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <stdlib.h>

#include <string>
#include <iostream>
#include <fstream>
#include <map>
#include <utility>

#include "breakpad_googletest_includes.h"
#include "common/using_std_string.h"
#include "google_breakpad/processor/basic_source_line_resolver.h"
#include "google_breakpad/processor/call_stack.h"
#include "google_breakpad/processor/code_module.h"
#include "google_breakpad/processor/code_modules.h"
#include "google_breakpad/processor/minidump.h"
#include "google_breakpad/processor/minidump_processor.h"
#include "google_breakpad/processor/process_state.h"
#include "google_breakpad/processor/stack_frame.h"
#include "google_breakpad/processor/symbol_supplier.h"
#include "processor/logging.h"
#include "processor/stackwalker_unittest_utils.h"

using std::map;

namespace google_breakpad {
class MockMinidump : public Minidump {
 public:
  MockMinidump() : Minidump("") {
  }

  MOCK_METHOD0(Read, bool());
  MOCK_CONST_METHOD0(path, string());
  MOCK_CONST_METHOD0(header, const MDRawHeader*());
  MOCK_METHOD0(GetThreadList, MinidumpThreadList*());
  MOCK_METHOD0(GetSystemInfo, MinidumpSystemInfo*());
  MOCK_METHOD0(GetMiscInfo, MinidumpMiscInfo*());
  MOCK_METHOD0(GetBreakpadInfo, MinidumpBreakpadInfo*());
  MOCK_METHOD0(GetException, MinidumpException*());
  MOCK_METHOD0(GetAssertion, MinidumpAssertion*());
  MOCK_METHOD0(GetModuleList, MinidumpModuleList*());
  MOCK_METHOD0(GetUnloadedModuleList, MinidumpUnloadedModuleList*());
  MOCK_METHOD0(GetMemoryList, MinidumpMemoryList*());
};

class MockMinidumpUnloadedModule : public MinidumpUnloadedModule {
 public:
  MockMinidumpUnloadedModule() : MinidumpUnloadedModule(nullptr) {}
};

class MockMinidumpUnloadedModuleList : public MinidumpUnloadedModuleList {
 public:
  MockMinidumpUnloadedModuleList() : MinidumpUnloadedModuleList(nullptr) {}

  ~MockMinidumpUnloadedModuleList() {}
  MOCK_CONST_METHOD0(Copy, CodeModules*());
  MOCK_CONST_METHOD1(GetModuleForAddress,
                     const MinidumpUnloadedModule*(uint64_t));
};

class MockMinidumpThreadList : public MinidumpThreadList {
 public:
  MockMinidumpThreadList() : MinidumpThreadList(nullptr) {}

  MOCK_CONST_METHOD0(thread_count, unsigned int());
  MOCK_CONST_METHOD1(GetThreadAtIndex, MinidumpThread*(unsigned int));
};

class MockMinidumpMemoryList : public MinidumpMemoryList {
 public:
  MockMinidumpMemoryList() : MinidumpMemoryList(nullptr) {}

  MOCK_METHOD1(GetMemoryRegionForAddress, MinidumpMemoryRegion*(uint64_t));
};

class MockMinidumpThread : public MinidumpThread {
 public:
  MockMinidumpThread() : MinidumpThread(nullptr) {}

  MOCK_CONST_METHOD1(GetThreadID, bool(uint32_t*));
  MOCK_METHOD0(GetContext, MinidumpContext*());
  MOCK_METHOD0(GetMemory, MinidumpMemoryRegion*());
  MOCK_CONST_METHOD0(GetStartOfStackMemoryRange, uint64_t());
};

// This is crappy, but MinidumpProcessor really does want a
// MinidumpMemoryRegion.
class MockMinidumpMemoryRegion : public MinidumpMemoryRegion {
 public:
  MockMinidumpMemoryRegion(uint64_t base, const string& contents) :
      MinidumpMemoryRegion(nullptr) {
    region_.Init(base, contents);
  }

  uint64_t GetBase() const { return region_.GetBase(); }
  uint32_t GetSize() const { return region_.GetSize(); }

  bool GetMemoryAtAddress(uint64_t address, uint8_t*  value) const {
    return region_.GetMemoryAtAddress(address, value);
  }
  bool GetMemoryAtAddress(uint64_t address, uint16_t* value) const {
    return region_.GetMemoryAtAddress(address, value);
  }
  bool GetMemoryAtAddress(uint64_t address, uint32_t* value) const {
    return region_.GetMemoryAtAddress(address, value);
  }
  bool GetMemoryAtAddress(uint64_t address, uint64_t* value) const {
    return region_.GetMemoryAtAddress(address, value);
  }

  MockMemoryRegion region_;
};

// A test miscelaneous info stream, just returns values from the
// MDRawMiscInfo fed to it.
class TestMinidumpMiscInfo : public MinidumpMiscInfo {
 public:
  explicit TestMinidumpMiscInfo(const MDRawMiscInfo& misc_info) :
      MinidumpMiscInfo(nullptr) {
    valid_ = true;
    misc_info_ = misc_info;
  }
};

}  // namespace google_breakpad

namespace {

using google_breakpad::BasicSourceLineResolver;
using google_breakpad::CallStack;
using google_breakpad::CodeModule;
using google_breakpad::MinidumpContext;
using google_breakpad::MinidumpMemoryRegion;
using google_breakpad::MinidumpMiscInfo;
using google_breakpad::MinidumpProcessor;
using google_breakpad::MinidumpSystemInfo;
using google_breakpad::MinidumpThreadList;
using google_breakpad::MinidumpThread;
using google_breakpad::MockMinidump;
using google_breakpad::MockMinidumpMemoryList;
using google_breakpad::MockMinidumpMemoryRegion;
using google_breakpad::MockMinidumpThread;
using google_breakpad::MockMinidumpThreadList;
using google_breakpad::MockMinidumpUnloadedModule;
using google_breakpad::MockMinidumpUnloadedModuleList;
using google_breakpad::ProcessState;
using google_breakpad::SymbolSupplier;
using google_breakpad::SystemInfo;
using ::testing::_;
using ::testing::AnyNumber;
using ::testing::DoAll;
using ::testing::Mock;
using ::testing::Ne;
using ::testing::Property;
using ::testing::Return;
using ::testing::SetArgumentPointee;

static const char* kSystemInfoOS = "Windows NT";
static const char* kSystemInfoOSShort = "windows";
static const char* kSystemInfoOSVersion = "5.1.2600 Service Pack 2";
static const char* kSystemInfoCPU = "x86";
static const char* kSystemInfoCPUInfo =
    "GenuineIntel family 6 model 13 stepping 8";

#define ASSERT_TRUE_ABORT(cond) \
  if (!(cond)) {                                                        \
    fprintf(stderr, "FAILED: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
    abort(); \
  }

#define ASSERT_EQ_ABORT(e1, e2) ASSERT_TRUE_ABORT((e1) == (e2))

static string GetTestDataPath() {
  char* srcdir = getenv("srcdir");

  return string(srcdir ? srcdir : ".") + "/src/processor/testdata/";
}

class TestSymbolSupplier : public SymbolSupplier {
 public:
  TestSymbolSupplier() : interrupt_(false) {}

  virtual SymbolResult GetSymbolFile(const CodeModule* module,
                                     const SystemInfo* system_info,
                                     string* symbol_file);

  virtual SymbolResult GetSymbolFile(const CodeModule* module,
                                     const SystemInfo* system_info,
                                     string* symbol_file,
                                     string* symbol_data);

  virtual SymbolResult GetCStringSymbolData(const CodeModule* module,
                                            const SystemInfo* system_info,
                                            string* symbol_file,
                                            char** symbol_data,
                                            size_t* symbol_data_size);

  virtual void FreeSymbolData(const CodeModule* module);

  // When set to true, causes the SymbolSupplier to return INTERRUPT
  void set_interrupt(bool interrupt) { interrupt_ = interrupt; }

 private:
  bool interrupt_;
  map<string, char*> memory_buffers_;
};

SymbolSupplier::SymbolResult TestSymbolSupplier::GetSymbolFile(
    const CodeModule* module,
    const SystemInfo* system_info,
    string* symbol_file) {
  ASSERT_TRUE_ABORT(module);
  ASSERT_TRUE_ABORT(system_info);
  ASSERT_EQ_ABORT(system_info->cpu, kSystemInfoCPU);
  ASSERT_EQ_ABORT(system_info->cpu_info, kSystemInfoCPUInfo);
  ASSERT_EQ_ABORT(system_info->os, kSystemInfoOS);
  ASSERT_EQ_ABORT(system_info->os_short, kSystemInfoOSShort);
  ASSERT_EQ_ABORT(system_info->os_version, kSystemInfoOSVersion);

  if (interrupt_) {
    return INTERRUPT;
  }

  if (module && module->code_file() == "c:\\test_app.exe") {
      *symbol_file = GetTestDataPath() + "symbols/test_app.pdb/" +
                     module->debug_identifier() + "/test_app.sym";
    return FOUND;
  }

  return NOT_FOUND;
}

SymbolSupplier::SymbolResult TestSymbolSupplier::GetSymbolFile(
    const CodeModule* module,
    const SystemInfo* system_info,
    string* symbol_file,
    string* symbol_data) {
  SymbolSupplier::SymbolResult s = GetSymbolFile(module, system_info,
                                                 symbol_file);
  if (s == FOUND) {
    std::ifstream in(symbol_file->c_str());
    std::getline(in, *symbol_data, string::traits_type::to_char_type(
                     string::traits_type::eof()));
    in.close();
  }

  return s;
}

SymbolSupplier::SymbolResult TestSymbolSupplier::GetCStringSymbolData(
    const CodeModule* module,
    const SystemInfo* system_info,
    string* symbol_file,
    char** symbol_data,
    size_t* symbol_data_size) {
  string symbol_data_string;
  SymbolSupplier::SymbolResult s = GetSymbolFile(module,
                                                 system_info,
                                                 symbol_file,
                                                 &symbol_data_string);
  if (s == FOUND) {
    *symbol_data_size = symbol_data_string.size() + 1;
    *symbol_data = new char[*symbol_data_size];
    if (*symbol_data == nullptr) {
      BPLOG(ERROR) << "Memory allocation failed for module: "
                   << module->code_file() << " size: " << *symbol_data_size;
      return INTERRUPT;
    }
    memcpy(*symbol_data, symbol_data_string.c_str(), symbol_data_string.size());
    (*symbol_data)[symbol_data_string.size()] = '\0';
    memory_buffers_.insert(make_pair(module->code_file(), *symbol_data));
  }

  return s;
}

void TestSymbolSupplier::FreeSymbolData(const CodeModule* module) {
  map<string, char*>::iterator it = memory_buffers_.find(module->code_file());
  if (it != memory_buffers_.end()) {
    delete [] it->second;
    memory_buffers_.erase(it);
  }
}

// A test system info stream, just returns values from the
// MDRawSystemInfo fed to it.
class TestMinidumpSystemInfo : public MinidumpSystemInfo {
 public:
  explicit TestMinidumpSystemInfo(MDRawSystemInfo info) :
      MinidumpSystemInfo(nullptr) {
    valid_ = true;
    system_info_ = info;
    csd_version_ = new string("");
  }
};

// A test minidump context, just returns the MDRawContextX86
// fed to it.
class TestMinidumpContext : public MinidumpContext {
 public:
  explicit TestMinidumpContext(const MDRawContextX86& context) :
      MinidumpContext(nullptr) {
    valid_ = true;
    SetContextX86(new MDRawContextX86(context));
    SetContextFlags(MD_CONTEXT_X86);
  }
};

class MinidumpProcessorTest : public ::testing::Test {
};

TEST_F(MinidumpProcessorTest, TestUnloadedModules) {
  MockMinidump dump;

  EXPECT_CALL(dump, path()).WillRepeatedly(Return("mock minidump"));
  EXPECT_CALL(dump, Read()).WillRepeatedly(Return(true));

  MDRawHeader fake_header;
  fake_header.time_date_stamp = 0;
  EXPECT_CALL(dump, header()).WillRepeatedly(Return(&fake_header));

  MDRawSystemInfo raw_system_info;
  memset(&raw_system_info, 0, sizeof(raw_system_info));
  raw_system_info.processor_architecture = MD_CPU_ARCHITECTURE_X86;
  raw_system_info.platform_id = MD_OS_WIN32_NT;
  TestMinidumpSystemInfo dump_system_info(raw_system_info);

  EXPECT_CALL(dump, GetSystemInfo()).
      WillRepeatedly(Return(&dump_system_info));

  // No loaded modules

  MockMinidumpUnloadedModuleList unloaded_module_list;
  EXPECT_CALL(dump, GetUnloadedModuleList()).
      WillOnce(Return(&unloaded_module_list));

  MockMinidumpMemoryList memory_list;
  EXPECT_CALL(dump, GetMemoryList()).
      WillOnce(Return(&memory_list));

  MockMinidumpThreadList thread_list;
  EXPECT_CALL(dump, GetThreadList()).
      WillOnce(Return(&thread_list));

  EXPECT_CALL(thread_list, thread_count()).
    WillRepeatedly(Return(1));

  MockMinidumpThread thread;
  EXPECT_CALL(thread_list, GetThreadAtIndex(0)).
    WillOnce(Return(&thread));

  EXPECT_CALL(thread, GetThreadID(_)).
    WillRepeatedly(DoAll(SetArgumentPointee<0>(1),
                         Return(true)));

  MDRawContextX86 thread_raw_context;
  memset(&thread_raw_context, 0,
         sizeof(thread_raw_context));
  thread_raw_context.context_flags = MD_CONTEXT_X86_FULL;
  const uint32_t kExpectedEIP = 0xabcd1234;
  thread_raw_context.eip = kExpectedEIP;
  TestMinidumpContext thread_context(thread_raw_context);
  EXPECT_CALL(thread, GetContext()).
    WillRepeatedly(Return(&thread_context));

  // The memory contents don't really matter here, since it won't be used.
  MockMinidumpMemoryRegion thread_memory(0x1234, "xxx");
  EXPECT_CALL(thread, GetMemory()).
    WillRepeatedly(Return(&thread_memory));
  EXPECT_CALL(thread, GetStartOfStackMemoryRange()).
    Times(0);
  EXPECT_CALL(memory_list, GetMemoryRegionForAddress(_)).
    Times(0);

  MockMinidumpUnloadedModuleList* unloaded_module_list_copy =
      new MockMinidumpUnloadedModuleList();
  EXPECT_CALL(unloaded_module_list, Copy()).
      WillOnce(Return(unloaded_module_list_copy));

  MockMinidumpUnloadedModule unloaded_module;
  EXPECT_CALL(*unloaded_module_list_copy, GetModuleForAddress(kExpectedEIP)).
      WillOnce(Return(&unloaded_module));

  MinidumpProcessor processor(static_cast<SymbolSupplier*>(nullptr), nullptr);
  ProcessState state;
  EXPECT_EQ(processor.Process(&dump, &state),
            google_breakpad::PROCESS_OK);

  // The single frame should be populated with the unloaded module.
  ASSERT_EQ(1U, state.threads()->size());
  ASSERT_EQ(1U, state.threads()->at(0)->frames()->size());
  ASSERT_EQ(kExpectedEIP, state.threads()->at(0)->frames()->at(0)->instruction);
  ASSERT_EQ(&unloaded_module, state.threads()->at(0)->frames()->at(0)->module);
}

TEST_F(MinidumpProcessorTest, TestCorruptMinidumps) {
  MockMinidump dump;
  TestSymbolSupplier supplier;
  BasicSourceLineResolver resolver;
  MinidumpProcessor processor(&supplier, &resolver);
  ProcessState state;

  EXPECT_EQ(processor.Process("nonexistent minidump", &state),
            google_breakpad::PROCESS_ERROR_MINIDUMP_NOT_FOUND);

  EXPECT_CALL(dump, path()).WillRepeatedly(Return("mock minidump"));
  EXPECT_CALL(dump, Read()).WillRepeatedly(Return(true));

  MDRawHeader fakeHeader;
  fakeHeader.time_date_stamp = 0;
  EXPECT_CALL(dump, header()).
      WillOnce(Return(static_cast<MDRawHeader*>(nullptr))).
      WillRepeatedly(Return(&fakeHeader));

  EXPECT_EQ(processor.Process(&dump, &state),
            google_breakpad::PROCESS_ERROR_NO_MINIDUMP_HEADER);

  EXPECT_CALL(dump, GetThreadList()).
      WillOnce(Return(static_cast<MinidumpThreadList*>(nullptr)));
  EXPECT_CALL(dump, GetSystemInfo()).
      WillRepeatedly(Return(static_cast<MinidumpSystemInfo*>(nullptr)));

  EXPECT_EQ(processor.Process(&dump, &state),
            google_breakpad::PROCESS_ERROR_NO_THREAD_LIST);
}

// This test case verifies that the symbol supplier is only consulted
// once per minidump per module.
TEST_F(MinidumpProcessorTest, TestSymbolSupplierLookupCounts) {
  MockSymbolSupplier supplier;
  BasicSourceLineResolver resolver;
  MinidumpProcessor processor(&supplier, &resolver);

  string minidump_file = GetTestDataPath() + "minidump2.dmp";
  ProcessState state;
  EXPECT_CALL(supplier, GetCStringSymbolData(
      Property(&google_breakpad::CodeModule::code_file,
               "c:\\test_app.exe"),
      _, _, _, _)).WillOnce(Return(SymbolSupplier::NOT_FOUND));
  EXPECT_CALL(supplier, GetCStringSymbolData(
      Property(&google_breakpad::CodeModule::code_file,
               Ne("c:\\test_app.exe")),
      _, _, _, _)).WillRepeatedly(Return(SymbolSupplier::NOT_FOUND));
  // Avoid GMOCK WARNING "Uninteresting mock function call - returning
  // directly" for FreeSymbolData().
  EXPECT_CALL(supplier, FreeSymbolData(_)).Times(AnyNumber());
  ASSERT_EQ(processor.Process(minidump_file, &state),
            google_breakpad::PROCESS_OK);

  ASSERT_TRUE(Mock::VerifyAndClearExpectations(&supplier));

  // We need to verify that across minidumps, the processor will refetch
  // symbol files, even with the same symbol supplier.
  EXPECT_CALL(supplier, GetCStringSymbolData(
      Property(&google_breakpad::CodeModule::code_file,
               "c:\\test_app.exe"),
      _, _, _, _)).WillOnce(Return(SymbolSupplier::NOT_FOUND));
  EXPECT_CALL(supplier, GetCStringSymbolData(
      Property(&google_breakpad::CodeModule::code_file,
               Ne("c:\\test_app.exe")),
      _, _, _, _)).WillRepeatedly(Return(SymbolSupplier::NOT_FOUND));
  // Avoid GMOCK WARNING "Uninteresting mock function call - returning
  // directly" for FreeSymbolData().
  EXPECT_CALL(supplier, FreeSymbolData(_)).Times(AnyNumber());
  ASSERT_EQ(processor.Process(minidump_file, &state),
            google_breakpad::PROCESS_OK);
}

TEST_F(MinidumpProcessorTest, TestBasicProcessing) {
  TestSymbolSupplier supplier;
  BasicSourceLineResolver resolver;
  MinidumpProcessor processor(&supplier, &resolver);

  string minidump_file = GetTestDataPath() + "minidump2.dmp";

  ProcessState state;
  ASSERT_EQ(processor.Process(minidump_file, &state),
            google_breakpad::PROCESS_OK);
  ASSERT_EQ(state.system_info()->os, kSystemInfoOS);
  ASSERT_EQ(state.system_info()->os_short, kSystemInfoOSShort);
  ASSERT_EQ(state.system_info()->os_version, kSystemInfoOSVersion);
  ASSERT_EQ(state.system_info()->cpu, kSystemInfoCPU);
  ASSERT_EQ(state.system_info()->cpu_info, kSystemInfoCPUInfo);
  ASSERT_TRUE(state.crashed());
  ASSERT_EQ(state.crash_reason(), "EXCEPTION_ACCESS_VIOLATION_WRITE");
  ASSERT_EQ(state.crash_address(), 0x45U);
  ASSERT_EQ(state.threads()->size(), size_t(1));
  EXPECT_EQ((*state.threads())[0]->tid(), 3060U);
  ASSERT_EQ(state.requesting_thread(), 0);
  EXPECT_EQ(1171480435U, state.time_date_stamp());
  EXPECT_EQ(1171480435U, state.process_create_time());

  CallStack* stack = state.threads()->at(0);
  ASSERT_TRUE(stack);
  ASSERT_EQ(stack->frames()->size(), 4U);

  ASSERT_TRUE(stack->frames()->at(0)->module);
  ASSERT_EQ(stack->frames()->at(0)->module->base_address(), 0x400000U);
  ASSERT_EQ(stack->frames()->at(0)->module->code_file(), "c:\\test_app.exe");
  ASSERT_EQ(stack->frames()->at(0)->function_name,
            "`anonymous namespace'::CrashFunction");
  ASSERT_EQ(stack->frames()->at(0)->source_file_name, "c:\\test_app.cc");
  ASSERT_EQ(stack->frames()->at(0)->source_line, 58);

  ASSERT_TRUE(stack->frames()->at(1)->module);
  ASSERT_EQ(stack->frames()->at(1)->module->base_address(), 0x400000U);
  ASSERT_EQ(stack->frames()->at(1)->module->code_file(), "c:\\test_app.exe");
  ASSERT_EQ(stack->frames()->at(1)->function_name, "main");
  ASSERT_EQ(stack->frames()->at(1)->source_file_name, "c:\\test_app.cc");
  ASSERT_EQ(stack->frames()->at(1)->source_line, 65);

  // This comes from the CRT
  ASSERT_TRUE(stack->frames()->at(2)->module);
  ASSERT_EQ(stack->frames()->at(2)->module->base_address(), 0x400000U);
  ASSERT_EQ(stack->frames()->at(2)->module->code_file(), "c:\\test_app.exe");
  ASSERT_EQ(stack->frames()->at(2)->function_name, "__tmainCRTStartup");
  ASSERT_EQ(stack->frames()->at(2)->source_file_name,
            "f:\\sp\\vctools\\crt_bld\\self_x86\\crt\\src\\crt0.c");
  ASSERT_EQ(stack->frames()->at(2)->source_line, 327);

  // No debug info available for kernel32.dll
  ASSERT_TRUE(stack->frames()->at(3)->module);
  ASSERT_EQ(stack->frames()->at(3)->module->base_address(), 0x7c800000U);
  ASSERT_EQ(stack->frames()->at(3)->module->code_file(),
            "C:\\WINDOWS\\system32\\kernel32.dll");
  ASSERT_TRUE(stack->frames()->at(3)->function_name.empty());
  ASSERT_TRUE(stack->frames()->at(3)->source_file_name.empty());
  ASSERT_EQ(stack->frames()->at(3)->source_line, 0);

  ASSERT_EQ(state.modules()->module_count(), 13U);
  ASSERT_TRUE(state.modules()->GetMainModule());
  ASSERT_EQ(state.modules()->GetMainModule()->code_file(), "c:\\test_app.exe");
  ASSERT_FALSE(state.modules()->GetModuleForAddress(0));
  ASSERT_EQ(state.modules()->GetMainModule(),
            state.modules()->GetModuleForAddress(0x400000));
  ASSERT_EQ(state.modules()->GetModuleForAddress(0x7c801234)->debug_file(),
            "kernel32.pdb");
  ASSERT_EQ(state.modules()->GetModuleForAddress(0x77d43210)->version(),
            "5.1.2600.2622");

  // Test that disabled exploitability engine defaults to
  // EXPLOITABILITY_NOT_ANALYZED.
  ASSERT_EQ(google_breakpad::EXPLOITABILITY_NOT_ANALYZED,
            state.exploitability());

  // Test that the symbol supplier can interrupt processing
  state.Clear();
  supplier.set_interrupt(true);
  ASSERT_EQ(processor.Process(minidump_file, &state),
            google_breakpad::PROCESS_SYMBOL_SUPPLIER_INTERRUPTED);
}

TEST_F(MinidumpProcessorTest, TestThreadMissingMemory) {
  MockMinidump dump;
  EXPECT_CALL(dump, path()).WillRepeatedly(Return("mock minidump"));
  EXPECT_CALL(dump, Read()).WillRepeatedly(Return(true));

  MDRawHeader fake_header;
  fake_header.time_date_stamp = 0;
  EXPECT_CALL(dump, header()).WillRepeatedly(Return(&fake_header));

  MDRawSystemInfo raw_system_info;
  memset(&raw_system_info, 0, sizeof(raw_system_info));
  raw_system_info.processor_architecture = MD_CPU_ARCHITECTURE_X86;
  raw_system_info.platform_id = MD_OS_WIN32_NT;
  TestMinidumpSystemInfo dump_system_info(raw_system_info);

  EXPECT_CALL(dump, GetSystemInfo()).
      WillRepeatedly(Return(&dump_system_info));

  MockMinidumpThreadList thread_list;
  EXPECT_CALL(dump, GetThreadList()).
      WillOnce(Return(&thread_list));

  MockMinidumpMemoryList memory_list;
  EXPECT_CALL(dump, GetMemoryList()).
      WillOnce(Return(&memory_list));

  // Return a thread missing stack memory.
  MockMinidumpThread no_memory_thread;
  EXPECT_CALL(no_memory_thread, GetThreadID(_)).
    WillRepeatedly(DoAll(SetArgumentPointee<0>(1),
                         Return(true)));
  EXPECT_CALL(no_memory_thread, GetMemory()).
    WillRepeatedly(Return(static_cast<MinidumpMemoryRegion*>(nullptr)));

  const uint64_t kTestStartOfMemoryRange = 0x1234;
  EXPECT_CALL(no_memory_thread, GetStartOfStackMemoryRange()).
    WillRepeatedly(Return(kTestStartOfMemoryRange));
  EXPECT_CALL(memory_list, GetMemoryRegionForAddress(kTestStartOfMemoryRange)).
    WillRepeatedly(Return(static_cast<MinidumpMemoryRegion*>(nullptr)));

  MDRawContextX86 no_memory_thread_raw_context;
  memset(&no_memory_thread_raw_context, 0,
         sizeof(no_memory_thread_raw_context));
  no_memory_thread_raw_context.context_flags = MD_CONTEXT_X86_FULL;
  const uint32_t kExpectedEIP = 0xabcd1234;
  no_memory_thread_raw_context.eip = kExpectedEIP;
  TestMinidumpContext no_memory_thread_context(no_memory_thread_raw_context);
  EXPECT_CALL(no_memory_thread, GetContext()).
    WillRepeatedly(Return(&no_memory_thread_context));

  EXPECT_CALL(thread_list, thread_count()).
    WillRepeatedly(Return(1));
  EXPECT_CALL(thread_list, GetThreadAtIndex(0)).
    WillOnce(Return(&no_memory_thread));

  MinidumpProcessor processor(static_cast<SymbolSupplier*>(nullptr), nullptr);
  ProcessState state;
  EXPECT_EQ(processor.Process(&dump, &state),
            google_breakpad::PROCESS_OK);

  // Should have a single thread with a single frame in it.
  ASSERT_EQ(1U, state.threads()->size());
  ASSERT_EQ(1U, state.threads()->at(0)->frames()->size());
  ASSERT_EQ(kExpectedEIP, state.threads()->at(0)->frames()->at(0)->instruction);
}

TEST_F(MinidumpProcessorTest, GetProcessCreateTime) {
  const uint32_t kProcessCreateTime = 2000;
  const uint32_t kTimeDateStamp = 5000;
  MockMinidump dump;
  EXPECT_CALL(dump, path()).WillRepeatedly(Return("mock minidump"));
  EXPECT_CALL(dump, Read()).WillRepeatedly(Return(true));

  // Set time of crash.
  MDRawHeader fake_header;
  fake_header.time_date_stamp = kTimeDateStamp;
  EXPECT_CALL(dump, header()).WillRepeatedly(Return(&fake_header));

  // Set process create time.
  MDRawMiscInfo raw_misc_info;
  memset(&raw_misc_info, 0, sizeof(raw_misc_info));
  raw_misc_info.process_create_time = kProcessCreateTime;
  raw_misc_info.flags1 |= MD_MISCINFO_FLAGS1_PROCESS_TIMES;
  google_breakpad::TestMinidumpMiscInfo dump_misc_info(raw_misc_info);
  EXPECT_CALL(dump, GetMiscInfo()).WillRepeatedly(Return(&dump_misc_info));

  // No threads
  MockMinidumpThreadList thread_list;
  EXPECT_CALL(dump, GetThreadList()).WillOnce(Return(&thread_list));
  EXPECT_CALL(thread_list, thread_count()).WillRepeatedly(Return(0));

  MinidumpProcessor processor(static_cast<SymbolSupplier*>(nullptr), nullptr);
  ProcessState state;
  EXPECT_EQ(google_breakpad::PROCESS_OK, processor.Process(&dump, &state));

  // Verify the time stamps.
  ASSERT_EQ(kTimeDateStamp, state.time_date_stamp());
  ASSERT_EQ(kProcessCreateTime, state.process_create_time());
}

TEST_F(MinidumpProcessorTest, TestThreadMissingContext) {
  MockMinidump dump;
  EXPECT_CALL(dump, path()).WillRepeatedly(Return("mock minidump"));
  EXPECT_CALL(dump, Read()).WillRepeatedly(Return(true));

  MDRawHeader fake_header;
  fake_header.time_date_stamp = 0;
  EXPECT_CALL(dump, header()).WillRepeatedly(Return(&fake_header));

  MDRawSystemInfo raw_system_info;
  memset(&raw_system_info, 0, sizeof(raw_system_info));
  raw_system_info.processor_architecture = MD_CPU_ARCHITECTURE_X86;
  raw_system_info.platform_id = MD_OS_WIN32_NT;
  TestMinidumpSystemInfo dump_system_info(raw_system_info);

  EXPECT_CALL(dump, GetSystemInfo()).
      WillRepeatedly(Return(&dump_system_info));

  MockMinidumpThreadList thread_list;
  EXPECT_CALL(dump, GetThreadList()).
      WillOnce(Return(&thread_list));

  MockMinidumpMemoryList memory_list;
  EXPECT_CALL(dump, GetMemoryList()).
      WillOnce(Return(&memory_list));

  // Return a thread missing a thread context.
  MockMinidumpThread no_context_thread;
  EXPECT_CALL(no_context_thread, GetThreadID(_)).
    WillRepeatedly(DoAll(SetArgumentPointee<0>(1),
                         Return(true)));
  EXPECT_CALL(no_context_thread, GetContext()).
    WillRepeatedly(Return(static_cast<MinidumpContext*>(nullptr)));

  // The memory contents don't really matter here, since it won't be used.
  MockMinidumpMemoryRegion no_context_thread_memory(0x1234, "xxx");
  EXPECT_CALL(no_context_thread, GetMemory()).
    WillRepeatedly(Return(&no_context_thread_memory));
  EXPECT_CALL(no_context_thread, GetStartOfStackMemoryRange()).
    Times(0);
  EXPECT_CALL(memory_list, GetMemoryRegionForAddress(_)).
    Times(0);

  EXPECT_CALL(thread_list, thread_count()).
    WillRepeatedly(Return(1));
  EXPECT_CALL(thread_list, GetThreadAtIndex(0)).
    WillOnce(Return(&no_context_thread));

  MinidumpProcessor processor(static_cast<SymbolSupplier*>(nullptr), nullptr);
  ProcessState state;
  EXPECT_EQ(processor.Process(&dump, &state),
            google_breakpad::PROCESS_OK);

  // Should have a single thread with zero frames.
  ASSERT_EQ(1U, state.threads()->size());
  ASSERT_EQ(0U, state.threads()->at(0)->frames()->size());
}

TEST_F(MinidumpProcessorTest, Test32BitCrashingAddress) {
  TestSymbolSupplier supplier;
  BasicSourceLineResolver resolver;
  MinidumpProcessor processor(&supplier, &resolver);

  string minidump_file = GetTestDataPath() + "minidump_32bit_crash_addr.dmp";

  ProcessState state;
  ASSERT_EQ(processor.Process(minidump_file, &state),
            google_breakpad::PROCESS_OK);
  ASSERT_EQ(state.system_info()->os, kSystemInfoOS);
  ASSERT_EQ(state.system_info()->os_short, kSystemInfoOSShort);
  ASSERT_EQ(state.system_info()->os_version, kSystemInfoOSVersion);
  ASSERT_EQ(state.system_info()->cpu, kSystemInfoCPU);
  ASSERT_EQ(state.system_info()->cpu_info, kSystemInfoCPUInfo);
  ASSERT_TRUE(state.crashed());
  ASSERT_EQ(state.crash_reason(), "EXCEPTION_ACCESS_VIOLATION_WRITE");
  ASSERT_EQ(state.crash_address(), 0x45U);
}

TEST_F(MinidumpProcessorTest, TestXStateX86ContextMinidump) {
  // This tests if we can passively process a minidump with cet registers in its
  // context. Dump is captured from a toy executable and is readable by windbg.
  MinidumpProcessor processor(nullptr, nullptr /*&supplier, &resolver*/);

  string minidump_file = GetTestDataPath()
                         + "tiny-exe-with-cet-xsave-x86.dmp";

  ProcessState state;
  ASSERT_EQ(processor.Process(minidump_file, &state),
            google_breakpad::PROCESS_OK);
  ASSERT_EQ(state.system_info()->os, "Windows NT");
  ASSERT_EQ(state.system_info()->os_version, "10.0.22631 ");
  ASSERT_EQ(state.system_info()->cpu, "x86");
  ASSERT_EQ(state.system_info()->cpu_info,
            "GenuineIntel family 6 model 151 stepping 2");
  ASSERT_FALSE(state.crashed());
  ASSERT_EQ(state.threads()->size(), size_t(3));

  // TODO: verify cetumsr and cetussp once these are supported by
  // breakpad.
}

TEST_F(MinidumpProcessorTest, TestXStateAmd64ContextMinidump) {
  // This tests if we can passively process a minidump with cet registers in its
  // context. Dump is captured from a toy executable and is readable by windbg.
  MinidumpProcessor processor(nullptr, nullptr /*&supplier, &resolver*/);

  string minidump_file = GetTestDataPath()
                         + "tiny-exe-with-cet-xsave.dmp";

  ProcessState state;
  ASSERT_EQ(processor.Process(minidump_file, &state),
            google_breakpad::PROCESS_OK);
  ASSERT_EQ(state.system_info()->os, "Windows NT");
  ASSERT_EQ(state.system_info()->os_version, "10.0.22000 282");
  ASSERT_EQ(state.system_info()->cpu, "amd64");
  ASSERT_EQ(state.system_info()->cpu_info,
            "family 6 model 140 stepping 1");
  ASSERT_FALSE(state.crashed());
  ASSERT_EQ(state.threads()->size(), size_t(1));

  // TODO: verify cetumsr and cetussp once these are supported by
  // breakpad.
}

TEST_F(MinidumpProcessorTest, TestFastFailException) {
  // This tests if we can understand fastfail exception subcodes.
  // Dump is captured from a toy executable and is readable by windbg.
  MinidumpProcessor processor(nullptr, nullptr /*&supplier, &resolver*/);

  string minidump_file = GetTestDataPath()
                         + "tiny-exe-fastfail.dmp";

  ProcessState state;
  ASSERT_EQ(processor.Process(minidump_file, &state),
            google_breakpad::PROCESS_OK);
  ASSERT_TRUE(state.crashed());
  ASSERT_EQ(state.threads()->size(), size_t(4));
  ASSERT_EQ(state.crash_reason(), "FAST_FAIL_FATAL_APP_EXIT");
}

#ifdef __linux__
TEST_F(MinidumpProcessorTest, TestNonCanonicalAddress) {
  // This tests if we can correctly fixup non-canonical address GPF fault
  // addresses.
  // Dump is captured from a toy executable and is readable by windbg.
  MinidumpProcessor processor(nullptr, nullptr /*&supplier, &resolver*/);
  processor.set_enable_objdump(true);

  string minidump_file = GetTestDataPath()
                         + "write_av_non_canonical.dmp";

  ProcessState state;
  ASSERT_EQ(processor.Process(minidump_file, &state),
            google_breakpad::PROCESS_OK);
  ASSERT_TRUE(state.crashed());
  ASSERT_EQ(state.crash_address(), 0xfefefefefefefefeU);
}
#endif // __linux__

}  // namespace

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
