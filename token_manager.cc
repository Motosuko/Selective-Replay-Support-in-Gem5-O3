#include "cpu/o3/token_manager.hh"

#include "cpu/o3/limits.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5
{

namespace o3
{

uint64_t TokenManager::activeTokens = 0;
unsigned TokenManager::lastAllocatedToken = 0;
unsigned TokenManager::maxNumActiveTokens = 0;
unsigned TokenManager::currentNumActiveTokens = 0;
unsigned TokenManager::tokenOverAllocationCount = 0;

bool
TokenManager::allocateTokenID(const DynInstPtr &inst) {

    // Start at next token position (more likely available).
    int query_idx = lastAllocatedToken % MaxTokenID;

    // printf("Current token allocation: %lu\n", activeTokens);

    // Check to make sure we haven't gone over the max allowed token value.
    for (int query_attempt = 0; query_attempt < MaxTokenID; query_attempt++)
    {
        if (!(activeTokens & ((uint64_t) 1 << query_idx))) { // if token with index "query_idx" is not-allocated, let's allocate it
            activeTokens |= ((uint64_t) 1 << query_idx);
            inst->tokenID = query_idx + 1; // Represents a token, value 1 - max tokens
            lastAllocatedToken = query_idx + 1;
            _incrementCurrentActiveTokenCount();
            return true;
        }

        query_idx = (query_idx+1) % MaxTokenID;
    }

    // If we didn't exit early, then we have no tokens to allocate; loudly proclaim this error
    _incrementOverAllocationCount();
    // printf("ERROR: Max number of tokens (%d) reached. Number of errors now: %d\n", MaxTokenID, tokenOverAllocationCount);
    inst->tokenID = MaxTokenID + 1; // impossible token, but setting to non-zero for testing purposes.
    return false; // indicates failure to allocate
}

bool
TokenManager::deallocateTokenID(unsigned token) {

    if (token <= MaxTokenID && token > 0) {
        activeTokens &= ~(1 << (token-1)); // Unset allocation flag for token.
        // printf("Current token allocation during deallocation: %lu\n", activeTokens);
        _decrementCurrentActiveTokenCount();
        return true;
    }

    return false;
}

void
TokenManager::_incrementCurrentActiveTokenCount() {
    currentNumActiveTokens++;
    if (currentNumActiveTokens > maxNumActiveTokens)
        maxNumActiveTokens = currentNumActiveTokens;
}

void
TokenManager::_decrementCurrentActiveTokenCount() {
    if (currentNumActiveTokens > 0)
        currentNumActiveTokens--;
}

void
TokenManager::_incrementOverAllocationCount() {
    tokenOverAllocationCount++;
}

void
TokenManager::_resetOverAllocationCount() {
    tokenOverAllocationCount = 0;
}

} // namespace o3
} // namespace gem5