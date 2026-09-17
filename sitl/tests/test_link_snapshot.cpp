#include "NimbusLinkSnapshot.h"
#include <cstdlib>

static void check(bool result)
{
    if (!result)
        std::abort();
}
#include <cstdint>
#include <cstdio>

int main()
{
    NimbusLinkSnapshot tx;
    check(tx.due(0, false)); // first client learns the real disconnected state
    check(!tx.due(1, false));
    check(tx.due(10, true)); // RF transitions are immediate
    check(!tx.due(1009, true));
    check(
        tx.due(1010, true)); // a new SDK process receives context without RF loss
    check(!tx.due(1011, true));
    check(tx.due(1100, false)); // no delayed link-loss indication
    check(!tx.due(2099, false));
    check(tx.due(2100, false)); // reconnection never invents an active radio link
    NimbusLinkSnapshot wrap;
    check(wrap.due(UINT32_MAX - 499U, true));
    check(!wrap.due(499, true));
    check(wrap.due(500, true));
    puts("Link snapshot: startup, SDK restart, immediate RF loss and clock wrap "
         "passed.");
}
