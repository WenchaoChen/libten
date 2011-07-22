#include "task.hh"

#include <algorithm>
#include <set>

namespace detail {
__thread runner *runner_ = NULL;
}

// statics
atomic_count task::ntasks(0);
runner::list runner::runners;
mutex runner::tmutex;

static void milliseconds_to_timespec(unsigned int ms, timespec &ts) {
    // convert milliseconds to seconds and nanoseconds
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
}

static unsigned int timespec_to_milliseconds(const timespec &ts) {
    // convert timespec to milliseconds
    unsigned int ms = ts.tv_sec * 1000;
    // 1 millisecond is 1 million nanoseconds
    ms += ts.tv_nsec / 1000000;
    return ms;
}

static std::ostream &operator << (std::ostream &o, task_deque &l) {
    o << "[";
    for (task_deque::iterator i=l.begin(); i!=l.end(); ++i) {
        o << *i << ",";
    }
    o << "]";
    return o;
}

struct task_timeout_heap_compare {
    bool operator ()(const task *a, const task *b) const {
        // handle -1 case, which we want at the end
        if (a->ts.tv_sec < 0) return true;
        return a->ts > b->ts;
    }
};

size_t runner::count() {
    mutex::scoped_lock l(runner::tmutex);
    return runner::runners.size();
}

typedef std::vector<epoll_event> event_vector;

void runner::run_queued_tasks() {
    mutex::scoped_lock l(mut);
    while (!runq.empty()) {
        task *c = runq.front();
        runq.pop_front();
        l.unlock();
        task::swap(&scheduler, c);
        switch (c->state) {
        case task::state_idle:
            // task wants to sleep
            l.lock();
            break;
        case task::state_running:
            c->state = task::state_idle;
            l.lock();
            runq.push_back(c);
            break;
        case task::state_exiting:
            delete c;
            l.lock();
            break;
        case task::state_migrating:
            if (c->in) {
                c->in->add_to_runqueue(c);
            } else {
                add_to_empty_runqueue(c);
            }
            l.lock();
            break;
        default:
            abort();
            break;
        }
    }
}

template <typename SetT> struct in_set {
    SetT &set;

    in_set(SetT &s) : set(s) {}

    bool operator()(typename SetT::key_type &k) {
        return set.count(k) > 0;
    }
};

void runner::check_io() {
    event_vector events(efd.maxevents ? efd.maxevents : 1);
    if (waiters.empty()) return;
    bool done = false;
    while (!done) {
        timespec now;
        // TODO: probably should cache now for runner
        // avoid calling it every add_waiter()
        THROW_ON_ERROR(clock_gettime(CLOCK_MONOTONIC, &now));
        std::make_heap(waiters.begin(), waiters.end(), task_timeout_heap_compare());
        int timeout_ms = -1;
        assert(!waiters.empty());
        if (waiters.front()->ts.tv_sec > 0) {
            if (waiters.front()->ts <= now) {
                // epoll_wait must return immediately
                timeout_ms = 0;
            } else {
                timeout_ms = timespec_to_milliseconds(waiters.front()->ts - now);
                // help avoid spinning on timeouts smaller than 1 ms
                if (timeout_ms <= 0) timeout_ms = 1;
            }
        }

        if (events.empty()) {
            events.resize(efd.maxevents ? efd.maxevents : 1);
        }
        efd.wait(events, timeout_ms);

        std::set<task *> wake_tasks;
        for (event_vector::const_iterator i=events.begin();
            i!=events.end();++i)
        {
            task *t = pollfds[i->data.fd].first;
            pollfd *pfd = pollfds[i->data.fd].second;
            if (t && pfd) {
                pfd->revents = i->events;
                add_to_runqueue(t);
                wake_tasks.insert(t);
            } else {
                // TODO: otherwise we might want to remove fd from epoll
                fprintf(stderr, "event for fd: %i but has no task\n", i->data.fd);
            }
        }

        THROW_ON_ERROR(clock_gettime(CLOCK_MONOTONIC, &now));
        for (task_heap::iterator i=waiters.begin(); i!=waiters.end(); ++i) {
            task *t = *i;
            if (t->ts.tv_sec > 0 && t->ts <= now) {
                wake_tasks.insert(t);
                add_to_runqueue(t);
            }
        }

        task_heap::iterator nend =
            std::remove_if(waiters.begin(), waiters.end(), in_set<std::set<task *> >(wake_tasks));
        waiters.erase(nend, waiters.end());
        // there are tasks to wake up!
        if (!wake_tasks.empty()) done = true;
    }
}

void runner::schedule() {
    detail::runner_->append_to_list();
    try {
        for (;;) {
            run_queued_tasks();
            check_io();

            // check_io might have filled runqueue again
            mutex::scoped_lock l(mut);
            if (runq.empty()) {
                if (task::ntasks > 0) {
                    // block waiting for tasks to be scheduled on this runner
                    sleep(l);
                } else {
                    l.unlock();
                    wakeup_all_runners();
                    break;
                }
            }
        }
    } catch (...) {}
    detail::runner_->remove_from_list();
}

void *runner::start(void *arg) {
    using namespace detail;
    runner_ = (runner *)arg;
    thread::self().detach();
    try {
        detail::runner_->schedule();
    } catch (...) {}
    delete detail::runner_;
    detail::runner_ = NULL;
    return NULL;
}

task::task(const proc &f_, size_t stack_size)
    : co((coroutine::proc)task::start, this, stack_size),
    f(f_), state(state_idle)
{
    ++ntasks;
}

task *task::self() {
    return runner::self()->get_task();
}

void task::swap(task *from, task *to) {
    // TODO: wrong place for this code. put in scheduler
    runner::self()->set_task(to);
    if (to->state == state_idle)
        to->state = state_running;
    if (from->state == state_running)
        from->state = state_idle;
    // do the actual coro swap
    from->co.swap(&to->co);
    if (from->state == state_idle)
        from->state = state_running;
    runner::self()->set_task(from);
    // don't modify to state after
    // because it might have been changed
}

void task::spawn(const proc &f) {
    task *t = new task(f);
    runner::self()->add_to_runqueue(t);
}

void task::yield() {
    assert(task::self() != &runner::self()->scheduler);
    task::self()->co.swap(&runner::self()->scheduler.co);
}

void task::start(task *t) {
    try {
        t->f();
    } catch(std::exception &e) {
        fprintf(stderr, "exception in task(%p): %s\n", t, e.what());
        abort();
    }
    // NOTE: the scheduler deletes tasks in exiting state
    // so this function won't ever return. don't expect
    // objects on this stack to have the destructor called
    t->state = state_exiting;
    t->co.swap(&runner::self()->scheduler.co);
}

void task::migrate(runner *to) {
    task *t = task::self();
    assert(t->co.main() == false);
    // if "to" is NULL, first available runner is used
    // or a new one is spawned
    // logic is in schedule()
    t->in = to;
    t->state = state_migrating;
    task::yield();
    // will resume in other runner
}

void runner::add_to_empty_runqueue(task *c) {
    mutex::scoped_lock l(tmutex);
    bool added = false;
    for (runner::list::iterator i=runners.begin(); i!=runners.end(); ++i) {
        if ((*i)->add_to_runqueue_if_asleep(c)) {
            added = true;
            break;
        }
    }
    if (!added) {
        new runner(c);
    }
}

void task::sleep(unsigned int ms) {
    task *t = task::self();
    assert(t->co.main() == false);
    runner *r = runner::self();
    milliseconds_to_timespec(ms, t->ts);
    r->add_waiter(t);
    task::yield();
}

void task::suspend(mutex::scoped_lock &l) {
    assert(co.main() == false);
    state = task::state_idle;
    in = runner::self();
    l.unlock();
    task::yield();
    l.lock();
}

void task::resume() {
    assert(in->add_to_runqueue(this) == true);
}

void runner::add_waiter(task *t) {
    timespec abs;
    if (t->ts.tv_sec != -1) {
        THROW_ON_ERROR(clock_gettime(CLOCK_MONOTONIC, &abs));
        t->ts += abs;
    }
    t->state = task::state_idle;
    waiters.push_back(t);
}

bool task::poll(int fd, short events, unsigned int ms) {
    pollfd fds = {fd, events, 0};
    return task::poll(&fds, 1, ms);
}

int task::poll(pollfd *fds, nfds_t nfds, int timeout) {
    task *t = task::self();
    assert(t->co.main() == false);
    runner *r = runner::self();
    // TODO: maybe make a state for waiting io?
    t->state = state_idle;
    if (timeout) {
        milliseconds_to_timespec(timeout, t->ts);
    } else {
        t->ts.tv_sec = -1;
        t->ts.tv_nsec = -1;
    }
    r->add_waiter(t);

    for (nfds_t i=0; i<nfds; ++i) {
        epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        // make room for the highest fd number
        int fd = fds[i].fd;
        if (r->pollfds.size() <= fd) {
            r->pollfds.resize(fd+1);
        }
        r->pollfds[fd].first = t;
        r->pollfds[fd].second = &fds[i];
        // TODO: need more tests for edge triggered events
        ev.events = fds[i].events | EPOLLET;
        ev.data.fd = fd;
        assert(r->efd.add(fd, ev) == 0);
    }

    //t->fds = fds;
    //t->nfds = nfds;
    // will be woken back up by epoll loop in schedule()
    task::yield();

    //t->fds = NULL;
    //t->nfds = 0;

    // TODO: figure out a way to not need to remove on every loop by using EPOLLET
    // right now the epoll_event.data.ptr is on the task's stack, so not possible.
    int rvalue = 0;
    for (nfds_t i=0; i<nfds; ++i) {
        if (fds[i].revents) rvalue++;
        //assert(r->efd.remove(fds[i].fd) == 0);
        r->pollfds[fds[i].fd].first = 0;
        r->pollfds[fds[i].fd].second = 0;
    }
    return rvalue;
}
