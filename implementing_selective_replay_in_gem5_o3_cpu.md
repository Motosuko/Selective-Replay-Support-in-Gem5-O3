# Implementing Selective Replay in gem5 O3 CPU

## Abstract

This work presents a practical selective replay implementation strategy for the gem5 O3 CPU that starts from a partially completed student prototype and turns it into a runnable, debuggable, and substantially more robust design. The core idea is to assign each renamed load a replay token, propagate token dependences through renamed destination registers, track only replay-relevant instructions in a replay queue, and trigger targeted re-execution when the memory-ordering machinery reports a load violation. In the current code base, the rename stage allocates tokens and builds dependence vectors, the instruction queue maintains a replay-only side structure, and the violation path reschedules only instructions whose dependence vectors include the offending token. The implementation also adds guard rails that were missing from the older prototype, including valid-token checks, a 256-bit dependence vector, active-token masking before replay tracking, eager token release at commit, replay-queue cleanup on squash, and explicit replay-state reset before rescheduling replayed instructions.

The main finding is twofold. First, selective replay in gem5 O3 is feasible without redesigning the entire pipeline: the existing rename, IQ, mem-dependence, and commit hooks are enough to build a working token-based mechanism. Second, correctness depends much more on instruction lifetime management than on token allocation itself. The largest remaining challenge is architectural cleanliness: replay-relevant metadata should live in a dedicated structure with clear insertion and eviction rules rather than piggybacking on the IQ's general-purpose `instList`. This paper therefore describes both what is implemented now and what must change next to make selective replay robust enough for long-running experiments and publication-quality evaluation.

## Introduction

Out-of-order processors gain performance by allowing younger instructions to execute before older ones when dependences permit it. The cost of this aggressiveness is mis-speculation: when a load executes too early, or when the memory system later reveals that the load observed the wrong value, the machine must recover. A conventional response is broad squash-and-replay, which discards all younger work even if only a small subset of instructions actually depends on the offending load. Selective replay addresses this inefficiency by re-executing only the instructions that are transitively dependent on the mis-speculated operation.

This problem matters for two reasons. First, memory-order speculation is common in aggressive O3 cores, so overly broad recovery can waste issue bandwidth, reorder buffer capacity, and memory-system effort. Second, gem5 is widely used for architectural research, so an implementation defect in recovery logic can distort both performance results and simulator stability. In our case, the inherited project from the earlier CS 251A effort already had the beginnings of a token-based design, but it could not serve as a dependable research platform. It handled token allocation and register-based dependence propagation, yet it lacked a complete replay queue design and did not properly trigger or manage replay from violation detection. Worse, it coupled replay bookkeeping to the IQ's internal instruction lifetime in ways that could keep `DynInst` objects alive far too long, leak replay metadata, or crash simulation.

Our solution is to turn selective replay into a pipeline-crossing protocol with explicit responsibilities at each stage. The fetch stage creates `DynInst` objects that carry replay metadata. The rename stage allocates a token for each load and propagates token dependences through renamed physical destinations. The instruction queue records only instructions with nonzero dependence vectors in a dedicated replay queue, masking out inactive tokens so stale dependences do not accumulate. When the memory-order logic reports a violation, the IQ scans only that replay queue, identifies the replay set using the faulting load's token bit, and reschedules just those instructions. Commit frees the token and removes the corresponding bit from all replay-queue entries, while squash removes dead entries to prevent stale `DynInstPtr` retention. In addition, our recent fixes reset stale execution state before a replayed instruction is reintroduced into the pipeline, preventing old completion and commit-visible state from surviving into the replay attempt.

This work advances the state of the art in the following ways:

1. **It converts an incomplete student prototype into a coherent end-to-end selective replay design for gem5 O3.** The project now includes token allocation, dependence propagation, replay-set identification, targeted replay triggering, token cleanup, and replay-state reset in a single code path.
2. **It introduces an explicit replay queue rather than relying on whole-pipeline squash semantics alone.** This queue narrows replay work to instructions that actually carry replay-relevant metadata.
3. **It fixes key correctness hazards in token tracking.** These fixes include valid-token bounds checks, a 256-bit token bitmap, prevention of self-token insertion for loads, active-token masking to remove stale dependence bits, and eager deallocation at commit.
4. **It identifies the remaining architectural gap clearly.** The highest-priority next step is to move replay metadata out of the IQ's general `instList` lifetime and into a dedicated replay-tracking structure with well-defined eviction and cleanup policy.
5. **It provides a practical template for future gem5 recovery research.** The design is simple enough to study and extend, yet concrete enough to serve as the starting point for experiments on memory dependence prediction, replay granularity, and speculative recovery cost.

## Methods

### 1. Design overview

Our implementation follows a token-based selective replay architecture similar in spirit to the design described in *Adding Selective Replay and Bank Conflict Prediction to the Out-of-Order CPU model in Gem5* (`selective replay architecture.pdf`). Each dynamic load that reaches rename is assigned a unique replay token. That token represents the possibility that the load may later be discovered to have violated memory ordering. Any younger instruction that consumes data derived from that load inherits the token bit in a dependence vector. If a violation is later reported for that load, the machine can find the replay set by searching for the token bit instead of conservatively squashing every younger instruction.

At a high level, the mechanism is implemented as follows:

```text
          +---------+      +----------+      +------------------+
          | Fetch   | ---> | Rename   | ---> | InstructionQueue |
          | DynInst |      | token +  |      | replayQueue      |
          | create  |      | dep prop |      | depVec tracking  |
          +---------+      +----------+      +------------------+
                                                    |
                                                    | violation(load token T)
                                                    v
                                            +------------------+
                                            | replay set scan  |
                                            | depVec & T != 0  |
                                            +------------------+
                                              /              \
                                             v                v
                                    non-mem addIfReady   memDep reschedule
```

The design is intentionally incremental: it reuses existing gem5 O3 structures rather than inventing a new pipeline stage. The important implementation question is therefore not “where do we put selective replay?” but “which existing stage owns each replay responsibility?”

### 2. Hardware and simulator structures

The implementation uses the following metadata per dynamic instruction:

- `tokenID`: nonzero only for loads that successfully allocate a replay token.
- `dependenceVector`: a 256-bit OR-accumulated bit vector indicating which earlier load tokens this instruction depends on.
- `needsReplay`: a replay marker used once a violation identifies the instruction as part of the replay set.
- `tokenManager`: pointer used for token release bookkeeping.

The token state itself is managed by `TokenManager`, which keeps a static bitmap of active tokens, plus allocation/deallocation counters. The dependence vector is implemented as a 256-bit `std::bitset`, which avoids undefined behavior from over-shifting native integers and directly fixes one of the most dangerous bugs in the older prototype.

### 3. Token allocation at rename

Selective replay begins in the rename stage. When a load is renamed, the implementation calls `TokenManager::allocateTokenID(inst)` and records the assigned token in the `DynInst`. This preserves one of the valuable ideas from the earlier CS 251A project: token allocation belongs at rename because rename already has access to instruction type, physical-register mappings, and the exact dynamic instruction instance that downstream stages will manipulate.

The newer implementation keeps this policy but hardens it in two ways. First, token IDs are validated before later use, so `tokenID == 0` means “no token” and `tokenID == MaxTokenID + 1` means “allocation failed,” preventing accidental invalid shifts in later stages. Second, the token pool is widened to 256 entries and represented with a fixed-size bitset, making the representation explicit and mechanically safe.

### 4. Dependence-vector propagation

After a destination register is renamed, the rename stage computes the instruction's dependence vector by OR-ing together the dependence vectors of all renamed source registers. This is the key dataflow step: it lets the token from a load propagate transitively through arithmetic chains without having to walk the dynamic dataflow graph later.

The implementation improves on the older prototype in two important ways:

1. **No stale physical-register dependence accumulation.** The destination dependence vector is replaced, not OR-ed into an older entry. This matters because physical registers are reused; OR-ing would preserve token bits from a previous lifetime of the same physical register.
2. **No load self-token pollution.** A load's destination register receives `src_bits | own_token_bit` so downstream consumers inherit the load's token, but the load's own `dependenceVector` is updated with `src_bits` only. This avoids the classic bug where a load ends up depending on itself and never leaves replay bookkeeping.

The dataflow can be illustrated as follows:

```text
Load L1  --token t5-->  R8
   |
   +--> ADD A (src R8)      depVec(A) = {t5}
            |
            +--> MUL B      depVec(B) = {t5}
                     |
                     +--> ST C        depVec(C) = {t5}

If L1 violates memory ordering later, replay set = {A, B, C}
```

In simplified pseudocode, the rename-stage algorithm is:

```c++
src_bits = OR(dependenceVectors[renamed_src_i] for each source i)
if (inst.isLoad() && validToken(inst.tokenID)) {
    dependenceVectors[renamed_dest] = src_bits | bit(inst.tokenID)
} else {
    dependenceVectors[renamed_dest] = src_bits
}
inst.dependenceVector |= src_bits
```

This code pattern is the bridge between token allocation and replay triggering: once the dependence vector has been propagated correctly, later replay-set identification becomes a constant-time bit-test per candidate instruction.

### 5. Replay queue design

A central lesson from the failed earlier prototype is that token allocation and dependence propagation are not enough. A selective replay system also needs a **dedicated replay candidate structure**. In our design, this is `InstructionQueue::replayQueue[tid]`, a per-thread list that holds only instructions with nonzero dependence vectors.

The replay queue serves two purposes:

1. It reduces violation handling cost by avoiding a full walk over all in-flight IQ entries.
2. It gives replay-relevant metadata a separate, narrower tracking path from the rest of the scheduler's logic.

To keep replay-queue state sane, the implementation masks each instruction's `dependenceVector` against the currently active token bitmap before inserting it. This prevents a subtle stale-bit bug: if the governing load has already committed and freed its token before the dependent instruction reaches IQ insertion, the dependence bit is simply removed instead of letting the instruction wait forever for a token that no longer exists.

That said, the current system is still transitional. Replay metadata is better than before because `replayQueue` exists, but some cleanup behavior still depends on the IQ's broader `instList` lifetime. Our highest-priority next step is to make replay-relevant metadata fully independent: the replay queue should own its own insertion, eviction, and squash/commit cleanup policy rather than relying on main-IQ retirement behavior to eventually make the bookkeeping consistent.

### 6. Violation detection and replay-set identification

Selective replay becomes useful only when the memory-order subsystem can identify the offending load and notify the IQ. In our project, the detection event is the existing memory-order violation path. Once `InstructionQueue::violation(store, faulting_load)` is called, the replay logic extracts the load's token, converts it to a bit mask, and scans only the replay queue. Any younger, non-squashed instruction whose dependence vector includes the token bit becomes part of the replay set.

This is precisely the missing piece that the older prototype did not finish: replay is now triggered by violation detection, and the replay set is chosen by data dependence rather than by age alone.

### 7. Re-executing only the replay set

Once the replay set is identified, the implementation uses two replay paths:

- **Memory instructions** are passed back through the memory-dependence path with `rescheduleMemInst()` so they respect memory-order constraints on replay.
- **Non-memory instructions** have their stale execution state cleared and are re-added to the ready list with `addIfReady()`.

A key bug fix in our work is the explicit reset of stale execution state before replay. Without this step, a replayed instruction could retain old `Issued`, `Executed`, `Completed`, `ResultReady`, or `CanCommit` state and therefore carry inconsistent pipeline-visible information into its second execution. The new `prepareForSelectiveReplay()` helper clears that state and drops old recorded results before the instruction is rescheduled.

### 8. Token release and cleanup

A major simulator-stability problem in the older project was instruction lifetime leakage. If token cleanup depended only on `DynInst` destruction, but replay bookkeeping accidentally kept `DynInstPtr` references alive, then tokens would never return to the free pool and the simulator would accumulate too many in-flight instructions.

Our implementation therefore releases tokens eagerly at commit. When a load commits, the IQ walks the replay queue, clears the corresponding token bit from every dependent instruction, erases any entry whose dependence vector becomes zero, deallocates the token immediately, and then zeroes the load's `tokenID` so destruction-time cleanup cannot double-free it. On squash, the IQ also removes squashed instructions from the replay queue so dead `DynInstPtr` references do not continue to pin instruction objects in memory.

This approach is still not the final architecture we want, because replay metadata should eventually be managed by a dedicated structure rather than indirectly through `instList` retention. However, eager commit-time cleanup is already a large improvement over the older prototype because it prevents token starvation and dramatically reduces the chance of simulation crashes caused by artificially inflated dynamic-instruction lifetime.

### 9. Limitations and next step

The implementation is now substantially more complete than the older prototype, but one architectural limitation remains central: the IQ still participates too much in replay metadata lifetime. The best long-term fix is **not** to keep committed or squashed instructions artificially alive inside the IQ. Instead, replay-relevant metadata should be stored in a dedicated replay-tracking structure that has its own invariants:

- insert when a nonzero dependence vector first becomes replay-relevant;
- update when tokens are freed;
- erase on replay completion, squash, or token exhaustion cleanup;
- never force unrelated IQ retirement logic to act as replay garbage collection.

This design direction matches our practical findings: the hardest part of selective replay in gem5 is not identifying the replay set, but ensuring that the metadata describing that set has a clean lifetime.

## Related Work

Our immediate reference point was the earlier CS 251A student project hosted at `eriadnus/cs251a-final-project` [2]. That project deserves credit for two ideas that we retained: token allocation at rename using a token bitmap, and dependence-vector propagation using a map keyed by renamed physical registers. Those pieces captured the right high-level selective replay abstraction. However, after code review, we found that the project did not complete the most important mechanism: it did not provide a robust replay queue containing only replay candidates with a clear eviction policy, nor did it finish the end-to-end path from memory-order violation detection to selective re-execution of the replay set. In practice, replay bookkeeping could stay entangled with the IQ's internal lifetime, causing `DynInst` retention, token starvation, and simulator instability. The older code also suffered from concrete safety bugs, including invalid token handling that could lead to undefined bit-shift behavior.

The present work can therefore be understood as a repair-and-completion effort rather than a clean-slate redesign. Relative to the older CS 251A code base, our implementation adds the missing replay queue, connects replay triggering to the violation path, clears stale dependence bits using the active-token mask, releases tokens at commit rather than waiting on destruction, removes replay-queue entries on squash, and hardens token arithmetic with width checks and valid-token guards. The result is closer to the intended architecture described in the selective replay design document [1], but it is also more explicit about the remaining gap: replay metadata should ultimately live in a dedicated structure rather than depending on general IQ retirement behavior for cleanup.

More broadly, this work follows the classic microarchitectural insight that recovery should match the true speculative footprint of an error. Full squash recovery is simple, but it is often wasteful because many younger instructions are independent of the faulting operation. Token-based selective replay narrows the recovery domain to the transitive dependence cone of the offending load. Our contribution is to show how that idea can be integrated into gem5 O3 with relatively modest structural change while also exposing the simulator-engineering issues that matter in practice: token lifetime, pointer ownership, stale metadata cleanup, and replay-state consistency.

**Citations**

[1] Nikhita Kunati, *Adding Selective Replay and Bank Conflict Prediction to the Out-of-Order CPU model in Gem5*, `selective replay architecture.pdf` in this repository.

[2] eriadnus, *cs251a-final-project*, GitHub repository, https://github.com/eriadnus/cs251a-final-project.

## Conclusions and Future Work

This project shows that selective replay can be implemented in gem5 O3 with a relatively small set of well-placed changes, provided that the design treats replay as an end-to-end protocol rather than as a local IQ optimization. The resulting implementation now includes the essential ingredients that the earlier prototype was missing: rename-time token allocation, transitive dependence-vector propagation, a replay queue that narrows the candidate set, violation-triggered replay-set identification, targeted reissue for memory and non-memory instructions, eager token release at commit, squash-time replay-queue cleanup, and explicit replay-state reset before re-execution. Taken together, these changes move the project from an incomplete concept toward a usable research mechanism.

Our main finding is that the hardest part of selective replay is not the bit-vector logic itself; it is lifecycle management. Token allocation and dependence propagation are relatively straightforward once the rename-stage metadata flow is defined. The difficult problems appear later: ensuring that replay metadata does not outlive the instruction incorrectly, preventing stale execution state from leaking into replay, deciding when token bits should be cleared, and making sure recovery logic interacts cleanly with commit and squash. In other words, the correctness of selective replay depends less on how the replay set is identified and more on whether the simulator maintains a clean ownership model for replay-related state over the entire instruction lifetime.

Several open problems remain. First, replay metadata is still too closely coupled to the IQ's internal `instList`, which means replay garbage collection is not yet fully independent from normal scheduler bookkeeping. Second, the current `needsReplay` flag is still too weak to represent the full replay lifecycle; a richer replay-state machine would make correctness easier to reason about and debug. Third, commit-side correctness should be tightened further so that replayed instructions are validated under explicit invariants, not only by the side effects of resetting issue/commit-visible state. Fourth, the design still needs stronger evaluation under long-running workloads, denser violation patterns, and token-pressure stress tests to ensure that replay-queue cleanup and token reuse remain stable over time. Finally, the paper draft and in-repo design notes should be updated continuously so they track the exact implementation parameters and do not drift as the code evolves.

The most important next step is therefore to introduce a **dedicated replay metadata structure** with explicit insertion, update, and eviction rules. Such a structure should own replay-relevant state directly, rather than relying on the main IQ instruction lifetime to keep replay candidates alive just long enough for cleanup. From there, the project should add a replay-state enum in `DynInst`, formalize the transition rules for replay request / replay issue / replay completion, add focused regression tests for violation-triggered selective replay, and then quantify the performance impact of replay relative to full squash recovery. If those steps are completed, this project will have a much stronger foundation for both simulator stability and publishable architectural evaluation.
