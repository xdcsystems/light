// rules_lint.sh self-test (#93): one violation of each kind except those in include/.
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
