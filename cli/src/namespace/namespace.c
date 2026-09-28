#include "namespace.h"
#include "uts.h"
#include "pid.h"

void setup_namespaces(void) {
    /* setting up the uts namespace */
    setup_uts_namespace();
    setup_pid_namespace();
}