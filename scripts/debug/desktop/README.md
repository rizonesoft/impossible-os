# scripts\debug\desktop -- Desktop test runners

Empty placeholder for desktop UI test runners.

The desktop UI test framework is owned by
[`todo/00-infrastructure/TODO-05-desktop-ui-test-framework.md`](../../../todo/00-infrastructure/TODO-05-desktop-ui-test-framework.md).
When that TODO ships, per-test bat files (and a `run-all-desktop-tests.bat`
aggregate) land here, parallel to the layout in
[`scripts\debug\kernel\`](../kernel/) and [`scripts\debug\usermode\`](../usermode/).

The root-level [`scripts\debug\run-all-tests.bat`](../run-all-tests.bat)
already invokes this directory (currently a no-op, since it is empty);
once the bat files exist, it will pick them up automatically.
