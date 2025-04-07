int __attribute__((__ms_abi__)) test(int i)
{
    return i;
}

/* In case -mfunction-return is in use. */
void __x86_return_thunk(void) {};
