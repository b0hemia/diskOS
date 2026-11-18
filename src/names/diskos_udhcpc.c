/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "names.h"
#include <stdlib.h>

/* Conservative BusyBox option grammar. Unknown syntax retains the exact stock argv.
 * A bare DISC is accepted ONLY for the known unquoted SNOWSKY DISC launch. */
static int hostname_arg(int argc, char **argv) {
    int host = 0, iface = 0;
    for (int i = 1; i < argc; ++i) {
        const char *s = argv[i];
        if (!strcmp(s, "DISC") && i == argc-1 && host == i-1 &&
            !strcmp(argv[host], "hostname:SNOWSKY")) { continue; }
        if (strlen(s) != 2 || s[0] != '-') return 0;
        if (strchr("fbnqvoCRBS", s[1])) continue;
        if (!strchr("irtspTVAHhcxOF" , s[1]) || ++i >= argc ||
            !argv[i][0] || argv[i][0] == '-') return 0;
        if (s[1] == 'i') { if (iface++) return 0; }
        if (s[1] == 'h' || s[1] == 'H') return 0; /* competing/deprecated hostname */
        if (s[1] == 'x') {
            const char *v = argv[i];
            if (!strncmp(v, "hostname:", 9)) { if (host || !v[9]) return 0; host = i; }
            else {
                /* Preserve unrelated options, but reject raw option 12 and ambiguous encodings. */
                if (strncmp(v, "vendor:", 7) && strncmp(v, "lease:", 6) &&
                    strncmp(v, "0x3d:", 5) && strncmp(v, "0x3c:", 5)) return 0;
            }
        }
    }
    return iface == 1 ? host : 0;
}
int main(int argc, char **argv) {
    char name[33], option[42];
    char **out = calloc((size_t)argc + 2, sizeof *out);
    if (!out) { /* allocation failure still execs the stock applet, without recursion */
        argv[0] = "udhcpc"; execv(NAMES_BUSYBOX, argv); return 127;
    }
    out[0] = (char *)NAMES_BUSYBOX; out[1] = "udhcpc";
    for (int i = 1; i < argc; ++i) out[i+1] = argv[i];
    const char *token = getenv(NAMES_ENV);
    int host = 0;
    if (token && !strcmp(token, "v1") && active_name(name) && name[0])
        host = hostname_arg(argc, argv);
    if (host) {
        strcpy(option, "hostname:"); strcat(option, name); out[host+1] = option;
    }
    execv(NAMES_BUSYBOX, out);
    argv[0] = "udhcpc";
    execv(NAMES_BUSYBOX, argv);
    return 127;
}
