#ifndef __CPU_O3_TOKEN_MANAGER_HH__
#define __CPU_O3_TOKEN_MANAGER_HH__

#include <cstdint>

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

    typedef uint64_t TokenDependenceVector;

    // Enforce at compile time that MaxTokenID fits within the bit width of
    // TokenDependenceVector so that shift expressions like
    //   (TokenDependenceVector)1 << (tokenID - 1)
    // are never undefined behaviour (max valid shift is bit_width - 1 = 63).
    static_assert(MaxTokenID <= sizeof(TokenDependenceVector) * 8,
        "MaxTokenID exceeds the bit width of TokenDependenceVector; "
        "increase TokenDependenceVector width or reduce MaxTokenID.");

    /** Allocate next token for LOAD instruction */
    bool allocateTokenID(const DynInstPtr &inst);

    /** Deallocate specified token (to be used when instructions commit). */
    bool deallocateTokenID(unsigned token);

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
    
    /** Bitstring of active, allocated set of tokens */
    static uint64_t activeTokens;

    /** Last token allocation completed, enables small optimization for token allocation */
    static unsigned lastAllocatedToken;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TOKEN_MANAGER_HH__
