#pragma once

#include <Foundation/Memory/AllocatorWrapper.h>
#include <Foundation/Memory/CommonAllocators.h>
#include <Foundation/Containers/HybridArray.h>
#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>

/// \brief Stack-like temporary allocator for Jolt, backed by a list of memory chunks.
///
/// Allocations must be freed in reverse order. A request that doesn't fit into the current chunk moves on to the next chunk,
/// which skips any existing chunk that is too small. Free unwinds the same way.
class plJoltTempAlloc : public JPH::TempAllocator
{
public:
  plJoltTempAlloc(const char* szName)
    : m_ProxyAlloc(szName, plFoundation::GetAlignedAllocator())
  {
    AddChunk(0);
    m_uiCurChunkIdx = 0;
  }

  ~plJoltTempAlloc()
  {
    for (plUInt32 i = 0; i < m_Chunks.GetCount(); ++i)
    {
      ClearChunk(i);
    }
  }

  virtual void* Allocate(JPH::uint inSize) override
  {
    if (inSize == 0)
      return nullptr;

    const plUInt32 uiNeeded = plMemoryUtils::AlignSize(inSize, 16u);

    while (true)
    {
      const plUInt32 uiRemaining = m_Chunks[m_uiCurChunkIdx].m_uiSize - m_Chunks[m_uiCurChunkIdx].m_uiLastOffset;

      if (uiRemaining >= uiNeeded)
        break;

      AddChunk(uiNeeded);
    }

    auto& lastAlloc = m_Chunks[m_uiCurChunkIdx];

    void* pRes = plMemoryUtils::AddByteOffset(lastAlloc.m_pPtr, lastAlloc.m_uiLastOffset);
    lastAlloc.m_uiLastOffset += uiNeeded;
    return pRes;
  }

  virtual void Free(void* pInAddress, JPH::uint inSize) override
  {
    if (pInAddress == nullptr)
      return;

    const plUInt32 uiAllocSize = plMemoryUtils::AlignSize(inSize, 16u);

    // Allocate may skip previously allocated chunks that are too small. Skip those
    // empty chunks when unwinding as well, rather than subtracting from offset zero.
    while (m_uiCurChunkIdx > 0 && m_Chunks[m_uiCurChunkIdx].m_uiLastOffset == 0)
      --m_uiCurChunkIdx;

    auto& lastAlloc = m_Chunks[m_uiCurChunkIdx];
    PL_ASSERT_ALWAYS(lastAlloc.m_uiLastOffset >= uiAllocSize, "Jolt temporary allocator underflow.");
    PL_ASSERT_DEV(plMemoryUtils::AddByteOffset(lastAlloc.m_pPtr, lastAlloc.m_uiLastOffset - uiAllocSize) == pInAddress, "Jolt temporary allocations must be freed in reverse order.");
    lastAlloc.m_uiLastOffset -= uiAllocSize;

    if (lastAlloc.m_uiLastOffset == 0 && m_uiCurChunkIdx > 0)
    {
      // move back to the previous chunk
      --m_uiCurChunkIdx;
    }
  }

  struct Chunk
  {
    void* m_pPtr = nullptr;
    plUInt32 m_uiSize = 0;
    plUInt32 m_uiLastOffset = 0;
  };

  void AddChunk(plUInt32 uiSize)
  {
    ++m_uiCurChunkIdx;

    if (m_uiCurChunkIdx < m_Chunks.GetCount())
      return;

    uiSize = plMath::Max(uiSize, 1024u * 1024u);

    auto& alloc = m_Chunks.ExpandAndGetRef();
    alloc.m_pPtr = PL_NEW_RAW_BUFFER(&m_ProxyAlloc, plUInt8, uiSize);
    alloc.m_uiSize = uiSize;
  }

  void ClearChunk(plUInt32 uiChunkIdx)
  {
    PL_DELETE_RAW_BUFFER(&m_ProxyAlloc, m_Chunks[uiChunkIdx].m_pPtr);
    m_Chunks[uiChunkIdx].m_pPtr = nullptr;
    m_Chunks[uiChunkIdx].m_uiSize = 0;
    m_Chunks[uiChunkIdx].m_uiLastOffset = 0;
  }

  plUInt32 m_uiCurChunkIdx = 0;
  plHybridArray<Chunk, 16> m_Chunks;
  plProxyAllocator m_ProxyAlloc;
};
