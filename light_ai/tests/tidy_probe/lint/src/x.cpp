// rules_lint.sh self-test (#93): the violations of each kind except those in include/ and src/sdk.cpp.
#include "x.hpp"
#include <gst/gst.h>
// xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
// жжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжжж
int a();  // NOLINT(custom-no-virtual)
// NOLINTNEXTLINE(custom-no-static-local): a reason, but not on the allowlist
int b();
int c();  // NOLINT
int d();  // NOLINT(google-readability-casting): not a custom check, not counted
// NOLINTBEGIN(*): switches every check off
int e();
// NOLINTEND(*)
int g();  // NOLINT(readability-x): a reason NOLINT(custom-no-catch-all): a second one in the same line
int h();  // NOLINT(*-catch-all,cust*): globs that clang-tidy expands
int i();  // NOLINT(custom-no-such-check): not a real check, switches nothing off
int j();  // NOLINT(custom-no-v?rtual): a ? is literal in clang-tidy, switches nothing off
// NOLINTBEGIN(custom-no-catch-all): a reason and an allowlist line do not make a range allowable
int k();
// NOLINTEND(custom-no-catch-all)
// NOLINTBEGIN(readability-x): not a custom check, not counted
int l();
// NOLINTEND(readability-x)
// NOLINTBEGIN -- a bare range switches every check off: nolint-range *, never allowed
int m();
// NOLINTEND -- a bare END only closes it: silent
int n();  // NOLINT(misc-const-correctness)
int o();  // NOLINT(misc-*): a glob over the built-in project rule (#94)
// NOLINTBEGIN(misc-const-correctness): a range of the built-in project rule is never allowed either
int p();
// NOLINTEND(misc-const-correctness)
int q();  // NOLINT(misc-unused-parameters): a built-in check that is not a project rule, not counted
