/*
Copyright 2020 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#define _CRT_SECURE_NO_WARNINGS

#include "common.h"
#include "tinyinstinstrumentation.h"
#include "litecov.h"

#include <sstream>
#include <vector>

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
#include <windows.h>
#include <process.h>
#endif


void TinyInstInstrumentation::Init(int argc, char **argv) {
  instrumentation = new LiteCov();
  instrumentation->Init(argc, argv);

  persist = GetBinaryOption("-persist", argc, argv, false);
  stack_crash_signature =
    GetBinaryOption("-stack_crash_signature", argc, argv, false);
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
  instrumentation->SetCrashStackCollection(stack_crash_signature);
#endif
  num_iterations = GetIntOption("-iterations", argc, argv, 1);
}

RunResult TinyInstInstrumentation::Run(int argc, char **argv, uint32_t init_timeout, uint32_t timeout) {
  DebuggerStatus status;
  RunResult ret = OTHER_ERROR;

  if (instrumentation->IsTargetFunctionDefined()) {
    if (cur_iteration == num_iterations) {
      instrumentation->Kill();
      cur_iteration = 0;
    }
  }
  
  // else clear only when the target function is reached
  if (!instrumentation->IsTargetFunctionDefined()) {
    instrumentation->ClearCoverage();
  }

  uint32_t timeout1 = timeout;
  if (instrumentation->IsTargetFunctionDefined()) {
    timeout1 = init_timeout;
  }

  if (instrumentation->IsTargetAlive() && persist) {
    status = instrumentation->Continue(timeout1);
  } else {
    instrumentation->Kill();
    cur_iteration = 0;
    status = instrumentation->Run(argc, argv, timeout1);
  }

  // if target function is defined,
  // we should wait until it is hit
  if (instrumentation->IsTargetFunctionDefined()) {
    if (status != DEBUGGER_TARGET_START) {
      // try again with a clean process
      WARN("Target function not reached, retrying with a clean process\n");
      instrumentation->Kill();
      cur_iteration = 0;
      status = instrumentation->Run(argc, argv, init_timeout);
    }

    if (status != DEBUGGER_TARGET_START) {
      switch (status) {
      case DEBUGGER_CRASHED:
        FATAL("Process crashed before reaching the target method\n");
        break;
      case DEBUGGER_HANGED:
        FATAL("Process hanged before reaching the target method\n");
        break;
      case DEBUGGER_PROCESS_EXIT:
        FATAL("Process exited before reaching the target method\n");
        break;
      default:
        FATAL("An unknown problem occured before reaching the target method\n");
        break;
      }
    }

    instrumentation->ClearCoverage();

    status = instrumentation->Continue(timeout);
  }

  switch (status) {
  case DEBUGGER_CRASHED:
    ret = CRASH;
    instrumentation->Kill();
    break;
  case DEBUGGER_HANGED:
    ret = HANG;
    instrumentation->Kill();
    break;
  case DEBUGGER_PROCESS_EXIT:
    ret = OK;
    if (instrumentation->IsTargetFunctionDefined()) {
      WARN("Process exit during target function\n");
      ret = HANG;
    }
    break;
  case DEBUGGER_TARGET_END:
    if (instrumentation->IsTargetFunctionDefined()) {
      ret = OK;
      cur_iteration++;
    } else {
      FATAL("Unexpected status received from the debugger\n");
    }
    break;
  default:
    FATAL("Unexpected status received from the debugger\n");
    break;
  }

  return ret;
}

RunResult TinyInstInstrumentation::RunWithCrashAnalysis(int argc, char** argv, uint32_t init_timeout, uint32_t timeout) {
  // clean process when reproducing crashes
  instrumentation->Kill();
  // disable instrumentation when reproducing crashes
  instrumentation->DisableInstrumentation();
  RunResult ret = Run(argc, argv, init_timeout, timeout);
  instrumentation->Kill();
  instrumentation->EnableInstrumentation();
  return ret;
}

RunResult TinyInstInstrumentation::RunNative(
  int argc,
  char** argv,
  uint32_t timeout)
{
  instrumentation->Kill();

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
  if ((argc <= 0) || !argv || !argv[0]) {
    return OTHER_ERROR;
  }

  std::vector<const char *> native_argv;
  for (int i = 0; i < argc; i++) {
    native_argv.push_back(argv[i]);
  }
  native_argv.push_back(NULL);

  intptr_t process_value = _spawnvp(
    _P_NOWAIT,
    argv[0],
    native_argv.data());

  if (process_value == -1) {
    return OTHER_ERROR;
  }

  HANDLE process_handle =
    reinterpret_cast<HANDLE>(process_value);

  DWORD wait_result =
    WaitForSingleObject(process_handle, timeout);

  if (wait_result == WAIT_TIMEOUT) {
    TerminateProcess(process_handle, 1);
    WaitForSingleObject(process_handle, INFINITE);
    CloseHandle(process_handle);
    return HANG;
  }

  if (wait_result != WAIT_OBJECT_0) {
    TerminateProcess(process_handle, 1);
    CloseHandle(process_handle);
    return OTHER_ERROR;
  }

  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process_handle, &exit_code)) {
    CloseHandle(process_handle);
    return OTHER_ERROR;
  }

  CloseHandle(process_handle);

  if ((exit_code & 0xF0000000UL) == 0xC0000000UL) {
    return CRASH;
  }

  return OK;
#else
  return OTHER_ERROR;
#endif
}

void TinyInstInstrumentation::CleanTarget() {
  instrumentation->Kill();
}

bool TinyInstInstrumentation::HasNewCoverage() {
  return instrumentation->HasNewCoverage();
}

void TinyInstInstrumentation::GetCoverage(Coverage &coverage, bool clear_coverage) {
  instrumentation->GetCoverage(coverage, clear_coverage);
}

void TinyInstInstrumentation::ClearCoverage() {
  instrumentation->ClearCoverage();
}

void TinyInstInstrumentation::IgnoreCoverage(Coverage &coverage) {
  instrumentation->IgnoreCoverage(coverage);
}

TinyInstInstrumentation::~TinyInstInstrumentation() {
  instrumentation->Kill();
  delete instrumentation;
}

std::string TinyInstInstrumentation::GetCrashName() {
  LiteCov::Exception exception = instrumentation->GetLastException();
  std::stringstream stream;
  switch (exception.type) {
  case LiteCov::ExceptionType::ACCESS_VIOLATION:
    stream << "access_violation";
    break;
  case LiteCov::ExceptionType::ILLEGAL_INSTRUCTION:
    stream << "illegal_instruction";
    break;
  case LiteCov::ExceptionType::STACK_OVERFLOW:
    stream << "stack_overflow";
    break;
  default:
    stream << "other";
    break;
  }
  stream << "_";
  stream << AnonymizeAddress(exception.ip);
  stream << "_";
  stream << AnonymizeAddress(exception.access_address);
  return stream.str();
}

std::string TinyInstInstrumentation::GetCrashSignature() {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
  if (stack_crash_signature) {
    auto exception = instrumentation->GetLastException();

    if (!exception.stack_signature_frames.empty()) {
      std::string crash_name = GetCrashName();
      size_t access_separator = crash_name.rfind("_");
      size_t ip_separator = std::string::npos;

      if (access_separator != std::string::npos &&
          access_separator > 0) {
        ip_separator = crash_name.rfind(
          "_", access_separator - 1);
      }

      if (ip_separator != std::string::npos) {
        std::stringstream stream;
        stream << crash_name.substr(0, ip_separator);
        stream << "_stack";

        for (size_t i = 0;
             i < exception.stack_signature_frames.size() && i < 2;
             i++) {
          stream << "_";
          stream << exception.stack_signature_frames[i];
        }

        return stream.str();
      }
    }
  }
#endif
  std::string crash_name = GetCrashName();
  size_t separator = crash_name.rfind("_");
  if (separator == std::string::npos) return crash_name;
  return crash_name.substr(0, separator);
}

uint64_t TinyInstInstrumentation::GetReturnValue() {
  return instrumentation->GetTargetReturnValue();
}
