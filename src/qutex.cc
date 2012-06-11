#include "ten/task/qutex.hh"
#include "proc.hh"

namespace ten {

void qutex::lock() {
    task *t = this_proc()->ctask;
    DCHECK(t) << "BUG: qutex::lock called outside of task";
    {
        std::unique_lock<std::timed_mutex> lk(_m);
        DCHECK(_owner != t) << "no recursive locking";
        if (_owner == nullptr) {
            _owner = t;
            DVLOG(5) << "LOCK qutex: " << this << " owner: " << _owner;
            return;
        }
        DVLOG(5) << "QUTEX[" << this << "] lock waiting add: " << t <<  " owner: " << _owner;
        _waiting.push_back(t);
    }

    try {
        // loop to handle spurious wakeups from other threads
        for (;;) {
            t->swap();
            std::unique_lock<std::timed_mutex> lk(_m);
            if (_owner == this_proc()->ctask) {
                break;
            }
        }
    } catch (...) {
        std::unique_lock<std::timed_mutex> lk(_m);
        internal_unlock(lk);
        throw;
    }
}

bool qutex::try_lock() {
    task *t = this_proc()->ctask;
    DCHECK(t) << "BUG: qutex::try_lock called outside of task";
    std::unique_lock<std::timed_mutex> lk(_m, std::try_to_lock);
    if (lk.owns_lock()) {
        if (_owner == nullptr) {
            _owner = t;
            return true;
        }
    }
    return false;
}

void qutex::unlock() {
    std::unique_lock<std::timed_mutex> lk(_m);
    internal_unlock(lk);
}

inline void qutex::internal_unlock(std::unique_lock<std::timed_mutex> &lk) {
    task *t = this_proc()->ctask;
    DCHECK(lk.owns_lock()) << "BUG: lock not owned " << t;
    DVLOG(5) << "QUTEX[" << this << "] unlock: " << t;
    if (t == _owner) {
        if (!_waiting.empty()) {
            t = _owner = _waiting.front();
            _waiting.pop_front();
        } else {
            t = _owner = nullptr;
        }
        DVLOG(5) << "UNLOCK qutex: " << this
            << " new owner: " << _owner
            << " waiting: " << _waiting.size();
        lk.unlock();
        // must use t here, not owner because
        // lock has been released
        if (t) t->ready();
    } else {
        // this branch is taken when exception is thrown inside
        // a task that is currently waiting inside qutex::lock
        auto i = std::find(_waiting.begin(), _waiting.end(), t);
        if (i != _waiting.end()) {
            _waiting.erase(i);
        }
    }
}

} // namespace
