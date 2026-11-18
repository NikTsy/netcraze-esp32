#include <assert.h>
#include "observation.h"

int main(void)
{
    exit_result_t healthy = {.measured = true, .probes = {{true, true}, {true, true}, {true, true}}};
    round_result_t round = {.primary = healthy, .backup = healthy, .restored = true};
    int failures = 0;
    round.primary.probes[0].http_ok = false;
    assert(!observe_round(&round, &failures) && failures == 1);
    assert(!observe_round(&round, &failures) && failures == 2);
    assert(observe_round(&round, &failures) && failures == 3);

    for (int service = 0; service < PROBE_COUNT; service++) {
        round.backup = healthy;
        round.backup.probes[service].http_ok = false;
        assert(!observe_round(&round, &failures) && failures == 3);
        round.backup = healthy;
        round.backup.probes[service].nat_ok = false;
        assert(!observe_round(&round, &failures) && failures == 3);
    }
    round.backup = healthy;
    round.backup.measured = false;
    assert(!observe_round(&round, &failures));
    round.backup = healthy;
    round.primary = healthy;
    assert(!observe_round(&round, &failures) && failures == 0);

    round.primary.probes[1].nat_ok = false;
    assert(!observe_round(&round, &failures) && failures == 1);
    round.primary.measured = false;
    assert(!observe_round(&round, &failures) && failures == 0);
    round.primary.measured = true;
    assert(!observe_round(&round, &failures) && failures == 1);
    round.restored = false;
    assert(!observe_round(&round, &failures) && failures == 0);
}
