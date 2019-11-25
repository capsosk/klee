//===-- AddressSpace.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "AddressSpace.h"
#include "CoreStats.h"
#include "Memory.h"
#include "TimingSolver.h"

#include "klee/Expr/Expr.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/KValue.h"

using namespace klee;

///

void AddressSpace::bindObject(const MemoryObject *mo, ObjectState *os) {
  assert(os->copyOnWriteOwner==0 && "object already has owner");
  os->copyOnWriteOwner = cowKey;
  objects = objects.replace(std::make_pair(mo, os));
  if (mo->segment != 0)
    segmentMap = segmentMap.replace(std::make_pair(mo->segment, mo));
}

void AddressSpace::unbindObject(const MemoryObject *mo) {
  if (mo->segment != 0)
    segmentMap = segmentMap.remove(mo->segment);
  objects = objects.remove(mo);
  // NOTE MemoryObjects are reference counted, *mo is deleted at this point
}

const ObjectState *AddressSpace::findObject(const MemoryObject *mo) const {
  const MemoryMap::value_type *res = objects.lookup(mo);
  
  return res ? res->second : 0;
}

ObjectState *AddressSpace::getWriteable(const MemoryObject *mo,
                                        const ObjectState *os) {
  assert(!os->readOnly);

  if (cowKey==os->copyOnWriteOwner) {
    return const_cast<ObjectState*>(os);
  } else {
    ObjectState *n = new ObjectState(*os);
    n->copyOnWriteOwner = cowKey;
    objects = objects.replace(std::make_pair(mo, n));
    return n;    
  }
}

/// 

bool AddressSpace::resolveConstantAddress(const KValue &pointer,
                                          ObjectPair &result) const {
  uint64_t segment = cast<ConstantExpr>(pointer.getSegment())->getZExtValue();
  uint64_t address = 0;

  if (isa<ConstantExpr>(pointer.getValue())) {
    address = cast<ConstantExpr>(pointer.getValue())->getZExtValue();
  }

  if (segment == 0 && address != 0) {
    const auto it = concreteAddressMap.find(address);
    if (it != concreteAddressMap.end())
      segment = it->second;
  }

  if (segment != 0) {
    if (const SegmentMap::value_type *res = segmentMap.lookup(segment)) {
      // TODO bounds check?
      result = *objects.lookup(res->second);
      return true;
    }
  }
  return false;
}

bool AddressSpace::resolveOne(ExecutionState &state,
                              TimingSolver *solver,
                              const KValue &pointer,
                              ObjectPair &result,
                              bool &success) const {
  if (pointer.isConstant()) {
    success = resolveConstantAddress(pointer, result);
    if (!success) {
      ResolutionList resList;
      resolveAddressWithOffset(state, solver, pointer.getOffset(), resList);
      if (resList.size() == 1) {
        success = true;
        result = resList.at(0);
      }
    }
    return true;
  } else {
    ref<ConstantExpr> segment = dyn_cast<ConstantExpr>(pointer.getSegment());
    if (segment.isNull()) {
      TimerStatIncrementer timer(stats::resolveTime);
      if (!solver->getValue(state, pointer.getSegment(), segment))
        return false;
    }

    if (!segment->isZero()) {
      return resolveConstantAddress(KValue(segment, pointer.getOffset()), result);
    }

    // didn't work, now we have to search
    MemoryObject hack;
    MemoryMap::iterator oi = objects.upper_bound(&hack);
    MemoryMap::iterator begin = objects.begin();
    MemoryMap::iterator end = objects.end();
      
    MemoryMap::iterator start = oi;
    while (oi!=begin) {
      --oi;
      const MemoryObject *mo = oi->first;
        
      bool mayBeTrue;
      if (!solver->mayBeTrue(state, 
                             mo->getBoundsCheckPointer(pointer), mayBeTrue))
        return false;
      if (mayBeTrue) {
        result = *oi;
        success = true;
        return true;
      } else {
        bool mustBeTrue;
        if (!solver->mustBeTrue(state, 
                                UgeExpr::create(pointer.getOffset(), mo->getBaseExpr()),
                                mustBeTrue))
          return false;
        if (mustBeTrue)
          break;
      }
    }

    // search forwards
    for (oi=start; oi!=end; ++oi) {
      const MemoryObject *mo = oi->first;

      bool mustBeTrue;
      if (!solver->mustBeTrue(state, 
                              UltExpr::create(pointer.getOffset(), mo->getBaseExpr()),
                              mustBeTrue))
        return false;
      if (mustBeTrue) {
        break;
      } else {
        bool mayBeTrue;

        if (!solver->mayBeTrue(state, 
                               mo->getBoundsCheckPointer(pointer),
                               mayBeTrue))
          return false;
        if (mayBeTrue) {
          result = *oi;
          success = true;
          return true;
        }
      }
    }

    success = false;
    return true;
  }
}

bool AddressSpace::resolve(ExecutionState &state,
                           TimingSolver *solver,
                           const KValue &pointer,
                           ResolutionList &rl,
                           unsigned maxResolutions,
                           time::Span timeout) const {
  if (isa<ConstantExpr>(pointer.getSegment()))
    return resolveConstantSegment(state, solver, pointer, rl, maxResolutions, timeout);

  bool mayBeTrue;
  ref<Expr> zeroSegment = ConstantExpr::create(0, pointer.getWidth());
  if (!solver->mayBeTrue(state, Expr::createIsZero(pointer.getSegment()), mayBeTrue))
    return true;
  if (mayBeTrue && resolveConstantSegment(state, solver,
                                          KValue(zeroSegment, pointer.getValue()),
                                          rl, maxResolutions, timeout))
    return true;
  // TODO inefficient
  TimerStatIncrementer timer(stats::resolveTime);
  for (const SegmentMap::value_type &res : segmentMap) {
    if (timeout && timeout < timer.delta())
      return true;
    ref<Expr> segmentExpr = ConstantExpr::create(res.first, pointer.getWidth());
    ref<Expr> expr = EqExpr::create(pointer.getSegment(), segmentExpr);
    if (!solver->mayBeTrue(state, expr, mayBeTrue))
      return true;
    if (mayBeTrue)
      rl.push_back(*objects.lookup(res.second));
  }
  return false;
}

bool AddressSpace::resolveConstantSegment(ExecutionState &state,
                                          TimingSolver *solver,
                                          const KValue &pointer,
                                          ResolutionList &rl,
                                          unsigned maxResolutions,
                                          time::Span timeout) const {
  if (!cast<ConstantExpr>(pointer.getSegment())->isZero()) {
    ObjectPair res;
    if (resolveConstantAddress(pointer, res))
      rl.push_back(res);
    return false;
  }
  
  resolveAddressWithOffset(state, solver, pointer.getOffset(), rl);

  return false;
}
void AddressSpace::resolveAddressWithOffset(const ExecutionState &state,
                                            TimingSolver *solver,
                                            const ref<Expr> &address,
                                            ResolutionList &rl) const {
  if(!isa<ConstantExpr>(address))
    return;

  ObjectPair op;
  for (const auto pair: concreteAddressMap) {
    auto segment = pair.second;
    const auto *res = segmentMap.lookup(segment);

    if (!res)
      continue;

    op = *objects.lookup(res->second);
    auto offset = SubExpr::alloc(address, ConstantExpr::alloc(pair.first, Expr::Int64));
    auto check = op.first->getBoundsCheckOffset(offset);
    bool mayBeTrue = false;
    if (solver->mayBeTrue(state, check, mayBeTrue)) {
      if (mayBeTrue)
        rl.push_back(op);
    }
  }
}

// These two are pretty big hack so we can sort of pass memory back
// and forth to externals. They work by abusing the concrete cache
// store inside of the object states, which allows them to
// transparently avoid screwing up symbolics (if the byte is symbolic
// then its concrete cache byte isn't being used) but is just a hack.

void AddressSpace::copyOutConcretes(const ConcreteAddressMap &resolved, bool ignoreReadOnly) {
  for (MemoryMap::iterator it = objects.begin(), ie = objects.end();
       it != ie; ++it) {
    const MemoryObject *mo = it->first;

    auto pair = resolved.find(mo->segment);
    if (pair == resolved.end())
      continue;

    if (!mo->isUserSpecified) {
      ObjectState *os = it->second;
      auto address = reinterpret_cast<std::uint8_t*>(pair->second);

      // if the allocated real virtual process' memory
      // is less that the size bound, do not try to write to it...
      if (os->getSizeBound() > mo->allocatedSize)
        continue;

      if (!os->readOnly || ignoreReadOnly) {
        if (address) {
          auto &concreteStore = os->offsetPlane->concreteStore;
          concreteStore.resize(os->offsetPlane->sizeBound,
                               os->offsetPlane->initialValue);
          memcpy(address, concreteStore.data(), concreteStore.size());
        }
      }
    }
  }
}

bool AddressSpace::copyInConcretes(const ConcreteAddressMap &resolved) {
  for (MemoryMap::iterator it = objects.begin(), ie = objects.end(); 
       it != ie; ++it) {
    const MemoryObject *mo = it->first;
    auto pair = resolved.find(mo->segment);
    if (pair == resolved.end())
      continue;

    if (!mo->isUserSpecified) {
      const ObjectState *os = it->second;

      if (!copyInConcrete(mo, os, pair->second))
        return false;
    }
  }

  return true;
}

bool AddressSpace::copyInConcrete(const MemoryObject *mo, const ObjectState *os,
                                  const uint64_t &resolvedAddress) {
  auto address = reinterpret_cast<std::uint8_t*>(resolvedAddress);
  // TODO segment
  auto &concreteStoreR = os->offsetPlane->concreteStore;
  if (memcmp(address, concreteStoreR.data(), concreteStoreR.size())!=0) {
    if (os->readOnly) {
      return false;
    } else {
      ObjectState *wos = getWriteable(mo, os);
      auto &concreteStoreW = wos->offsetPlane->concreteStore;
      memcpy(concreteStoreW.data(), address, concreteStoreW.size());
    }
  }
  return true;
}

/***/

bool MemoryObjectLT::operator()(const MemoryObject *a, const MemoryObject *b) const {
  return a->id < b->id;
}

