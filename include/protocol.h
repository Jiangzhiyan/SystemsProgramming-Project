#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <sys/types.h>

#define SOCK_ADDRESS "/tmp/optimus_prime"

struct prime_request {
    long int index;
    long int number;
};

struct prime_reply {
    long int index;
    long int result;
};

#endif /* PROTOCOL_H */
