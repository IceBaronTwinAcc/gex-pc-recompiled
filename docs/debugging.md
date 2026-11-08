# Step-by-step debugging

Use this process for any defect, not only rendering or browser problems. Finish each step with
a concrete result before moving on. Read [project-structure.md](project-structure.md)
for navigation and the [testing skill](../.github/skills/testing/SKILL.md)
before building or launching applications.

## Procedure

1. **Define the failure before changing anything.** Write down the triggering
   action, expected result, actual result, and how to recognize success.
   Separate multiple symptoms; they may have different causes. Ask for missing
   information only when it changes the investigation.
   **Result:** a specific failure and an observable acceptance condition.
2. **Preserve the failing environment.** Inspect instructions, working-tree
   changes, build configuration, selected implementations, dependencies, and
   relevant settings. Record the actual executable or artifact being used.
   Do not overwrite user data, reset unrelated changes, replace an existing
   service, or silently substitute another platform, dependency, or backend.
   **Result:** enough environment information to repeat the same failure.
3. **Reproduce and capture a baseline.** Use the smallest safe sequence that
   exhibits the problem. Record the input, output, error, and timing needed
   to distinguish failure from success. Bound automated runs and isolate their
   settings. If only the user's environment fails, collect evidence there;
   a passing substitute environment is a control, not a reproduction.
   **Result:** a repeatable failing case, or a clearly stated reproduction gap.
4. **Follow the actual execution path.** Start at the failing operation and
   follow its callers, selected implementation, data transformations, and
   external interfaces. Check build selection rather than guessing from filenames.
   Read relevant functions and their surrounding state, not only matching lines.
   Extend the search only when the current path points somewhere new.
   **Result:** a short chain showing where inputs become the observed output.
5. **Find the first incorrect boundary.** Compare what each component receives
   with what it produces. Check values, dimensions, units, ownership, state,
   return codes, and ordering as relevant. For stalls, distinguish waiting for
   work from waiting for a resource, lock, event, or consumer. Start with existing
   logs and tests; add narrowly scoped instrumentation only for missing evidence.
   Keep it opt-in or temporary, bounded, and free of sensitive data.
   **Result:** the earliest observed violation and the evidence supporting it.
6. **Make a testable hypothesis.** State a proposed cause, the observation it
   predicts, and an observation that would disprove it. Separate facts from
   hypotheses. Prefer an experiment that distinguishes competing explanations;
   do not change several variables and then attribute success to one of them.
   **Result:** one hypothesis and one discriminating experiment.
7. **Run the experiment and update the explanation.** Compare with the baseline
   under the same inputs and settings. Check whether the predicted observation
   actually occurred. An action can change multiple states, so measure those
   states before interpreting the result. If disproved, retain the useful
   evidence, discard the hypothesis, and return to the first unexplained boundary.
   **Result:** a supported cause or a specific next hypothesis, not another
   unexplained workaround.
8. **Fix the cause at the responsible layer.** Identify the violated contract
   and restore it using existing patterns and interfaces. Check callers and
   related transitions affected by the change. Keep platform-specific behavior
   in its platform implementation. Do not suppress errors, discard required
   work, or add a success-shaped fallback to hide the symptom. Change generated
   behavior through its generator or rewrite, never by hand-editing generated code.
   **Result:** a focused patch with an explanation of why it addresses the cause.
9. **Verify the original failure and nearby behavior.** Repeat the baseline
   with the fixed artifact and unchanged reproduction conditions. Assert the
   actual acceptance condition, not a proxy such as a nonzero counter or lack
   of a crash. Add a focused regression test where feasible and exercise relevant
   boundaries, error paths, or transitions. Escalate testing only as needed.
   If a user-only reproduction remains unverified, state that limitation.
   **Result:** evidence for the requested outcome and preserved related behavior.
10. **Clean up and leave a precise handoff.** Remove only your own temporary
    probes and processes when no longer needed; retain concise evidence for
    unresolved problems and preserve the runnable artifact. Update directly
    related documentation without touching protected files. Do not commit or
    push unless requested. Report what is fixed, what remains uncertain, and
    the next required action if blocked.
    **Result:** a persistent change and an honest completion status.

## Keeping a small-context investigation coherent

Keep a compact checkpoint in session artifacts, not a growing transcript or an
unrequested repository notes file. Update it when the explanation changes:

| Item | Record |
| --- | --- |
| Failure | Trigger, expected result, actual result, acceptance condition. |
| Environment | Artifact, configuration, settings, dependency/backend identity. |
| Evidence | Relevant file/function locations and concise capture/log paths. |
| Current boundary | Last correct input and first incorrect output or state. |
| Hypothesis | Proposed cause, prediction, and disproof condition. |
| Experiment | Single variable changed and observed result. |
| Status | Confirmed facts, rejected explanations, unresolved symptoms, next action. |

On resuming, read this checkpoint and the relevant source before acting.
Do not promote an earlier assumption to a fact or repeat an experiment whose
result is already known. If experiments stop reducing uncertainty, summarize
the evidence and request the specific missing input rather than applying
more speculative changes.

For additional domain-specific checks, see
[renderer debugging](renderer-debugging.md),
[build and test troubleshooting](build-test-troubleshooting.md), and
[platform build and testing routes](platform-build-testing.md).
