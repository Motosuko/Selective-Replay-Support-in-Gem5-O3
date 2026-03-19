# Selective replay implementation review

This note reviews the current selective replay implementation against the intended token-based selective replay design described by `selective replay architecture.pdf` and the code comments that document the same model.

## Overall assessment

The implementation captures the **basic idea** of token-based selective replay:

- each renamed load gets a token;
- token dependencies are propagated through renamed destination registers;
- the IQ keeps a replay queue of instructions with non-zero dependency vectors;
- a memory-order violation reissues only instructions whose dependency vector contains the faulting load's token.

That said, the current implementation is **not yet fully correct or architecturally clean**. The biggest issues are not in token allocation itself, but in the **replay lifecycle** and the way replayed instructions interact with the normal IQ/commit pipeline.

## Main issues to correct

### 1. Replayed instructions are not reset to a clean pre-execute state

In `InstructionQueue::violation()`, non-memory instructions are replayed by only clearing `Issued` and `CanIssue`, then calling `addIfReady()`. Memory instructions are replayed through `rescheduleMemInst()`, which only clears `CanIssue` and resets translation state. This is incomplete.

Why this is a problem:

- The instruction may still be marked `Executed`, `Completed`, `ResultReady`, and/or `CanCommit`.
- Reissuing an instruction that still carries post-execution state is logically inconsistent.
- The ROB/commit side may still observe the instruction as commit-ready even though the replay has not actually produced a corrected result yet.

What should improve:

- Introduce an explicit helper that transitions a replayed instruction back into a well-defined replay state.
- Clear at least the execution/completion/commit-related flags that make the old execution visible.
- Re-establish a single, consistent replay path for memory and non-memory instructions.

Files involved:

- `inst_queue.cc`: `InstructionQueue::violation()` and `InstructionQueue::rescheduleMemInst()`.

### 2. Commit is explicitly not gated on replay completion

The comment in `InstructionQueue::commit()` states that an instruction can reach commit with `needsReplay == true` and treats that as benign because "the ROB commit path is not gated on the replay result." This is a serious architectural warning sign.

Why this is a problem:

- In a selective replay design, an instruction identified for replay should not retire architecturally until the replayed execution is the one that is validated.
- If an instruction can commit while still marked for replay, the implementation is relying on timing assumptions instead of enforcing correctness.
- This becomes especially dangerous for instructions that feed stores, control flow, or other architecturally visible state.

What should improve:

- Replay should become part of the correctness contract, not just a best-effort resubmission.
- Either the ROB/commit stage must block retirement of replay-marked instructions, or the pipeline must squash and rebuild all younger architecturally exposed state when a replay is required.

Files involved:

- `inst_queue.cc`: `InstructionQueue::commit()`.

### 3. The IQ keeps committed dependent instructions alive until tokens are freed

`InstructionQueue::commit()` removes an instruction from `instList` only when `dependenceVector == 0`. That means a committed instruction can remain in the IQ's tracking list solely because it still depends on some in-flight load token.

Why this is a problem:

- It mixes two different responsibilities: architectural retirement and replay bookkeeping.
- The IQ ends up retaining already-committed instructions as replay metadata carriers.
- The current comments show this was added partly to avoid leaks/stalls, which suggests the design is compensating for missing lifecycle separation.

What should improve:

- Separate replay bookkeeping from the core IQ lifetime rules.
- Committed instructions should not need to stay in the main IQ tracking list just so token sweeping still works.
- If replay metadata must outlive issue/execute, store it in a dedicated replay structure rather than overloading `instList`.

Files involved:

- `inst_queue.cc`: `InstructionQueue::commit()`.
- `inst_queue.hh`: `replayQueue` and `instList` interactions.

### 4. Replay membership is tracked, but replay completion is not modeled cleanly

The implementation marks matching instructions with `needsReplay = true` during `violation()`, but that flag is later cleared on squash or commit. There is no clear completion point that says: "this instruction has now replayed successfully and its old speculative execution is superseded."

Why this is a problem:

- `needsReplay` acts more like a sticky annotation than a replay state machine.
- There is no explicit transition for replay requested -> replay issued -> replay executed -> replay validated.
- Without this state model, it is difficult to reason about corner cases, double replay, or interactions with squash.

What should improve:

- Replace the single boolean with explicit replay states or a small replay status enum.
- Ensure the commit path can distinguish between not replayed yet vs replayed successfully.

Files involved:

- `dyn_inst.hh`: `needsReplay`.
- `inst_queue.cc`: replay and commit handling.

### 5. Token management is globally static but instantiated in multiple stages

There is a `TokenManager` member in both fetch and rename, while the actual token state (`activeTokens`, counters, last allocation) is static in `TokenManager`.

Why this is a problem:

- Functionally, the static data means all instances share one global token pool anyway.
- Architecturally, this is confusing: the design appears to have multiple token managers, but the behavior is actually global.
- `DynInst` objects are constructed with a pointer to fetch's `TokenManager`, while allocation happens in rename's `TokenManager`. This works only because the underlying token pool is static.

What should improve:

- Make the design explicit: either use one shared token manager owned by the CPU/O3 core, or make the API fully static and remove per-stage instances.
- Avoid passing around an instance pointer when the real state is global.

Files involved:

- `fetch.hh` / `fetch.cc`.
- `rename.hh` / `rename.cc`.
- `dyn_inst.hh` / `dyn_inst.cc`.
- `token_manager.hh` / `token_manager.cc`.

### 6. Dependency propagation is register-based only, which is narrower than a full replay dependency model

The current propagation logic derives dependence only through renamed source/destination registers. That is a reasonable first cut, but it is narrower than a full selective replay dependency model.

Why this matters:

- It does not explicitly model non-register architectural effects.
- It relies on the existing pipeline machinery to indirectly cover all relevant cases.
- Any instruction whose replay relevance is not faithfully represented by renamed register dependencies may be missed.

What should improve:

- Clearly document that the current design is register-dataflow selective replay, not a complete replay dependence graph.
- Audit whether condition-code, predicate, address-generation, and non-speculative cases are fully covered by renamed source tracking in this O3 model.

Files involved:

- `rename.cc`: dependence propagation.
- `dyn_inst.hh`: dependence vector storage.

## Things that look reasonable

These parts are logically sound improvements over a more naive implementation:

- Replacing destination dependence vectors instead of OR-ing into stale physical-register state.
- Preventing a load from inserting its own token into its own dependency vector.
- Masking dependency vectors against the active token set before adding them to replay tracking.
- Eager token release at commit, with destructor-time cleanup as a safety net.
- Removing squashed instructions from the replay queue to avoid stale `DynInstPtr` retention.

## Recommended next steps

1. Add a real replay state machine to `DynInst`.
2. Reset replayed instructions to a clean executable state before reinsertion.
3. Block commit for instructions that still require replay validation.
4. Separate replay bookkeeping lifetime from the IQ's normal committed-instruction lifetime.
5. Consolidate token management into a single shared owner.
6. Add focused tests for:
   - load/store ordering violations;
   - replay of ALU chains dependent on a faulting load;
   - replay of memory instructions dependent on a faulting load;
   - squash + replay interactions;
   - token exhaustion and token reuse.

## Bottom line

The current code is a **promising prototype**, but not yet a fully correct or fully logical implementation of selective replay as an architectural mechanism. The dependency-vector idea is solid; the weak point is the lack of a rigorous replay lifecycle and commit-side correctness enforcement.
