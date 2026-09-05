---
name: council-code-review
description: Run a code change through the "council flow" — a three-stage adversarial review (Critic finds weak points, Defender argues back, Judge issues a final verdict). Use this whenever the user asks for a "council review," "council flow," "adversarial review," wants a second opinion on a risky/contentious diff, or explicitly asks to run Critic/Defender/Judge on code. This is heavier than a normal review — prefer it for high-stakes PRs, security-sensitive changes, or when a single-pass review feels too agreeable. For a normal single-pass review, don't use this skill.
---

# Council Code Review

A three-stage adversarial review process for code changes, modeled on "LLM
council" / debate-style review: instead of one pass that tends to hedge and
agree with itself, the change is put through three **sequential, independent**
roles that each see only what they need to do their job well.

```
Critic  --->  Defender  --->  Judge
(attack)      (respond)       (verdict)
```

This is deliberately adversarial, not collaborative — the value comes from
genuine disagreement surfacing before a merge decision, not from a polite
consensus. Use it for stakes-worthy diffs (security-sensitive, hard to
reverse, contentious, or where you specifically want a harder look than a
normal review gives). For routine PRs, a normal single-pass review is faster
and sufficient — don't reach for this skill by default.

## Companion skill: code-review-skill

This skill is designed to sit alongside `code-review-skill`
(awesome-skills/code-review-skill) — that skill supplies deep per-language
and cross-cutting review knowledge (20+ language guides, security, N+1,
XSS, error handling, concurrency, architecture, universal quality
anti-patterns) and a PR triage script. Council flow supplies the adversarial
*process*; code-review-skill supplies the domain *checklists* that make the
Critic's and Defender's arguments sharp instead of generic. Check whether
`code-review-skill` is installed alongside this one (e.g. a sibling skill
directory, or its `reference/` and `scripts/` paths resolve) and use it as
described in Step 0 below. If it isn't installed, the council flow still
works — it just runs on general review knowledge instead of the deep
per-language guides.

## The flow

Run all three stages **in sequence**, each as its own subagent (Claude Code
`Task` tool call), not in parallel. Each stage's prompt template is in
`references/`. Fill in the `{{...}}` placeholders and dispatch the task with
that filled prompt as the subagent's full instructions.

### Step 0 — Gather the diff, scope, and domain checklist

Before dispatching anything, get the actual change:
1. `git diff` / `git diff --staged` for the code under review, or read the
   files the user points to.
2. If `code-review-skill`'s `scripts/pr-analyzer.py` is available, pipe the
   diff through it (`git diff main...HEAD | python scripts/pr-analyzer.py`)
   to get size/complexity/risk triage and per-file language detection for
   free — cheaper than doing this by hand, and it flags things like missing
   test changes on a risky diff before you even start.
3. Note the language/framework involved and skim the diff briefly yourself —
   you don't need a deep read, just enough to write a one- or two-line scope
   summary for the Critic (what the change is trying to do). Don't pre-judge
   quality; that's the Critic's and Defender's job, not yours.
4. **Build a domain checklist** from `code-review-skill`'s reference guides,
   if that skill is installed:
   - Load the language guide(s) matching the diff, e.g.
     `reference/react.md`, `reference/rust.md`, `reference/go.md` — see
     `code-review-skill/SKILL.md`'s language table for the full mapping.
   - Always also pull `reference/security-review-guide.md` and
     `reference/code-quality-universal.md` — they apply regardless of
     language.
   - Pull other cross-cutting guides when the diff touches their area:
     `reference/cross-cutting/sql-injection-prevention.md` and
     `reference/cross-cutting/xss-prevention.md` for anything handling
     user input, `reference/cross-cutting/n-plus-one-queries.md` for
     anything touching a DB/ORM, `reference/cross-cutting/error-handling-principles.md`
     and `reference/cross-cutting/async-concurrency-patterns.md` for
     anything with error paths or concurrency, `reference/common-bugs-checklist.md`
     for known per-language pitfalls, and `reference/architecture-review-guide.md`
     for structural/design-level changes.
   - Condense what you pulled into a short, concrete checklist (bullet
     points, not the full guide text) — this is the `{{DOMAIN_CHECKLIST}}`
     you'll hand to both the Critic and the Defender in Steps 1–2. Keep it
     to the parts actually relevant to this diff; don't paste entire guides
     into the subagent prompts.
   - If `code-review-skill` isn't installed, skip this — leave
     `{{DOMAIN_CHECKLIST}}` as "(none available — use general review
     judgment)" and proceed with the flow as normal.

### Step 1 — Critic (subagent)

Dispatch a `Task` using `references/critic-prompt.md`, filled in with the
diff, scope summary, and the `{{DOMAIN_CHECKLIST}}` from Step 0. The Critic
sees **only** the diff/scope/checklist — not any prior conversation about
the change — so it isn't anchored by earlier praise or framing.

The Critic's job is to find real weak points: correctness bugs, edge cases,
security issues, performance traps, maintainability debt, missing tests. It
is explicitly told not to balance its findings with praise or hedge with "but
this is probably fine" — that softening is the Defender's job, not the
Critic's. One-sided output here is correct, not a bug.

### Step 2 — Defender (subagent)

Dispatch a second `Task` using `references/defender-prompt.md`, filled in
with the diff, the same `{{DOMAIN_CHECKLIST}}`, and the Critic's full
output. The Defender is explicitly instructed to read the diff and trace the
code itself before answering — it is not allowed to just accept or reject
the Critic's description of what the code does; it forms its own view first,
then compares. The Defender must respond to
**every** point the Critic raised — not cherry-pick the easy ones — and for
each one say whether it's valid, overstated, mitigated elsewhere in the
codebase, or a real problem the Critic was right about, backed by its own
criticality score (1–5) and, where it concedes, its own suggested-fix diff.
A Defender that concedes nothing is as suspect as a Critic that finds
nothing; genuine concession on the strong points is expected and good.

### Step 3 — Judge (subagent)

Dispatch a third `Task` using `references/judge-prompt.md`, filled in with
the diff, the Critic's output, and the Defender's output in full — the Judge
has never seen the code change discussed any other way, only this exchange.
The Judge is also explicitly instructed to trace the code itself for each
contested finding — it doesn't resolve disagreements by picking whichever
side's prose reads more convincingly; where the two sides' traces of the
same code conflict, the Judge's own read is the tiebreaker, and if the
Judge's own trace disagrees with both sides it says so and rules on what it
actually found. The Judge weighs each contested point on its merits and
produces the final structured verdict (format below), including a final
criticality score (1–5) and a carried-forward or rewritten suggested-fix
diff for every surviving finding. The Judge does not just average or split
the difference — where the Defender's rebuttal is weak, the Judge should say
so and side with the Critic, and vice versa. If `code-review-skill` is
installed, its `reference/code-review-best-practices.md` governs *how* the
Judge phrases the summary (specific and actionable, focused on the code not
the person, questions over commands where the verdict is non-blocking) —
this affects tone only, never softens a genuine `blocking` finding into
something milder. If the user wants the verdict delivered as a postable PR
comment rather than the verdict table, use `code-review-skill`'s
`assets/pr-review-template.md` as the output shape instead.

### Step 4 — Deliver the verdict

Present the Judge's output to the user as-is; it's already in the target
format. Don't re-summarize or soften it. If the user wants to dig into a
specific point, you can pull the relevant detail back out of the Critic's or
Defender's transcript.

## Output format

The Judge writes its verdict using `assets/verdict-template.md`. Severity
labels and markers (identical to `code-review-skill`'s convention — reused
on purpose so a Council verdict and a normal `code-review-skill` review are
consistent and directly comparable):

| Label | Marker | Meaning |
|---|---|---|
| `blocking` | 🔴 | Must be fixed before merge |
| `important` | 🟡 | Should be fixed; may block depending on context |
| `nit` | 🟢 | Nice to have, not blocking |
| `suggestion` | 💡 | Alternative approach to consider |
| `learning` | 📚 | Educational note for the author, no action needed |
| `praise` | 🎉 | Explicitly worth calling out as good |

🔴/🟡/🟢 are the three severity tiers that drive the overall recommendation;
💡/📚/🎉 are non-blocking annotations that never affect it.

Each finding in the verdict carries exactly one severity label, a
**criticality score from 1 (least critical) to 5 (critical)**, a **concrete
suggested fix as a code diff**, which side (Critic, Defender, or the Judge's
own independent trace of the code) the Judge sided with, and a one-line
reason why. These three requirements — score, diff, independent trace — hold
at every stage, not just the Judge:

- **Criticality score (1–5) on every finding, at every stage.** The Critic
  scores its own findings; the Defender gives its own score per finding
  (which can differ from the Critic's); the Judge sets the final score. This
  is separate from the severity label — severity says whether it blocks
  merge, criticality says how bad it actually is if it fires.
- **Suggested fix as a code diff on every surviving finding.** A finding
  isn't complete without a concrete `- old` / `+ new` snippet showing the
  fix (or an explicit "can't be expressed as a diff, here's the one-sentence
  fix" fallback for things like "add a missing test"). Nobody in the flow —
  Critic, Defender, or Judge — should describe a problem without also
  showing the fix.
- **Independent code tracing at every stage, not just at the Critic.** The
  Defender and the Judge are each explicitly required to read the actual
  diff and trace the code path themselves before responding — they do not
  get to resolve a disagreement purely by judging whose prose sounds more
  convincing. See Steps 2–3 below and the prompt templates for how this is
  enforced.

## When not to use this

- Trivial or low-stakes diffs — the three-stage overhead isn't worth it.
- The user wants quick line-by-line style feedback, not a verdict — do a
  normal review instead.
- The user wants multiple *perspectives* surfaced for an ambiguous decision
  (not a specific known diff) — that's a different pattern (parallel
  advisors), not this sequential adversarial one.
