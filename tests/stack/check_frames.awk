# Reads gcc's -fstack-usage files (file:line:col:function<TAB>bytes<TAB>qualifier)
# and fails on any frame that is not explicitly allowed to be large.
#
# The measurement is a property of the source and the compiler, not of a run, so
# this is a build-time check: a new local buffer shows up here before it shows up
# as a dead sysmodule on someone's console.

BEGIN {
    FS = "\t"

    n = split(allow, list, " ")

    for (i = 1; i <= n; i++)
        allowed[list[i]] = 1

    bad = 0
    checked = 0
}

{
    split($1, parts, ":")
    fn = parts[4]
    size = $2 + 0

    if (fn == "")
        next

    # gcc reports the clones it made with a suffix ("...isra", "...constprop",
    # "...part.0"); the allowlist names the source function.
    sub(/\.(isra|constprop|part\.[0-9]+)$/, "", fn)

    checked++

    if (fn in allowed)
        next

    if (size >= limit) {
        printf("FAIL %s: %s uses %d bytes (limit %d)\n", FILENAME, fn, size, limit)
        bad++
    }
}

END {
    if (bad) {
        printf("%d frame(s) over the %d byte limit\n", bad, limit)
        exit 1
    }

    printf("%d functions, every stack frame under %d bytes\n", checked, limit)
}
