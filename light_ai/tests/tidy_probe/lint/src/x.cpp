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
