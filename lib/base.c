/* UniBin base library: console and basic timing support. */
#include <stdio.h>
#include <unistd.h>

static void ub_base_timeout(unsigned int seconds) {
    while (seconds > 0) seconds = sleep(seconds);
}
