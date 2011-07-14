#define BOOST_TEST_MODULE thread test
#include <boost/test/unit_test.hpp>
#include <boost/bind.hpp>

#include "thread.hh"

#include <iostream>

static void bar(pid_t p) {
    BOOST_CHECK_NE(thread::self()->id(), p);
    BOOST_CHECK(coroutine::self());
    coroutine::yield();
}

static void foo(pid_t p, mutex::scoped_lock &l) {
    BOOST_CHECK_NE(thread::self()->id(), p);
    coroutine::spawn(boost::bind(bar, p));
    l.unlock();
}

BOOST_AUTO_TEST_CASE(constructor_test) {
    mutex m;
    mutex::scoped_lock l(m);
    BOOST_CHECK(l.trylock() == false);

    pid_t pid = thread::self()->id();
    std::cout << "main pid: " << pid << "\n";
    BOOST_CHECK(pid);
    thread *t = thread::spawn(boost::bind(foo, pid, boost::ref(l)));
    l.lock();
}

static void co1(int &count) {
    count++;
    coroutine::yield();
    count++;
}

BOOST_AUTO_TEST_CASE(scheduler) {
    thread *t = thread::self();
    int count = 0;
    for (int i=0; i<10; ++i) {
        coroutine *c = coroutine::spawn(boost::bind(co1, boost::ref(count)));
    }
    t->schedule(false);
    BOOST_CHECK_EQUAL(20, count);
}

static void mig_co(mutex::scoped_lock &l) {
    pid_t start_pid = thread::self()->id();
    coroutine::migrate();
    pid_t end_pid = thread::self()->id();
    BOOST_CHECK_NE(start_pid, end_pid);
    l.unlock();
}

BOOST_AUTO_TEST_CASE(thread_migrate) {
    mutex m;
    mutex::scoped_lock l(m);

    BOOST_CHECK(thread::count() >= 0);
    thread *t = thread::spawn(boost::bind(mig_co, boost::ref(l)));
    l.lock();
    BOOST_CHECK(thread::count() > 1);
}