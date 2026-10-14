#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The Passport speaks to one TheGreatMe host at a time. A host declares how
// the user ranked it right after connecting, before enabling notifications:
//   H:P\n  this device is the user's preferred host
//   H:S\n  another device is preferred
//   H:A\n  no preference: whichever host connects first keeps the Passport
// A host that never declares (an app that predates this line) stays UNKNOWN
// and behaves like before: it keeps the Passport until it disconnects.
typedef enum {
    PASSPORT_HOST_ROLE_UNKNOWN = 0,
    PASSPORT_HOST_ROLE_AUTO,
    PASSPORT_HOST_ROLE_PREFERRED,
    PASSPORT_HOST_ROLE_SECONDARY,
} passport_host_role_t;

typedef enum {
    PASSPORT_HOST_KEEP_ACTIVE = 0,
    PASSPORT_HOST_PROMOTE_CANDIDATE,
} passport_host_decision_t;

// ATT application error returned to a second host that may not take over.
#define PASSPORT_HOST_BUSY_ATT_ERROR 0x80

// Returns true when payload is exactly one role line and stores its role.
bool passport_host_parse_role(const uint8_t *payload, size_t len, passport_host_role_t *role);

// Whether the Passport keeps advertising while this role holds it, so the
// preferred host can still reach it and take over.
bool passport_host_accepts_takeover(passport_host_role_t active);

// Decide what happens when a second host declares its role.
passport_host_decision_t passport_host_arbitrate(
    passport_host_role_t active,
    passport_host_role_t candidate
);

#ifdef __cplusplus
}
#endif
