#pragma once
// Native ESP-IDF/FreeRTOS cancellation; no S31 dependencies.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#ifndef CUCKOO_STOP_HOST
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
namespace cuckoo_stop {
enum class Kind { Command, Automatic };
using Ticket = uint64_t;
class Scope;
class Gate {
    friend class Scope;
    struct Node { TaskHandle_t task; Ticket ticket; Kind kind; Node* next; };
    std::recursive_mutex fence_;
    std::condition_variable_any drained_;
    std::recursive_mutex media_;
    Node* scopes_ = nullptr;
    Ticket epoch_ = 1;
    size_t active_ = 0;
    bool armed_ = false, closing_ = false;
    TaskHandle_t controller_ = nullptr;
    unsigned control_depth_ = 0;
    Node* CurrentLocked() const {
        auto task = xTaskGetCurrentTaskHandle();
        for (auto* p = scopes_; p; p = p->next) if (p->task == task) return p;
        return nullptr;
    }
    bool WritableLocked() const {
        if (!armed_) return true;  // peripheral construction only
        if (control_depth_ && controller_ == xTaskGetCurrentTaskHandle()) return true;
        auto* p = CurrentLocked();
        return !closing_ && p && p->ticket == epoch_;
    }
public:
    static Gate& Instance() { static Gate gate; return gate; }
    void Arm() { std::lock_guard<std::recursive_mutex> lock(fence_); armed_ = true; }
    bool Live() { std::lock_guard<std::recursive_mutex> lock(fence_); return WritableLocked(); }
    Ticket CurrentTicket() { std::lock_guard<std::recursive_mutex> lock(fence_); auto* p = CurrentLocked(); return p ? p->ticket : 0; }
    bool Closing() { std::lock_guard<std::recursive_mutex> lock(fence_); return closing_; }
    bool ReserveService() { std::lock_guard<std::recursive_mutex> lock(fence_); if (closing_) return false; ++active_; return true; }
    bool Automatic() {
        std::lock_guard<std::recursive_mutex> lock(fence_);
        auto* p = CurrentLocked(); return p && p->kind == Kind::Automatic;
    }
    template<class F> bool Write(F&& f) {
        std::lock_guard<std::recursive_mutex> lock(fence_);
        if (!WritableLocked()) return false;
        f(); return true;
    }
    // Metadata for accepted background jobs may survive a mechanical epoch change.
    // Never perform audio, allocation or hardware I/O inside this short lock.
    template<class F> bool MediaState(F&& f) {
        std::lock_guard<std::recursive_mutex> lock(fence_);
        if (closing_) return false;
        f(); return true;
    }
    // Only fast state/GPIO/LEDC writes belong here. Never audio/network/delay/task create.
    template<class F> Ticket Cancel(F&& f) {
        std::lock_guard<std::recursive_mutex> lock(fence_);
        ++epoch_; if (!epoch_) ++epoch_;
        auto prior = controller_; auto depth = control_depth_;
        controller_ = xTaskGetCurrentTaskHandle(); ++control_depth_;
        f(); controller_ = prior; control_depth_ = depth;
        return epoch_;
    }
    bool IsEpoch(Ticket ticket) {
        std::lock_guard<std::recursive_mutex> lock(fence_);
        return !closing_ && ticket == epoch_;
    }
    // Serializes slow media effects against later media operations, NOT against StopAll.
    template<class F> bool Media(F&& f) {
        std::lock_guard<std::recursive_mutex> media(media_);
        { std::lock_guard<std::recursive_mutex> lock(fence_);
          auto* p = CurrentLocked();
          if (closing_ || (p && p->ticket != epoch_)) return false; }
        f(); return true;
    }
    template<class F> bool MediaAt(Ticket epoch, F&& f) {
        std::lock_guard<std::recursive_mutex> media(media_);
        if (!IsEpoch(epoch)) return false;
        f(); return true;
    }
    template<class F> void MediaOwner(F&& f) {
        std::lock_guard<std::recursive_mutex> media(media_); f();
    }
    struct Reservation { Ticket ticket = 0; Kind kind = Kind::Command; };
    Reservation Reserve() {
        std::lock_guard<std::recursive_mutex> lock(fence_);
        if (!WritableLocked() || closing_) return {};
        auto* p = CurrentLocked(); if (!p) return {};
        ++active_; return {p->ticket, p->kind};
    }
    Reservation ReserveEpoch(Ticket ticket) {
        std::lock_guard<std::recursive_mutex> lock(fence_);
        if (closing_ || ticket != epoch_) return {};
        ++active_; return {ticket, Kind::Command};
    }
    void Unreserve() {
        std::lock_guard<std::recursive_mutex> lock(fence_);
        if (active_) --active_;
        drained_.notify_all();
    }
    void CloseAndWait() {
        std::unique_lock<std::recursive_mutex> lock(fence_);
        closing_ = true; ++epoch_;
        // Shutdown-only drain. StopAll itself never waits for workers/media.
        drained_.wait(lock, [&] { return active_ == 0; });
    }
#ifdef CUCKOO_STOP_HOST
    void ResetForTest() {
        std::lock_guard<std::recursive_mutex> lock(fence_);
        if (scopes_ || active_) std::abort();
        epoch_ = 1; armed_ = false; closing_ = false;
    }
#endif
};
class Scope {
    Gate& gate_ = Gate::Instance();
    Gate::Node node_{};
    bool registered_ = false;
public:
    explicit Scope(Kind kind = Kind::Command) { Enter({}, kind, false); }
    explicit Scope(Gate::Reservation reservation) { Enter(reservation, reservation.kind, true); }
    // A fresh performance is admitted once, before queuing children. An invalid
    // parent cannot use this constructor to exchange its stale ticket for a new one.
    explicit Scope(const std::function<bool()>& admit) {
        std::lock_guard<std::recursive_mutex> lock(gate_.fence_);
        auto* parent = gate_.CurrentLocked();
        bool allowed = !gate_.closing_ && (!parent || parent->ticket == gate_.epoch_);
        auto prior = gate_.controller_; auto depth = gate_.control_depth_;
        gate_.controller_ = xTaskGetCurrentTaskHandle(); ++gate_.control_depth_;
        bool accepted = allowed && admit();
        gate_.controller_ = prior; gate_.control_depth_ = depth;
        if (accepted) { ++gate_.epoch_; if (!gate_.epoch_) ++gate_.epoch_; }
        node_ = {xTaskGetCurrentTaskHandle(), accepted ? gate_.epoch_ : 0,
                 parent ? parent->kind : Kind::Command, gate_.scopes_};
        ++gate_.active_; gate_.scopes_ = &node_; registered_ = true;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Enter(Gate::Reservation reserved, Kind kind, bool adopt) {
        std::lock_guard<std::recursive_mutex> lock(gate_.fence_);
        auto* parent = gate_.CurrentLocked();
        node_ = {xTaskGetCurrentTaskHandle(), adopt ? reserved.ticket : (parent ? parent->ticket : gate_.epoch_),
                 parent ? parent->kind : kind, gate_.scopes_};
        if (adopt) node_.kind = reserved.kind;
        if (!adopt) ++gate_.active_;
        gate_.scopes_ = &node_; registered_ = true;
    }
    ~Scope() {
        if (!registered_) return;
        std::lock_guard<std::recursive_mutex> lock(gate_.fence_);
        auto** p = &gate_.scopes_;
        while (*p && *p != &node_) p = &(*p)->next;
        if (*p) *p = node_.next;
        if (gate_.active_) --gate_.active_;
        gate_.drained_.notify_all();
    }
    bool Valid() const {
        std::lock_guard<std::recursive_mutex> lock(gate_.fence_);
        return !gate_.closing_ && node_.ticket == gate_.epoch_;
    }
};
inline bool WaitWhile(TickType_t ticks, const std::function<bool()>& keep) {
    TickType_t quantum = pdMS_TO_TICKS(20); if (quantum < 1) quantum = 1;
    if (!keep()) return false;
    while (ticks) {
        auto slice = ticks < quantum ? ticks : quantum;
        vTaskDelay(slice); ticks -= slice;
        if (!keep()) return false;
    }
    return keep();
}
inline bool Live() { return Gate::Instance().Live(); }
inline bool Wait(TickType_t ticks) {
    TickType_t quantum = pdMS_TO_TICKS(20); if (quantum < 1) quantum = 1;
    if (!Live()) return false;
    while (ticks > 0) {
        auto slice = ticks < quantum ? ticks : quantum;
        vTaskDelay(slice); ticks -= slice;
        if (!Live()) return false;
    }
    return Live();
}
template<class T> class Value {
    std::atomic<T> value_;
public:
    Value(T value = T{}) : value_(value) {}
    Value(const Value& other) : value_(other.load()) {}
    T load(std::memory_order order = std::memory_order_seq_cst) const { return value_.load(order); }
    operator T() const { return load(); }
    void store(T value) { Gate::Instance().Write([&] { value_.store(value); }); }
    Value& operator=(T value) { store(value); return *this; }
    Value& operator=(const Value& value) { return *this = value.load(); }
    T operator++(int) { T old = load(); Gate::Instance().Write([&] { old = value_.load(); value_.store(old + 1); }); return old; }
    T operator++() { T result = load(); Gate::Instance().Write([&] { result = value_.load() + 1; value_.store(result); }); return result; }
    T operator--(int) { T old = load(); Gate::Instance().Write([&] { old = value_.load(); value_.store(old - 1); }); return old; }
    T operator--() { T result = load(); Gate::Instance().Write([&] { result = value_.load() - 1; value_.store(result); }); return result; }
    Value& operator+=(T add) { Gate::Instance().Write([&] { value_.store(value_.load() + add); }); return *this; }
    Value& operator-=(T sub) { Gate::Instance().Write([&] { value_.store(value_.load() - sub); }); return *this; }
};
struct TaskContext { Gate::Reservation reservation; std::function<void()> body; bool service = false; };
inline void TaskEntry(void* opaque) {
    { std::unique_ptr<TaskContext> context(static_cast<TaskContext*>(opaque));
      if (context->service) {
          if (!Gate::Instance().Closing()) context->body();
          Gate::Instance().Unreserve();
      } else {
          Scope scope(context->reservation);
          if (scope.Valid()) context->body();
      } }
    // FreeRTOS self-delete does not unwind C++: all scopes/closures died above.
    vTaskDelete(nullptr);
}
inline BaseType_t Launch(std::function<void()> body, const char* name, uint32_t stack,
                         UBaseType_t priority, BaseType_t core = tskNO_AFFINITY,
                         TaskHandle_t* handle = nullptr) {
    auto* context = new (std::nothrow) TaskContext{{}, std::move(body)};
    if (!context) return pdFAIL;
    context->reservation = Gate::Instance().Reserve();
    if (!context->reservation.ticket) { delete context; return pdFAIL; }
    BaseType_t result = xTaskCreatePinnedToCore(TaskEntry, name, stack, context, priority, handle, core);
    if (result != pdPASS) { Gate::Instance().Unreserve(); delete context; }
    return result;
}
inline BaseType_t LaunchPinned(void (*entry)(void*), const char* name, uint32_t stack,
                               void* arg, UBaseType_t priority, TaskHandle_t* handle,
                               BaseType_t core) {
    return Launch([entry, arg] { entry(arg); }, name, stack, priority, core, handle);
}
inline BaseType_t LaunchUnpinned(void (*entry)(void*), const char* name, uint32_t stack,
                                 void* arg, UBaseType_t priority, TaskHandle_t* handle) {
    return LaunchPinned(entry, name, stack, arg, priority, handle, tskNO_AFFINITY);
}
inline BaseType_t LaunchServiceBody(std::function<void()> body, const char* name, uint32_t stack,
                                    UBaseType_t priority, BaseType_t core = tskNO_AFFINITY) {
    auto* context = new (std::nothrow) TaskContext{{}, std::move(body), true};
    if (!context) return pdFAIL;
    if (!Gate::Instance().ReserveService()) { delete context; return pdFAIL; }
    auto result = xTaskCreatePinnedToCore(TaskEntry, name, stack, context, priority, nullptr, core);
    if (result != pdPASS) { Gate::Instance().Unreserve(); delete context; }
    return result;
}
inline BaseType_t LaunchService(void (*entry)(void*), const char* name, uint32_t stack,
                                 void* arg, UBaseType_t priority, TaskHandle_t* handle, BaseType_t core) {
    auto* context = new (std::nothrow) TaskContext{{}, [entry, arg] { entry(arg); }, true};
    if (!context) return pdFAIL;
    if (!Gate::Instance().ReserveService()) { delete context; return pdFAIL; }
    auto ret = xTaskCreatePinnedToCore(TaskEntry, name, stack, context, priority, handle, core);
    if (ret != pdPASS) { Gate::Instance().Unreserve(); delete context; }
    return ret;
}
inline BaseType_t LaunchAt(Ticket ticket, std::function<void()> body, const char* name,
                           uint32_t stack, UBaseType_t priority) {
    auto* context = new (std::nothrow) TaskContext{{}, std::move(body)};
    if (!context) return pdFAIL;
    context->reservation = Gate::Instance().ReserveEpoch(ticket);
    if (!context->reservation.ticket) { delete context; return pdFAIL; }
    auto ret = xTaskCreatePinnedToCore(TaskEntry, name, stack, context, priority, nullptr, tskNO_AFFINITY);
    if (ret != pdPASS) { Gate::Instance().Unreserve(); delete context; }
    return ret;
}
}  // namespace cuckoo_stop
#define CUCKOO_ACTION_BEGIN() cuckoo_stop::Scope cuckoo_action_scope; if (!cuckoo_action_scope.Valid()) return
#define CUCKOO_ACTION_WAIT(ticks) do { if (!cuckoo_stop::Wait(ticks)) return; } while (0)
