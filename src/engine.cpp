#include "mo/engine.hpp"
#include "mo/bug_injection.hpp"
#include <algorithm>
#include <set>
#include <sstream>
#include <type_traits>
namespace mo {
namespace {
const char *stateName(WorkflowState s) {
  switch (s) {
  case WorkflowState::Pending:
    return "Pending";
  case WorkflowState::Running:
    return "Running";
  case WorkflowState::Blocked:
    return "Blocked";
  case WorkflowState::Completed:
    return "Completed";
  case WorkflowState::Failed:
    return "Failed";
  case WorkflowState::Aborted:
    return "Aborted";
  }
  return "Unknown";
}
const char *healthName(Health h) {
  switch (h) {
  case Health::Healthy:
    return "Healthy";
  case Health::Suspect:
    return "Suspect";
  case Health::Faulted:
    return "Faulted";
  case Health::Recovering:
    return "Recovering";
  }
  return "Unknown";
}
bool compare(uint16_t a, Comparison op, uint16_t b) {
  switch (op) {
  case Comparison::Equal:
    return a == b;
  case Comparison::NotEqual:
    return a != b;
  case Comparison::Less:
    return a < b;
  case Comparison::LessEqual:
    return a <= b;
  case Comparison::Greater:
    return a > b;
  case Comparison::GreaterEqual:
    return a >= b;
  }
  return false;
}
} // namespace
Engine::Engine() : Engine(EngineConfig{}) {}
Engine::Engine(EngineConfig c, std::unique_ptr<VictimPolicy> policy)
    : config_(c), log_(c.eventCapacity), scheduler_([this, c] {
        auto config = c.scheduler;
        config.realtimeObserver = [this](bool ok) {
          log_.record("realtime_worker", 0, -1, 0, ok ? "granted" : "fallback");
        };
        return config;
      }()),
      detector_(&log_, std::move(policy)), resources_(c.locking, &log_) {
  resources_.attachDetector(&detector_);
}
Engine::~Engine() { stop(); }
bool Engine::addDevice(int id, Endpoint endpoint) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_ || finished_ || id < 0 || devices_.count(id))
    return false;
  std::unique_ptr<Transport> t;
  if (config_.transportFactory)
    t = config_.transportFactory(id, endpoint);
  else if (endpoint.serialPath.empty())
    t = std::make_unique<TcpTransport>(endpoint.host, endpoint.port);
  else
    t = std::make_unique<SerialTransport>(endpoint.serialPath, endpoint.baud);
  if (!t)
    return false;
  devices_[id] = std::make_shared<Device>(reactor_, std::move(t));
  health_[id] = Health::Healthy;
  return true;
}
bool Engine::start() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_)
      return !stopping_;
    if (finished_ || !reactor_.valid())
      return false;
    started_ = true;
  }
  scheduler_.start();
  reactor_.start();
  reactor_.post([this] {
    auto realtime = applyRealtime(config_.scheduler.realtime);
    log_.record("realtime_reactor", 0, -1, 0,
                !config_.scheduler.realtime.enabled ? "off"
                : realtime                          ? "granted"
                                                    : "fallback");
    for (auto &entry : devices_) {
      int id = entry.first;
      auto d = entry.second;
      d->setObserver(
          [this, id](const char *type, uint16_t tx, CommandContext context) {
            log_.record(type, context.workflow, id, context.command,
                        "transaction=" + std::to_string(tx));
          });
      d->start();
      watchdogs_[id] = std::make_shared<Watchdog>(
          reactor_, config_.watchdog,
          [this, id](Health h) { healthChanged(id, h); },
          [this, d](std::function<void(bool)> done) {
            d->reconnect();
            d->request(
                {1, 3, config_.watchdog.probeRegister, 1, {}},
                [done = std::move(done)](Result<Response> r) {
                  done(std::holds_alternative<Response>(r) &&
                       !std::get<Response>(r).exception);
                },
                config_.watchdog.probeTimeout);
          });
      log_.record("device_health", 0, id, 0, "Healthy");
    }
  });
  barrier();
  return true;
}
void Engine::barrier() {
  std::promise<void> p;
  auto f = p.get_future();
  reactor_.post([&p] { p.set_value(); });
  f.wait();
}
bool Engine::valid(const Workflow &w) const {
  if (w.manual && config_.locking != LockMode::Unchecked)
    return false;
  std::set<int> declared(w.devices.begin(), w.devices.end());
  for (int id : w.devices)
    if (!devices_.count(id))
      return false;
  for (const auto &step : w.steps) {
    bool ok = std::visit(
        [&](const auto &s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, Delay>)
            return s.duration.count() >= 0;
          else if constexpr (std::is_same_v<T, Conditional>)
            return true;
          else if constexpr (std::is_same_v<T, AcquireDevices>) {
            return w.manual && config_.locking == LockMode::Unchecked &&
                   std::all_of(s.devices.begin(), s.devices.end(),
                               [&](int id) { return declared.count(id) != 0; });
          } else
            return declared.count(s.device) != 0;
        },
        step);
    if (!ok)
      return false;
  }
  return true;
}
WorkflowHandle Engine::submit(Workflow w) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!w.id) {
    while (runs_.count(nextId_))
      ++nextId_;
    w.id = nextId_++;
  }
  auto r = std::make_shared<Run>();
  r->workflow = std::move(w);
  r->birth = ++birth_;
  auto f = r->promise.get_future().share();
  WorkflowHandle h{r->workflow.id, f};
  if (runs_.count(h.id)) {
    r->promise.set_value({h.id,
                          WorkflowState::Failed,
                          0,
                          WorkflowReason::InvalidWorkflow,
                          "duplicate workflow id",
                          {}});
    return h;
  }
  runs_[h.id] = r;
  log_.record("workflow_state", h.id, -1, 0, "Pending");
  if (!started_ || stopping_) {
    finish(r, WorkflowState::Aborted, WorkflowReason::Stopped,
           "engine is not running");
    return h;
  }
  if (!valid(r->workflow)) {
    finish(r, WorkflowState::Failed, WorkflowReason::InvalidWorkflow,
           "invalid device declaration or step");
    return h;
  }
  for (int id : r->workflow.devices)
    if (health_.at(id) == Health::Faulted ||
        health_.at(id) == Health::Recovering) {
      finish(r, WorkflowState::Failed, WorkflowReason::DeviceFaulted,
             "device isolated");
      return h;
    }
  schedule(r);
  return h;
}
void Engine::schedule(const std::shared_ptr<Run> &r) {
  scheduler_.enqueue(r->workflow.priority, [this, r] { advance(r); });
}
void Engine::state(const std::shared_ptr<Run> &r, WorkflowState s) {
  if (r->state == s)
    return;
  r->state = s;
  log_.record("workflow_state", r->workflow.id, -1, 0, stateName(s));
}
void Engine::finish(const std::shared_ptr<Run> &r, WorkflowState s,
                    WorkflowReason reason, std::string detail) {
  if (r->terminal)
    return;
  r->terminal = true;
  state(r, s);
  if (r->delayTimer) {
    auto timer = r->delayTimer;
    reactor_.post([this, timer] { reactor_.cancel(timer); });
    r->delayTimer = 0;
  }
  if (!(MO_BUG("missing_abort_release") && s == WorkflowState::Aborted))
    resources_.release(r->workflow.id);
  if (s == WorkflowState::Completed)
    ++stats_.completed;
  else if (s == WorkflowState::Aborted)
    ++stats_.aborted;
  else
    ++stats_.failed;
  r->promise.set_value(
      {r->workflow.id, s, r->step, reason, std::move(detail), r->variables});
}
bool Engine::acquire(const std::shared_ptr<Run> &r,
                     const std::vector<int> &devices) {
  auto result =
      resources_.acquire(r->workflow.id, devices, r->workflow.priority,
                         r->birth, [this, r] { schedule(r); });
  if (result.status == AcquireStatus::RankViolation) {
    finish(r, WorkflowState::Failed, WorkflowReason::RankViolation,
           "resource rank order violation");
    return false;
  }
  if (result.status == AcquireStatus::Granted)
    return true;
  state(r, WorkflowState::Blocked);
  if (result.victim) {
    auto victim = runs_.find(result.victim);
    if (victim != runs_.end() && !victim->second->terminal) {
      ++stats_.deadlocksResolved;
      finish(victim->second, WorkflowState::Aborted, WorkflowReason::Deadlock,
             "wait-for cycle; see deadlock_detected event");
    }
  }
  return false;
}
void Engine::advance(const std::shared_ptr<Run> &r) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (r->terminal || r->waiting)
    return;
  if (stopping_) {
    finish(r, WorkflowState::Aborted, WorkflowReason::Stopped,
           "engine stopping");
    return;
  }
  for (int id : r->workflow.devices)
    if (health_.at(id) == Health::Faulted ||
        health_.at(id) == Health::Recovering) {
      finish(r, WorkflowState::Failed, WorkflowReason::DeviceFaulted,
             "device isolated");
      return;
    }
  if (!r->acquired) {
    if (!r->workflow.manual && !acquire(r, r->workflow.devices))
      return;
    r->acquired = true;
  }
  state(r, WorkflowState::Running);
  if (r->step == r->workflow.steps.size()) {
    finish(r, WorkflowState::Completed, WorkflowReason::None, {});
    return;
  }
  const auto &step = r->workflow.steps[r->step];
  if (auto s = std::get_if<Conditional>(&step)) {
    auto variable = r->variables.find(s->variable);
    if (variable == r->variables.end() || s->index >= variable->second.size()) {
      finish(r, WorkflowState::Failed, WorkflowReason::ConditionFailed,
             "missing conditional read value");
      return;
    }
    auto branch =
        compare(variable->second[s->index], s->comparison, s->constant)
            ? s->onTrue
            : s->onFalse;
    if (branch == Branch::Fail) {
      finish(r, WorkflowState::Failed, WorkflowReason::ConditionFailed,
             "conditional failed");
      return;
    }
    size_t remaining = r->workflow.steps.size() - r->step - 1;
    r->step += 1 + (branch == Branch::Skip ? std::min(s->skip, remaining) : 0);
    schedule(r);
    return;
  }
  if (auto s = std::get_if<AcquireDevices>(&step)) {
    if (acquire(r, s->devices)) {
      ++r->step;
      schedule(r);
    }
    return;
  }
  if (auto s = std::get_if<Delay>(&step)) {
    r->waiting = true;
    auto duration = s->duration;
    reactor_.post([this, r, duration] {
      std::lock_guard<std::mutex> lock(mutex_);
      if (r->terminal)
        return;
      r->delayTimer = reactor_.after(duration, [this, r] {
        std::lock_guard<std::mutex> lock(mutex_);
        r->delayTimer = 0;
        if (r->terminal)
          return;
        r->waiting = false;
        ++r->step;
        schedule(r);
      });
    });
    return;
  }
  Request req;
  int id = -1;
  std::string variable;
  std::visit(
      [&](const auto &s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, WriteRegister>) {
          id = s.device;
          req = {1, 6, s.address, 1, {s.value}};
        } else if constexpr (std::is_same_v<T, WriteRegisters>) {
          id = s.device;
          req = {1, 16, s.address, static_cast<uint16_t>(s.values.size()),
                 s.values};
        } else if constexpr (std::is_same_v<T, ReadRegisters>) {
          id = s.device;
          req = {1, 3, s.address, s.count, {}};
          variable = s.variable;
        }
      },
      step);
  r->waiting = true;
  auto command = nextCommand_++;
  log_.record("workflow_dispatch", r->workflow.id, id, command,
              "step=" + std::to_string(r->step));
  reactor_.post([this, r, id, req = std::move(req),
                 variable = std::move(variable), command]() mutable {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (r->terminal)
        return;
      if (health_.at(id) == Health::Faulted ||
          health_.at(id) == Health::Recovering) {
        finish(r, WorkflowState::Failed, WorkflowReason::DeviceFaulted,
               "device isolated before dispatch");
        return;
      }
    }
    devices_.at(id)->request(req,
                             [this, r, id, variable = std::move(variable)](
                                 Result<Response> result) mutable {
                               response(r, id, std::move(result),
                                        std::move(variable));
                             },
                             config_.requestTimeout, {r->workflow.id, command});
  });
}
void Engine::response(const std::shared_ptr<Run> &r, int id,
                      Result<Response> result, std::string variable) {
  // Protocol exceptions mean a live device rejecting a command, not bad health.
  bool transportOk = std::holds_alternative<Response>(result);
  // Locally rejected requests say nothing about device health.
  auto error = std::get_if<Error>(&result);
  if (!error ||
      (*error != Error::InvalidArgument && *error != Error::UnknownFunction &&
       *error != Error::Exhausted))
    watchdogs_.at(id)->observe(transportOk);
  std::lock_guard<std::mutex> lock(mutex_);
  if (r->terminal)
    return;
  r->waiting = false;
  if (auto error = std::get_if<Error>(&result)) {
    auto h = health_.at(id);
    bool isolated = *error == Error::DeviceFaulted || h == Health::Faulted ||
                    h == Health::Recovering;
    finish(r, WorkflowState::Failed,
           isolated ? WorkflowReason::DeviceFaulted
                    : WorkflowReason::RequestFailed,
           "transport error " + std::to_string(static_cast<int>(*error)));
    return;
  }
  auto response = std::get<Response>(std::move(result));
  if (response.exception) {
    finish(r, WorkflowState::Failed, WorkflowReason::RequestFailed,
           "Modbus exception " + std::to_string(response.exception));
    return;
  }
  if (!variable.empty())
    r->variables[variable] = std::move(response.values);
  ++r->step;
  schedule(r);
}
void Engine::healthChanged(int id, Health h) {
  std::lock_guard<std::mutex> lock(mutex_);
  health_[id] = h;
  healthCv_.notify_all();
  log_.record("device_health", 0, id, 0, healthName(h));
  if (h != Health::Faulted || stopping_)
    return;
  if (everFaulted_.insert(id).second)
    ++stats_.devicesFaulted;
  devices_.at(id)->isolate();
  if (config_.watchdog.failFast)
    for (auto &entry : runs_) {
      auto &r = entry.second;
      if (!r->terminal &&
          std::find(r->workflow.devices.begin(), r->workflow.devices.end(),
                    id) != r->workflow.devices.end())
        finish(r, WorkflowState::Failed, WorkflowReason::DeviceFaulted,
               "device " + std::to_string(id) + " faulted");
    }
}
void Engine::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_)
      return;
    if (!started_) {
      finished_ = true;
      healthCv_.notify_all();
      devices_.clear();
      return;
    }
    stopping_ = true;
    for (auto &entry : runs_)
      if (!entry.second->terminal)
        finish(entry.second, WorkflowState::Aborted, WorkflowReason::Stopped,
               "engine stopping");
  }
  reactor_.post([this] {
    for (auto &entry : watchdogs_)
      entry.second->stop();
    for (auto &entry : devices_)
      entry.second->stop();
  });
  barrier();
  scheduler_.stop();
  barrier();
  // Pending response callbacks cannot enqueue live work after all runs
  // terminate.
  reactor_.post([this] {
    watchdogs_.clear();
    devices_.clear();
  });
  barrier();
  reactor_.stop();
  reactor_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  started_ = false;
  finished_ = true;
  healthCv_.notify_all();
}
EngineStats Engine::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}
bool Engine::waitForHealth(int id, Health target,
                           std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return healthCv_.wait_for(lock, timeout,
                            [&] {
                              return finished_ || (health_.count(id) &&
                                                   health_.at(id) == target);
                            }) &&
         health_.count(id) && health_.at(id) == target;
}
Health Engine::health(int id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto i = health_.find(id);
  return i == health_.end() ? Health::Faulted : i->second;
}
} // namespace mo
