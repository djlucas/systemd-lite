/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdio.h>

int main(int argc, char *argv[]) {
        (void) argc;
        (void) argv;
        /* TODO: implement standalone socket activation manager. */
        fprintf(stderr, "systemd-socketd: not implemented yet\n");
        return 1;
}
