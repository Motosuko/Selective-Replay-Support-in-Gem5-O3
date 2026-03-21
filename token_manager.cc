#include "cpu/o3/token_manager.hh"

#include <array>
#include <sstream>

#include "base/cprintf.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5
{

namespace o3
{

TokenManager::TokenDependenceVector TokenManager::activeTokens;
unsigned TokenManager::lastAllocatedToken = 0;
unsigned TokenManager::maxNumActiveTokens = 0;
unsigned TokenManager::currentNumActiveTokens = 0;
unsigned TokenManager::tokenOverAllocationCount = 0;

TokenManager::TokenDependenceVector
TokenManager::getTokenBit(unsigned tokenID)
{
    TokenDependenceVector bit;
    if (tokenID >= 1 && tokenID <= MaxTokenID) {
        bit.set(tokenID - 1);
    }
    return bit;
}

std::string
TokenManager::formatDependenceVector(const TokenDependenceVector &vec)
{
    constexpr size_t chunk_width = 64;
    constexpr size_t num_chunks =
        (MaxTokenID + chunk_width - 1) / chunk_width;
    std::array<uint64_t, num_chunks> chunks = {};

    for (size_t bit_idx = 0; bit_idx < MaxTokenID; ++bit_idx) {
        if (vec.test(bit_idx)) {
            const size_t chunk = bit_idx / chunk_width;
            const size_t offset = bit_idx % chunk_width;
            chunks[chunk] |= (uint64_t{1} << offset);
        }
    }

    std::ostringstream oss;
    oss << "0x";
    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
        oss << csprintf("%016llx",
                static_cast<unsigned long long>(*it));
    }
    return oss.str();
}

bool
TokenManager::allocateTokenID(const DynInstPtr &inst) {

    // Start at next token position (more likely available).
    int query_idx = lastAllocatedToken % MaxTokenID;

    // printf("Current token allocation: %lu\n", activeTokens);

    // Check to make sure we haven't gone over the max allowed token value.
    for (int query_attempt = 0; query_attempt < MaxTokenID; query_attempt++)
    {
        if (!activeTokens.test(query_idx)) {
            activeTokens.set(query_idx);
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
        activeTokens.reset(token - 1);
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
