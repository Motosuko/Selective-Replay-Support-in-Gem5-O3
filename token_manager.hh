#ifndef __CPU_O3_TOKEN_MANAGER_HH__
#define __CPU_O3_TOKEN_MANAGER_HH__

#include "base/types.hh"
#include "cpu/o3/dyn_inst_ptr.hh"

namespace gem5
{

namespace o3
{

/** Aimed for Selective Replay Support */
class TokenManager
{
  public:

    typedef uint64_t TokenDependenceVector;

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
