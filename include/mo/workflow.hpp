#pragma once
#include "mo/protocol.hpp"
#include "mo/resource_manager.hpp"
#include <chrono>
#include <future>
#include <map>
#include <string>
#include <variant>
namespace mo {
using Variables = std::map<std::string, std::vector<uint16_t>>;
enum class WorkflowState {
  Pending,
  Running,
  Blocked,
  Completed,
  Failed,
  Aborted
};
enum class WorkflowReason {
  None,
  InvalidWorkflow,
  RequestFailed,
  ConditionFailed,
  RankViolation,
  Deadlock,
  DeviceFaulted,
  Stopped
};
struct WriteRegister {
  int device;
  uint16_t address, value;
};
struct WriteRegisters {
  int device;
  uint16_t address;
  std::vector<uint16_t> values;
};
struct ReadRegisters {
  int device;
  uint16_t address, count;
  std::string variable;
};
struct Delay {
  std::chrono::milliseconds duration;
};
enum class Comparison {
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual
};
enum class Branch { Continue, Skip, Fail };
struct Conditional {
  std::string variable;
  size_t index;
  Comparison comparison;
  uint16_t constant;
  Branch onTrue = Branch::Continue, onFalse = Branch::Fail;
  size_t skip = 0;
};
// Test-only manual acquisition, accepted exclusively in UNCHECKED mode.
struct AcquireDevices {
  std::vector<int> devices;
};
using Step = std::variant<WriteRegister, WriteRegisters, ReadRegisters, Delay,
                          Conditional, AcquireDevices>;
struct Workflow {
  WorkflowId id = 0;
  std::string name;
  int priority = 0;
  std::vector<Step> steps;
  std::vector<int> devices;
  bool manual = false;
  Workflow &named(std::string n) {
    name = std::move(n);
    return *this;
  }
  Workflow &withId(WorkflowId i) {
    id = i;
    return *this;
  }
  Workflow &withPriority(int p) {
    priority = p;
    return *this;
  }
  Workflow &needs(std::vector<int> d) {
    devices = std::move(d);
    return *this;
  }
  Workflow &then(Step s) {
    steps.push_back(std::move(s));
    return *this;
  }
  Workflow &manualLocks() {
    manual = true;
    return *this;
  }
};
struct WorkflowResult {
  WorkflowId id;
  WorkflowState state;
  size_t failingStep;
  WorkflowReason reason;
  std::string detail;
  Variables variables;
};
struct WorkflowHandle {
  WorkflowId id;
  std::shared_future<WorkflowResult> completion;
};
} // namespace mo
