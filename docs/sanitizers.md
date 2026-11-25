# Sanitizer tests

The CI sanitizer job enables AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer. It uses `fast_unwind_on_malloc=0` to retain library frames in allocation stacks and applies `tests/lsan.supp` through `LSAN_OPTIONS`.

The suppression matches allocation stacks containing `libguile-3.0.so`, including Guile's process-global allocations and iconv allocations reached through Guile. LeakSanitizer matches any frame in the stack, so this also hides other leaks with a Guile frame, including leaks originating in Mortar callbacks called by Guile. Leaks without a Guile frame and other ASan and UBSan errors remain reported. Revisit the suppression when Guile or its lifecycle changes.
