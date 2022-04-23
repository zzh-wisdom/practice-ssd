enum states {
        FOO_START = 0,
        FOO_END,
        BAR_START,
        BAR_END
};
 
struct state_machine {
        enum states state;
 
        int count;
};
 
static void
foo_complete(void *ctx)
{
    struct state_machine *sm = ctx;
 
    sm->state = FOO_END;
    run_state_machine(sm);
}
 
static void
foo(struct state_machine *sm)
{
    do_async_op(foo_complete, sm);
}
 
static void
bar_complete(void *ctx)
{
    struct state_machine *sm = ctx;
 
    sm->state = BAR_END;
    run_state_machine(sm);
}
 
static void
bar(struct state_machine *sm)
{
    do_async_op(bar_complete, sm);
}
 
static void
run_state_machine(struct state_machine *sm)
{
    enum states prev_state;
 
    do {
        prev_state = sm->state;
 
        switch (sm->state) {
            case FOO_START:
                foo(sm);
                break;
            case FOO_END:
                /* This is the loop condition */
                if (sm->count++ < 5) {
                    sm->state = FOO_START;
                } else {
                    sm->state = BAR_START;
                }
                break;
            case BAR_START:
                bar(sm);
                break;
            case BAR_END:
                break;
        }
    } while (prev_state != sm->state);
}
 
void do_async_for(void)
{
        struct state_machine *sm;
 
        sm = malloc(sizeof(*sm));
        sm->state = FOO_START;
        sm->count = 0;
 
        run_state_machine(sm);
}