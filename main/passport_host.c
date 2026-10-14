#include "passport_host.h"

bool passport_host_parse_role(const uint8_t *payload, size_t len, passport_host_role_t *role)
{
    if (!payload || !role) return false;
    if (len == 4 && payload[3] == '\n') len = 3;
    if (len != 3 || payload[0] != 'H' || payload[1] != ':') return false;
    switch (payload[2]) {
    case 'A': *role = PASSPORT_HOST_ROLE_AUTO; return true;
    case 'P': *role = PASSPORT_HOST_ROLE_PREFERRED; return true;
    case 'S': *role = PASSPORT_HOST_ROLE_SECONDARY; return true;
    default: return false;
    }
}

bool passport_host_accepts_takeover(passport_host_role_t active)
{
    // Only a host that knows another device is preferred leaves the door open.
    // AUTO and UNKNOWN keep first-come-first-served; PREFERRED cannot be beaten.
    return active == PASSPORT_HOST_ROLE_SECONDARY;
}

passport_host_decision_t passport_host_arbitrate(
    passport_host_role_t active,
    passport_host_role_t candidate
)
{
    if (candidate == PASSPORT_HOST_ROLE_PREFERRED && active != PASSPORT_HOST_ROLE_PREFERRED) {
        return PASSPORT_HOST_PROMOTE_CANDIDATE;
    }
    return PASSPORT_HOST_KEEP_ACTIVE;
}
