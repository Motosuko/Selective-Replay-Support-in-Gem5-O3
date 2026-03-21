#ifndef __CPU_O3_TOKEN_MANAGER_HH__
#define __CPU_O3_TOKEN_MANAGER_HH__

#include <bitset>
#include <cstdint>
#include <string>

#include "base/types.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"

namespace gem5
{

namespace o3
{

/** Aimed for Selective Replay Support */
class TokenManager
{
  public:

    /** Dependency-tracking vector; each bit corresponds to one replay
     *  token (token ID k maps to bit k-1).  std::bitset<MaxTokenID> keeps
     *  the bit operations explicit and allows the token pool to scale past
     *  native integer widths such as 64 or 128 bits.
     */
    using TokenDependenceVector = std::bitset<MaxTokenID>;

    /** Allocate next token for LOAD instruction */
    bool allocateTokenID(const DynInstPtr &inst);

    /** Deallocate specified token (to be used when instructions commit). */
    bool deallocateTokenID(unsigned token);

    /** Return the current bitmask of allocated (active) tokens.
     *  Used by InstructionQueue::insert() to filter out stale depVec bits
     *  at dispatch time: if a bit is set in a dependent instruction's
     *  dependenceVector but the corresponding token is no longer active
     *  (its governing load has already committed and freed the token), that
     *  bit should be cleared so the instruction does not get stuck in
     *  instList waiting for a sweep that will never happen.
     */
    static TokenDependenceVector getActiveTokens() { return activeTokens; }

    /** Return a dependence vector with only tokenID's bit set. */
    static TokenDependenceVector getTokenBit(unsigned tokenID);

    /** Format a dependence vector for debug logging. */
    static std::string formatDependenceVector(
        const TokenDependenceVector &vec);

    /** Modifiers for debugging token allocation state tracking */
    void _incrementCurrentActiveTokenCount();
    void _decrementCurrentActiveTokenCount();
    void _incrementOverAllocationCount();
    void _resetOverAllocationCount();

    /** Max and current active tokens during lifetime of TokenManager, for debugging */
    static unsigned maxNumActiveTokens;
    static unsigned currentNumActiveTokens;

    /** Count of attempted over-allocations for dependency tokens */
    static unsigned tokenOverAllocationCount;

  private:
    
    /** Bitstring of active, allocated set of tokens. */
    static TokenDependenceVector activeTokens;

    /** Last token allocation completed, enables small optimization for token allocation */
    static unsigned lastAllocatedToken;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TOKEN_MANAGER_HH__
