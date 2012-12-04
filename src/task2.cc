#include "ten/task2/task.hh"
#include <atomic>
#include <array>
#include <algorithm>

namespace ten {
namespace task2 {

std::ostream &operator << (std::ostream &o, task::state s) {
    static std::array<const char *, 6> names = {{
        "fresh",
        "ready",
        "asleep",
        "canceled",
        "unwinding",
        "finished"
    }};
    o << names[static_cast<int>(s)];
    return o;
}

std::ostream &operator << (std::ostream &o, const task *t) {
    if (t) {
        o << "task[" << t->_id << "," << (void *)t << "," << t->_state << "]";
    } else {
        o << "task[null]";
    }
    return o;
}

task *runtime::current_task() {
    return thread_local_ptr<runtime>()->_current_task;
}

namespace this_task {
uint64_t get_id() {
    return runtime::current_task()->get_id();
}

void yield() {
    runtime::current_task()->yield();
}
} // end namespace this_task

/////// task ///////

namespace {
    std::atomic<uint64_t> task_id_counter{0};
}

task::cancellation_point::cancellation_point() {
    task *t = runtime::current_task();
    ++t->_cancel_points;
}

task::cancellation_point::~cancellation_point() {
    task *t = runtime::current_task();
    --t->_cancel_points;
}

uint64_t task::next_id() {
    return ++task_id_counter;
}

template <class Arg>
bool valid_states(Arg to) {
    return false;
}

template <class Arg, class... Args>
bool valid_states(Arg to, Arg valid, Args ...others) {
    if (to == valid) return true;
    return valid_states(to, others...);
}

void task::trampoline(intptr_t arg) {
    task *self = reinterpret_cast<task *>(arg);
    try {
        if (self->transition(state::ready)) {
            self->_f();
        }
    } catch (task_interrupted &e) {
    }
    self->transition(state::finished);
    self->_f = nullptr;

    runtime *r = self->_runtime;
    r->remove_task(self);
    r->schedule();
    // never get here
    LOG(FATAL) << "Oh no! You fell through the trampoline" << self;
}

bool task::transition(state to) {
    bool valid = true;
    while (valid) {
        state from = _state;
        switch (from) {
            case state::fresh:
                // from fresh we can go directly to finished
                // without needing to unwind
                if (to == state::canceled) {
                    to = state::finished;
                }
                valid = valid_states(to, state::ready, state::finished);
                break;
            case state::ready:
                valid = valid_states(to, state::asleep, state::canceled, state::finished);
                break;
            case state::asleep:
                valid = valid_states(to, state::ready, state::canceled);
                break;
            case state::canceled:
                valid = valid_states(to, state::unwinding, state::finished);
                break;
            case state::unwinding:
                valid = valid_states(to, state::finished);
                break;
            case state::finished:
                valid = valid_states(to);
                break;
            default:
                // bug
                std::terminate();
        }

        if (valid) {
            if (_state.compare_exchange_weak(from, to)) {
                return true;
            }
        }
    }
    return false;
}

void task::cancel() {
    if (transition(state::canceled)) {
        DVLOG(5) << "canceling: " << this << "\n";
        _runtime->ready(this);
    }
}

void task::join() {
    // TODO: implement
}

void task::yield() {
    DVLOG(5) << "readyq yield " << this;
    _runtime->_readyq.push_back(this);
    _runtime->schedule();
}

void task::post_swap() {
    state st = _state;
    if (st == state::canceled && _cancel_points > 0) {
        if (transition(state::unwinding)) {
            DVLOG(5) << "unwinding task: " << this;
            throw task_interrupted();
        }
    }

    if (_exception != nullptr) {
        std::exception_ptr exception = _exception;
        _exception = nullptr;
        std::rethrow_exception(exception);
    }
}

int runtime::dump() {
#ifdef TEN_TASK_TRACE
    runtime *r = thread_local_ptr<runtime>();
    for (shared_task &t : r->_alltasks) {
        LOG(INFO) << t.get();
        LOG(INFO) << t->_trace.str();
    }
#endif
    return 0;
}

void runtime::ready(task *t) {
    if (this != thread_local_ptr<runtime>()) {
        _dirtyq.push(t);
        // TODO: speed this up?
        std::unique_lock<std::mutex> lock{_mutex};
        _cv.notify_one();
    } else {
        DVLOG(5) << "readyq runtime ready: " << t;
        _readyq.push_back(t);
    }
}

void runtime::remove_task(task *t) {
    DVLOG(5) << "remove task " << t;
    using namespace std;
    //{
    //    auto i = find(begin(_readyq), end(_readyq), t);
    //    _readyq.erase(i);
    //}

    // TODO: needed?
    //_alarms.remove(t);

    auto i = find_if(begin(_alltasks), end(_alltasks),
            [t](shared_task &other) {
            return other.get() == t;
            });
    _gctasks.push_back(*i);
    _alltasks.erase(i);
}

void runtime::check_dirty_queue() {
    task *t = nullptr;
    while (_dirtyq.pop(t)) {
        DVLOG(5) << "readyq adding " << t << " from dirtyq";
        _readyq.push_back(t);
    }
}

void runtime::check_timeout_tasks() {
    _alarms.tick(_now, [this](task *t, std::exception_ptr exception) {
        if (t->transition(task::state::ready)) {
            if (exception != nullptr && t->_exception == nullptr) {
                t->_exception = exception;
            }
            ready(t);
        }
    });
}

void runtime::schedule() {
    CHECK(!_alltasks.empty());
    task *self = _current_task;

    do {
        check_dirty_queue();
        update_cached_time();
        check_timeout_tasks();

        if (_readyq.empty()) {
            std::unique_lock<std::mutex> lock{_mutex};
            if (_alarms.empty()) {
                _cv.wait(lock);
            } else {
                _cv.wait_until(lock, _alarms.front_when());
            }
        }
    } while (_readyq.empty());

    using ::operator <<;
    //DVLOG(5) << "readyq: " << _readyq;

    task *t = _readyq.front();
    _readyq.pop_front();
    _current_task = t;
    DVLOG(5) << self << " swap to " << t;
#ifdef TEN_TASK_TRACE
    self->_trace.capture();
#endif
    self->_ctx.swap(t->_ctx, reinterpret_cast<intptr_t>(t));
    _current_task = self;
    _gctasks.clear();
    self->post_swap();
}

deadline::deadline(std::chrono::milliseconds ms) {
    if (ms.count() < 0)
        throw errorx("negative deadline: %jdms", intmax_t(ms.count()));
    if (ms.count() > 0) {
        runtime *r = thread_local_ptr<runtime>();
        task *t = r->_current_task;
        _alarm = std::move(
                runtime::alarm_set_type::alarm(
                    r->_alarms, t, ms+runtime::now(), deadline_reached()
                    )
                );
        DVLOG(1) << "deadline alarm armed: " << _alarm._armed << " in " << ms.count() << "ms";
    }
}

void deadline::cancel() {
    _alarm.cancel();
}

deadline::~deadline() {
    cancel();
}

std::chrono::milliseconds deadline::remaining() const {
    return _alarm.remaining();
}

} // task2
} // ten

