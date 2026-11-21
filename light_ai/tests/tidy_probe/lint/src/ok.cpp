// rules_lint.sh self-test (#93): an allowlisted suppression with a reason: no finding.
int f();  // NOLINT(custom-no-virtual): the allowlist names it
int g();  // NOLINT(misc-const-correctness): the allowlist names the built-in project rule too (#94)
