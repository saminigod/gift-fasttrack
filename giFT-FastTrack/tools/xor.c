#include <stdio.h>
#include "crypt/fst_crypt.h"

int main (int argc, char **argv)
{
        unsigned long seed;

        if (argc<2) {
                fprintf(stderr, "Usage: %s seed\n", *argv);
                exit(2);
        }

        seed=strtoul(argv[1], NULL, 16);

        printf("%lx\n", fst_cipher_mangle_enc_type (seed, 0));

        exit(0);
}
