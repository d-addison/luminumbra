#pragma once

// Deliberately diagnosed by modernize-use-nullptr in an included header probe.
inline int* TidyHeaderSmoke() {
    return 0;
}
