// rules_lint.sh self-test (#93): an allowlisted suppression with a reason: no finding.
int f();  // NOLINT(custom-no-virtual): the allowlist names it
