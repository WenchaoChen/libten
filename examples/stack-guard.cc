#include "ten/task.hh"

using namespace ten;

static void stack_overflow() {
    char buf[256*1024];
    // this will attempt to write to the guard page
    // and cause a segmentation fault
    char crash = 1;

    // -O3 reorders buf and crash, so it will crash here
    buf[0] = 1;
    printf("%i %i\n", buf[0], crash);
}

int main() {
    return task::main([] {
        task::spawn(stack_overflow);
    });
}
