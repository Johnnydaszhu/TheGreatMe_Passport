#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "passport_host.h"

static bool parse(const char *text, passport_host_role_t *role)
{
    return passport_host_parse_role((const uint8_t *)text, strlen(text), role);
}

int main(void)
{
    passport_host_role_t role = PASSPORT_HOST_ROLE_UNKNOWN;
    assert(parse("H:P\n", &role) && role == PASSPORT_HOST_ROLE_PREFERRED);
    assert(parse("H:S\n", &role) && role == PASSPORT_HOST_ROLE_SECONDARY);
    assert(parse("H:A\n", &role) && role == PASSPORT_HOST_ROLE_AUTO);
    assert(parse("H:A", &role) && role == PASSPORT_HOST_ROLE_AUTO);

    // Board lines and malformed role lines must fall through to the board parser.
    assert(!parse("H:X\n", &role));
    assert(!parse("H:PP\n", &role));
    assert(!parse("P:42\n", &role));
    assert(!parse("H:P\nP:42\n", &role));
    assert(!passport_host_parse_role(NULL, 0, &role));

    // Only a secondary host keeps advertising for a takeover.
    assert(passport_host_accepts_takeover(PASSPORT_HOST_ROLE_SECONDARY));
    assert(!passport_host_accepts_takeover(PASSPORT_HOST_ROLE_PREFERRED));
    assert(!passport_host_accepts_takeover(PASSPORT_HOST_ROLE_AUTO));
    assert(!passport_host_accepts_takeover(PASSPORT_HOST_ROLE_UNKNOWN));

    // The preferred host wins against anyone but another preferred host.
    assert(passport_host_arbitrate(PASSPORT_HOST_ROLE_SECONDARY, PASSPORT_HOST_ROLE_PREFERRED)
           == PASSPORT_HOST_PROMOTE_CANDIDATE);
    assert(passport_host_arbitrate(PASSPORT_HOST_ROLE_AUTO, PASSPORT_HOST_ROLE_PREFERRED)
           == PASSPORT_HOST_PROMOTE_CANDIDATE);
    assert(passport_host_arbitrate(PASSPORT_HOST_ROLE_UNKNOWN, PASSPORT_HOST_ROLE_PREFERRED)
           == PASSPORT_HOST_PROMOTE_CANDIDATE);
    assert(passport_host_arbitrate(PASSPORT_HOST_ROLE_PREFERRED, PASSPORT_HOST_ROLE_PREFERRED)
           == PASSPORT_HOST_KEEP_ACTIVE);
    assert(passport_host_arbitrate(PASSPORT_HOST_ROLE_SECONDARY, PASSPORT_HOST_ROLE_SECONDARY)
           == PASSPORT_HOST_KEEP_ACTIVE);
    assert(passport_host_arbitrate(PASSPORT_HOST_ROLE_SECONDARY, PASSPORT_HOST_ROLE_AUTO)
           == PASSPORT_HOST_KEEP_ACTIVE);

    puts("passport host arbitration tests passed");
    return 0;
}
