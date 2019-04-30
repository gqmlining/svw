
/*
 * Copyright (c) 2010-2014, 2017 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Kevin Lim
 *          Korey Sewell
 */

#ifndef __CPU_O3_LSQ_UNIT_IMPL_HH__
#define __CPU_O3_LSQ_UNIT_IMPL_HH__

#include "arch/generic/debugfaults.hh"
#include "arch/locked_mem.hh"
#include "base/str.hh"
#include "config/the_isa.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/lsq_unit.hh"
#include "debug/Activity.hh"
#include "debug/IEW.hh"
#include "debug/LSQUnit.hh"
#include "debug/O3PipeView.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

template<class Impl>
LSQUnit<Impl>::WritebackEvent::WritebackEvent(DynInstPtr &_inst, PacketPtr _pkt,
                                              LSQUnit *lsq_ptr)
    : Event(Default_Pri, AutoDelete),
      inst(_inst), pkt(_pkt), lsqPtr(lsq_ptr)
{
}

template<class Impl>
void
LSQUnit<Impl>::WritebackEvent::process()
{
    assert(!lsqPtr->cpu->switchedOut());

    lsqPtr->writeback(inst, pkt);

    if (pkt->senderState)
        delete pkt->senderState;

    delete pkt;
}

template<class Impl>
const char *
LSQUnit<Impl>::WritebackEvent::description() const
{
    return "Store writeback";
}

template<class Impl>
void
LSQUnit<Impl>::completeDataAccess(PacketPtr pkt)
{
    LSQSenderState *state = dynamic_cast<LSQSenderState *>(pkt->senderState);
    DynInstPtr inst = state->inst;
    DPRINTF(IEW, "Writeback event [sn:%lli].\n", inst->seqNum);
    DPRINTF(Activity, "Activity: Writeback event [sn:%lli].\n", inst->seqNum);

    if (state->cacheBlocked) {
        // This is the first half of a previous split load,
        // where the 2nd half blocked, ignore this response
        DPRINTF(IEW, "[sn:%lli]: Response from first half of earlier "
                "blocked split load recieved. Ignoring.\n", inst->seqNum);
        delete state;
        return;
    }

    // If this is a split access, wait until all packets are received.
    if (TheISA::HasUnalignedMemAcc && !state->complete()) {
        return;
    }

    assert(!cpu->switchedOut());
    if (!inst->isSquashed()) {
        if (!state->noWB) {
            // Only loads and store conditionals perform the writeback
            // after receving the response from the memory
            assert(inst->isLoad() || inst->isStoreConditional());
            if (!TheISA::HasUnalignedMemAcc || !state->isSplit ||
                !state->isLoad) {
                writeback(inst, pkt);
            } else {
                writeback(inst, state->mainPkt);
            }
        }

        if (inst->isStore()) {
            completeStore(state->idx);
        }
    }

    if (TheISA::HasUnalignedMemAcc && state->isSplit && state->isLoad) {
        delete state->mainPkt;
    }

    pkt->req->setAccessLatency();
    cpu->ppDataAccessComplete->notify(std::make_pair(inst, pkt));

    delete state;
}

template <class Impl>
LSQUnit<Impl>::LSQUnit()
    : loads(0), stores(0), storesToWB(0), cacheBlockMask(0), stalled(false),
      isStoreBlocked(false), storeInFlight(false), hasPendingPkt(false),
      pendingPkt(nullptr)
{
}

template<class Impl>
void
LSQUnit<Impl>::init(O3CPU *cpu_ptr, IEW *iew_ptr, DerivO3CPUParams *params,
        LSQ *lsq_ptr, unsigned maxLQEntries, unsigned maxSQEntries,
        unsigned id)
{
    cpu = cpu_ptr;
    iewStage = iew_ptr;

    lsq = lsq_ptr;

    lsqID = id;

    DPRINTF(LSQUnit, "Creating LSQUnit%i object.\n",id);

    // Add 1 for the sentinel entry (they are circular queues).
    LQEntries = maxLQEntries + 1;
    SQEntries = maxSQEntries + 1;

    //Due to uint8_t index in LSQSenderState
    assert(LQEntries <= 256);
    assert(SQEntries <= 256);

    loadQueue.resize(LQEntries);
    storeQueue.resize(SQEntries);

    setsNum = params->forwardStructSets;
    waysNum = params->forwardStructWays;
    for (int i = 0; i < setsNum; ++i)
    {
        std::vector<DataForwardEntry> temp;
        temp.resize(waysNum);
        dataForwardStruct.push_back(temp);
    }

    depCheckShift = params->LSQDepCheckShift;
    checkLoads = params->LSQCheckLoads;
    cacheStorePorts = params->cacheStorePorts;
    needsTSO = params->needsTSO;

    resetState();
}


template<class Impl>
void
LSQUnit<Impl>::resetState()
{
    loads = stores = storesToWB = 0;

    loadHead = loadTail = 0;

    storeHead = storeWBIdx = storeTail = 0;

    usedStorePorts = 0;

    retryPkt = NULL;
    memDepViolator = NULL;

    stalled = false;

    cacheBlockMask = ~(cpu->cacheLineSize() - 1);
}

template<class Impl>
std::string
LSQUnit<Impl>::name() const
{
    if (Impl::MaxThreads == 1) {
        return iewStage->name() + ".lsq";
    } else {
        return iewStage->name() + ".lsq.thread" + std::to_string(lsqID);
    }
}

template<class Impl>
void
LSQUnit<Impl>::regStats()
{
    lsqForwLoads
        .name(name() + ".forwLoads")
        .desc("Number of loads that had data forwarded from stores");

    invAddrLoads
        .name(name() + ".invAddrLoads")
        .desc("Number of loads ignored due to an invalid address");

    lsqSquashedLoads
        .name(name() + ".squashedLoads")
        .desc("Number of loads squashed");

    lsqIgnoredResponses
        .name(name() + ".ignoredResponses")
        .desc("Number of memory responses ignored because the instruction is squashed");

    lsqMemOrderViolation
        .name(name() + ".memOrderViolation")
        .desc("Number of memory ordering violations");

    lsqSquashedStores
        .name(name() + ".squashedStores")
        .desc("Number of stores squashed");

    invAddrSwpfs
        .name(name() + ".invAddrSwpfs")
        .desc("Number of software prefetches ignored due to an invalid address");

    lsqBlockedLoads
        .name(name() + ".blockedLoads")
        .desc("Number of blocked loads due to partial load-store forwarding");

    lsqRescheduledLoads
        .name(name() + ".rescheduledLoads")
        .desc("Number of loads that were rescheduled");

    lsqCacheBlocked
        .name(name() + ".cacheBlocked")
        .desc("Number of times an access to memory failed due to the cache being blocked");
}

template<class Impl>
void
LSQUnit<Impl>::setDcachePort(MasterPort *dcache_port)
{
    dcachePort = dcache_port;
}

template<class Impl>
void
LSQUnit<Impl>::clearLQ()
{
    loadQueue.clear();
}

template<class Impl>
void
LSQUnit<Impl>::clearSQ()
{
    storeQueue.clear();
}

template<class Impl>
void
LSQUnit<Impl>::drainSanityCheck() const
{
    for (int i = 0; i < loadQueue.size(); ++i)
        assert(!loadQueue[i]);

    assert(storesToWB == 0);
    assert(!retryPkt);
}

template<class Impl>
void
LSQUnit<Impl>::takeOverFrom()
{
    resetState();
}

template<class Impl>
void
LSQUnit<Impl>::resizeLQ(unsigned size)
{
    unsigned size_plus_sentinel = size + 1;
    assert(size_plus_sentinel >= LQEntries);

    if (size_plus_sentinel > LQEntries) {
        while (size_plus_sentinel > loadQueue.size()) {
            DynInstPtr dummy;
            loadQueue.push_back(dummy);
            LQEntries++;
        }
    } else {
        LQEntries = size_plus_sentinel;
    }

    assert(LQEntries <= 256);
}

template<class Impl>
void
LSQUnit<Impl>::resizeSQ(unsigned size)
{
    unsigned size_plus_sentinel = size + 1;
    if (size_plus_sentinel > SQEntries) {
        while (size_plus_sentinel > storeQueue.size()) {
            SQEntry dummy;
            storeQueue.push_back(dummy);
            SQEntries++;
        }
    } else {
        SQEntries = size_plus_sentinel;
    }

    assert(SQEntries <= 256);
}

template <class Impl>
void
LSQUnit<Impl>::insert(DynInstPtr &inst)
{
    assert(inst->isMemRef());

    assert(inst->isLoad() || inst->isStore());

    if (inst->isLoad()) {
        insertLoad(inst);
    } else {
        insertStore(inst);
    }

    inst->setInLSQ();
}

template <class Impl>
void
LSQUnit<Impl>::insertLoad(DynInstPtr &load_inst)
{
    assert((loadTail + 1) % LQEntries != loadHead);
    assert(loads < LQEntries);

    DPRINTF(LSQUnit, "Inserting load PC %s, idx:%i [sn:%lli]\n",
            load_inst->pcState(), loadTail, load_inst->seqNum);

    load_inst->lqIdx = loadTail;

    if (stores == 0) {
        load_inst->sqIdx = -1;
    } else {
        load_inst->sqIdx = storeTail;
    }

    loadQueue[loadTail] = load_inst;

    incrLdIdx(loadTail);

    ++loads;
}

template <class Impl>
void
LSQUnit<Impl>::insertStore(DynInstPtr &store_inst)
{
    // Make sure it is not full before inserting an instruction.
    assert((storeTail + 1) % SQEntries != storeHead);
    assert(stores < SQEntries);

    DPRINTF(LSQUnit, "Inserting store PC %s, idx:%i [sn:%lli]\n",
            store_inst->pcState(), storeTail, store_inst->seqNum);

    store_inst->sqIdx = storeTail;
    store_inst->lqIdx = loadTail;

    storeQueue[storeTail] = SQEntry(store_inst);

    incrStIdx(storeTail);

    ++stores;
}

template <class Impl>
typename Impl::DynInstPtr
LSQUnit<Impl>::getMemDepViolator()
{
    DynInstPtr temp = memDepViolator;

    memDepViolator = NULL;

    return temp;
}

template <class Impl>
unsigned
LSQUnit<Impl>::numFreeLoadEntries()
{
        //LQ has an extra dummy entry to differentiate
        //empty/full conditions. Subtract 1 from the free entries.
        DPRINTF(LSQUnit, "LQ size: %d, #loads occupied: %d\n", LQEntries, loads);
        return LQEntries - loads - 1;
}

template <class Impl>
unsigned
LSQUnit<Impl>::numFreeStoreEntries()
{
        //SQ has an extra dummy entry to differentiate
        //empty/full conditions. Subtract 1 from the free entries.
        DPRINTF(LSQUnit, "SQ size: %d, #stores occupied: %d\n", SQEntries, stores);
        return SQEntries - stores - 1;

 }

template <class Impl>
void
LSQUnit<Impl>::checkSnoop(PacketPtr pkt)
{
    // Should only ever get invalidations in here
    assert(pkt->isInvalidate());

    int load_idx = loadHead;
    DPRINTF(LSQUnit, "Got snoop for address %#x\n", pkt->getAddr());

    // Only Invalidate packet calls checkSnoop
    assert(pkt->isInvalidate());
    for (int x = 0; x < cpu->numContexts(); x++) {
        ThreadContext *tc = cpu->getContext(x);
        bool no_squash = cpu->thread[x]->noSquashFromTC;
        cpu->thread[x]->noSquashFromTC = true;
        TheISA::handleLockedSnoop(tc, pkt, cacheBlockMask);
        cpu->thread[x]->noSquashFromTC = no_squash;
    }

    Addr invalidate_addr = pkt->getAddr() & cacheBlockMask;

    DynInstPtr ld_inst = loadQueue[load_idx];
    if (ld_inst) {
        Addr load_addr_low = ld_inst->physEffAddrLow & cacheBlockMask;
        Addr load_addr_high = ld_inst->physEffAddrHigh & cacheBlockMask;

        // Check that this snoop didn't just invalidate our lock flag
        if (ld_inst->effAddrValid() && (load_addr_low == invalidate_addr
                                        || load_addr_high == invalidate_addr)
            && ld_inst->memReqFlags & Request::LLSC)
            TheISA::handleLockedSnoopHit(ld_inst.get());
    }

    // If this is the only load in the LSQ we don't care
    if (load_idx == loadTail)
        return;

    incrLdIdx(load_idx);

    bool force_squash = false;

    while (load_idx != loadTail) {
        DynInstPtr ld_inst = loadQueue[load_idx];

        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered()) {
            incrLdIdx(load_idx);
            continue;
        }

        Addr load_addr_low = ld_inst->physEffAddrLow & cacheBlockMask;
        Addr load_addr_high = ld_inst->physEffAddrHigh & cacheBlockMask;

        DPRINTF(LSQUnit, "-- inst [sn:%lli] load_addr: %#x to pktAddr:%#x\n",
                    ld_inst->seqNum, load_addr_low, invalidate_addr);

        if ((load_addr_low == invalidate_addr
             || load_addr_high == invalidate_addr) || force_squash) {
            if (needsTSO) {
                // If we have a TSO system, as all loads must be ordered with
                // all other loads, this load as well as *all* subsequent loads
                // need to be squashed to prevent possible load reordering.
                force_squash = true;
            }
            if (ld_inst->possibleLoadViolation() || force_squash) {
                DPRINTF(LSQUnit, "Conflicting load at addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Mark the load for re-execution
                ld_inst->fault = std::make_shared<ReExec>();
            } else {
                DPRINTF(LSQUnit, "HitExternal Snoop for addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Make sure that we don't lose a snoop hitting a LOCKED
                // address since the LOCK* flags don't get updated until
                // commit.
                if (ld_inst->memReqFlags & Request::LLSC)
                    TheISA::handleLockedSnoopHit(ld_inst.get());

                // If a older load checks this and it's true
                // then we might have missed the snoop
                // in which case we need to invalidate to be sure
                ld_inst->hitExternalSnoop(true);
            }
        }
        incrLdIdx(load_idx);
    }
    return;
}

template <class Impl>
Fault
LSQUnit<Impl>::checkViolations(int load_idx, DynInstPtr &inst)
{
    Addr inst_eff_addr1 = inst->effAddr >> depCheckShift;
    Addr inst_eff_addr2 = (inst->effAddr + inst->effSize - 1) >> depCheckShift;

    /** @todo in theory you only need to check an instruction that has executed
     * however, there isn't a good way in the pipeline at the moment to check
     * all instructions that will execute before the store writes back. Thus,
     * like the implementation that came before it, we're overly conservative.
     */
    while (load_idx != loadTail) {
        DynInstPtr ld_inst = loadQueue[load_idx];
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered()) {
            incrLdIdx(load_idx);
            continue;
        }

        Addr ld_eff_addr1 = ld_inst->effAddr >> depCheckShift;
        Addr ld_eff_addr2 =
            (ld_inst->effAddr + ld_inst->effSize - 1) >> depCheckShift;

        if (inst_eff_addr2 >= ld_eff_addr1 && inst_eff_addr1 <= ld_eff_addr2) {
            if (inst->isLoad()) {
                // If this load is to the same block as an external snoop
                // invalidate that we've observed then the load needs to be
                // squashed as it could have newer data
                if (ld_inst->hitExternalSnoop()) {
                    if (!memDepViolator ||
                            ld_inst->seqNum < memDepViolator->seqNum) {
                        DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] "
                                "and [sn:%lli] at address %#x\n",
                                inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                        memDepViolator = ld_inst;

                        ++lsqMemOrderViolation;

                        return std::make_shared<GenericISA::M5PanicFault>(
                            "Detected fault with inst [sn:%lli] and "
                            "[sn:%lli] at address %#x\n",
                            inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                    }
                }

                // Otherwise, mark the load has a possible load violation
                // and if we see a snoop before it's commited, we need to squash
                ld_inst->possibleLoadViolation(true);
                DPRINTF(LSQUnit, "Found possible load violation at addr: %#x"
                        " between instructions [sn:%lli] and [sn:%lli]\n",
                        inst_eff_addr1, inst->seqNum, ld_inst->seqNum);
            } else {
                // A load/store incorrectly passed this store.
                // Check if we already have a violator, or if it's newer
                // squash and refetch.
                if (memDepViolator && ld_inst->seqNum > memDepViolator->seqNum)
                    break;

                DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] and "
                        "[sn:%lli] at address %#x\n",
                        inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                memDepViolator = ld_inst;

                ++lsqMemOrderViolation;

                return std::make_shared<GenericISA::M5PanicFault>(
                    "Detected fault with "
                    "inst [sn:%lli] and [sn:%lli] at address %#x\n",
                    inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
            }
        }

        incrLdIdx(load_idx);
    }
    return NoFault;
}


template <class Impl>
Fault
LSQUnit<Impl>::ReexecuteLoad(DynInstPtr &inst)
{
    using namespace TheISA;
    // Execute a specific load.
    Fault load_fault = NoFault;

    DPRINTF(LSQUnit, "Reexecuting load PC %s, [sn:%lli]\n",
            inst->pcState(), inst->seqNum);

    assert(!inst->isSquashed());

    // todo:copy olddata

    load_fault = inst->initiateAcc();

    if (inst->isTranslationDelayed() &&
        load_fault == NoFault)
        return load_fault;
   // if (load_fault!=NoFault)
   // std::cout << "debug: reexecute fault: " << load_fault->name() << std::endl;
    return load_fault;
}

template <class Impl>
Fault
LSQUnit<Impl>::executeLoad(DynInstPtr &inst)
{
    std::cout << std::endl;
    std::cout << "lsq exe ";inst->dump();
    using namespace TheISA;
    // Execute a specific load.
    Fault load_fault = NoFault;

    DPRINTF(LSQUnit, "Executing load PC %s, [sn:%lli]\n",
            inst->pcState(), inst->seqNum);

    assert(!inst->isSquashed());

    // add data forward mechisim
    // todo@ may need to change
    if (inst->firstEnterExe //&& (!inst->isSpecialInst)
        && dataForward(inst))
    {
        inst->isForward = true;
        // todo@ write value to proper reg
        std::cout << "asw: forward success ssn is: " << inst->forwardSSN;inst->dump();
        // Need to get physical address and effSize for svw filter
        if (inst->isConditionalInst) {
             inst->firstEnterExe = false;
             cpu->iew.instQueue.deferMemInst(inst);
             return NoFault;
        }
        std::cout <<"forward test time: " << curTick() << " inst SN: " << inst->seqNum;inst->dump();
        inst->onlyTLBTranslate = true;
        load_fault = inst->initiateAcc();
        //inst->onlyTLBTranslate = false;
        uint64_t val = 0;
        for (int i=0; i<inst->effSize; ++i)
        {
             //uint64_t temp = inst->forwardData[i];
             uint64_t temp = inst->memData[i];
             std::cout << "forward data: " << temp << " ";
             temp = temp << (8 * i);
             val = val | temp;
        }
        if (!inst->isUnsigned && inst->memData[inst->effSize - 1] >= 128 )
        {
             uint64_t temp = 255;
             temp = temp << (8 * inst->effSize);
             for (int i=inst->effSize; i<8; i++)
             {
                val = val | temp;
                temp = temp << 8;
             }
        }
        std::cout << "asw: forward val is: " << val << std::endl;
        if (inst->isFloating())
        {
            inst->setFloatRegOperandBits(
                  inst->staticInst.get(),0,val);
        }
        else
        {
            inst->setIntRegOperand(inst->staticInst.get(),0,val);
        }
    }
    else {
        //inst->isForward = true;
        load_fault = inst->initiateAcc();
        //if (inst->firstEnterExe)
        //inst->forwardSSN = cpu->SVWFilter.getSSN(inst);
        inst->isForward = false;
        inst->forwardSSN = cpu->retireSSN;
        std::cout << "asw: forward failed ssn is: " << inst->forwardSSN;inst->dump();
    }

    inst->firstEnterExe = false;
    if (inst->isTranslationDelayed() &&
        load_fault == NoFault)
        return load_fault;

    // If the instruction faulted or predicated false, then we need to send it
    // along to commit without the instruction completing.
    if (load_fault != NoFault || !inst->readPredicate()) {
        // Send this instruction to commit, also make sure iew stage
        // realizes there is activity.  Mark it as executed unless it
        // is a strictly ordered load that needs to hit the head of
        // commit.
        if (!inst->readPredicate())
            inst->forwardOldRegs();
        DPRINTF(LSQUnit, "Load [sn:%lli] not executed from %s\n",
                inst->seqNum,
                (load_fault != NoFault ? "fault" : "predication"));
        if (!(inst->hasRequest() && inst->strictlyOrdered()) ||
            inst->isAtCommit()) {
            inst->setExecuted();
        }
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
    } else {
        //inst->forwardSSN = cpu->SVWFilter.getSSN(inst);
        assert(inst->effAddrValid());
        int load_idx = inst->lqIdx;
        incrLdIdx(load_idx);

        if (inst->isForward)
        {
            inst->onlyTLBTranslate = false;

            inst->setExecuted();
            // Need to insert instruction into queue to commit
            iewStage->instToCommit(inst);

            iewStage->activityThisCycle();

            // see if this load changed the PC
            iewStage->checkMisprediction(inst);

        }

        if (checkLoads)
            return checkViolations(load_idx, inst);
    }
    if (load_fault!=NoFault)
      std::cout << "debug: exe load fault: " << load_fault->name() << std::endl;
    else
      std::cout << "debug: exe load no fault" << std::endl;

    return load_fault;
}

template <class Impl>
Fault
LSQUnit<Impl>::executeStore(DynInstPtr &store_inst)
{
    std::cout << std::endl;
    std::cout << "lsq exe ";store_inst->dump();
    using namespace TheISA;
    // Make sure that a store exists.
    assert(stores != 0);

    int store_idx = store_inst->sqIdx;

    DPRINTF(LSQUnit, "Executing store PC %s [sn:%lli]\n",
            store_inst->pcState(), store_inst->seqNum);

    assert(!store_inst->isSquashed());
    //updateForwardEntry(store_inst);

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    int load_idx = store_inst->lqIdx;

    Fault store_fault = store_inst->initiateAcc();
    if (store_inst->firstEnterExe)
    {
         //if (store_inst->canUpdateASW)
         updateForwardEntry(store_inst);
         store_inst->firstEnterExe = false;
    }

    if (store_inst->isTranslationDelayed() &&
        store_fault == NoFault)
        return store_fault;

    if (!store_inst->readPredicate()) {
        DPRINTF(LSQUnit, "Store [sn:%lli] not executed from predication\n",
                store_inst->seqNum);
        store_inst->forwardOldRegs();
        return store_fault;
    }

    if (storeQueue[store_idx].size == 0) {
        DPRINTF(LSQUnit,"Fault on Store PC %s, [sn:%lli], Size = 0\n",
                store_inst->pcState(), store_inst->seqNum);

        return store_fault;
    }
    std::cout << "asw update: effAddr is: " << store_inst->effAddr;store_inst->dump();
    // todo@ may need to identify where to place this op
    //if (store_inst->canUpdateASW)
    //    updateForwardEntry(store_inst);

    assert(store_fault == NoFault);

    if (store_inst->isStoreConditional()) {
        // Store conditionals need to set themselves as able to
        // writeback if we haven't had a fault by here.
        storeQueue[store_idx].canWB = true;

        ++storesToWB;
    }

    return checkViolations(load_idx, store_inst);

}

template <class Impl>
void
LSQUnit<Impl>::commitLoad()
{
    assert(loadQueue[loadHead]);

    DPRINTF(LSQUnit, "Committing head load instruction, PC %s\n",
            loadQueue[loadHead]->pcState());

    loadQueue[loadHead] = NULL;

    incrLdIdx(loadHead);

    --loads;
}

template <class Impl>
void
LSQUnit<Impl>::commitLoads(InstSeqNum &youngest_inst)
{
    assert(loads == 0 || loadQueue[loadHead]);

    while (loads != 0 && loadQueue[loadHead]->seqNum <= youngest_inst) {
        commitLoad();
    }
}

template <class Impl>
void
LSQUnit<Impl>::commitStores(InstSeqNum &youngest_inst)
{
    assert(stores == 0 || storeQueue[storeHead].inst);

    int store_idx = storeHead;

    while (store_idx != storeTail) {
        assert(storeQueue[store_idx].inst);
        // Mark any stores that are now committed and have not yet
        // been marked as able to write back.
        if (!storeQueue[store_idx].canWB) {
            if (storeQueue[store_idx].inst->seqNum > youngest_inst) {
                break;
            }
            DPRINTF(LSQUnit, "Marking store as able to write back, PC "
                    "%s [sn:%lli]\n",
                    storeQueue[store_idx].inst->pcState(),
                    storeQueue[store_idx].inst->seqNum);

            storeQueue[store_idx].canWB = true;

            ++storesToWB;
        }

        incrStIdx(store_idx);
    }
}

template <class Impl>
void
LSQUnit<Impl>::writebackPendingStore()
{
    if (hasPendingPkt) {
        assert(pendingPkt != NULL);

        // If the cache is blocked, this will store the packet for retry.
        if (sendStore(pendingPkt)) {
            storePostSend(pendingPkt);
        }
        pendingPkt = NULL;
        hasPendingPkt = false;
    }
}

template <class Impl>
void
LSQUnit<Impl>::writebackStores()
{
    // First writeback the second packet from any split store that didn't
    // complete last cycle because there weren't enough cache ports available.
    if (TheISA::HasUnalignedMemAcc) {
        writebackPendingStore();
    }

    while (storesToWB > 0 &&
           storeWBIdx != storeTail &&
           storeQueue[storeWBIdx].inst &&
           storeQueue[storeWBIdx].canWB &&
           ((!needsTSO) || (!storeInFlight)) &&
           usedStorePorts < cacheStorePorts) {

        if (isStoreBlocked) {
            DPRINTF(LSQUnit, "Unable to write back any more stores, cache"
                    " is blocked!\n");
            break;
        }

        // Store didn't write any data so no need to write it back to
        // memory.
        if (storeQueue[storeWBIdx].size == 0) {
            completeStore(storeWBIdx);

            incrStIdx(storeWBIdx);

            continue;
        }

        ++usedStorePorts;

        if (storeQueue[storeWBIdx].inst->isDataPrefetch()) {
            incrStIdx(storeWBIdx);

            continue;
        }

        assert(storeQueue[storeWBIdx].req);
        assert(!storeQueue[storeWBIdx].committed);

        if (TheISA::HasUnalignedMemAcc && storeQueue[storeWBIdx].isSplit) {
            assert(storeQueue[storeWBIdx].sreqLow);
            assert(storeQueue[storeWBIdx].sreqHigh);
        }

        DynInstPtr inst = storeQueue[storeWBIdx].inst;

        RequestPtr &req = storeQueue[storeWBIdx].req;
        const RequestPtr &sreqLow = storeQueue[storeWBIdx].sreqLow;
        const RequestPtr &sreqHigh = storeQueue[storeWBIdx].sreqHigh;

        storeQueue[storeWBIdx].committed = true;

        assert(!inst->memData);
        inst->memData = new uint8_t[req->getSize()];

        if (storeQueue[storeWBIdx].isAllZeros)
            memset(inst->memData, 0, req->getSize());
        else
            memcpy(inst->memData, storeQueue[storeWBIdx].data, req->getSize());

        PacketPtr data_pkt;
        PacketPtr snd_data_pkt = NULL;

        LSQSenderState *state = new LSQSenderState;
        state->isLoad = false;
        state->idx = storeWBIdx;
        state->inst = inst;

        if (!TheISA::HasUnalignedMemAcc || !storeQueue[storeWBIdx].isSplit) {

            // Build a single data packet if the store isn't split.
            data_pkt = Packet::createWrite(req);
            data_pkt->dataStatic(inst->memData);
            data_pkt->senderState = state;
        } else {
            // Create two packets if the store is split in two.
            data_pkt = Packet::createWrite(sreqLow);
            snd_data_pkt = Packet::createWrite(sreqHigh);

            data_pkt->dataStatic(inst->memData);
            snd_data_pkt->dataStatic(inst->memData + sreqLow->getSize());

            data_pkt->senderState = state;
            snd_data_pkt->senderState = state;

            state->isSplit = true;
            state->outstanding = 2;

            // Can delete the main request now.
            req = sreqLow;
        }

        DPRINTF(LSQUnit, "D-Cache: Writing back store idx:%i PC:%s "
                "to Addr:%#x, data:%#x [sn:%lli]\n",
                storeWBIdx, inst->pcState(),
                req->getPaddr(), (int)*(inst->memData),
                inst->seqNum);

        // @todo: Remove this SC hack once the memory system handles it.
        if (inst->isStoreConditional()) {
            assert(!storeQueue[storeWBIdx].isSplit);
            // Disable recording the result temporarily.  Writing to
            // misc regs normally updates the result, but this is not
            // the desired behavior when handling store conditionals.
            inst->recordResult(false);
            bool success = TheISA::handleLockedWrite(inst.get(), req, cacheBlockMask);
            inst->recordResult(true);

            if (!success) {
                // Instantly complete this store.
                DPRINTF(LSQUnit, "Store conditional [sn:%lli] failed.  "
                        "Instantly completing it.\n",
                        inst->seqNum);
                WritebackEvent *wb = new WritebackEvent(inst, data_pkt, this);
                cpu->schedule(wb, curTick() + 1);
                completeStore(storeWBIdx);
                incrStIdx(storeWBIdx);
                continue;
            }
        } else {
            // Non-store conditionals do not need a writeback.
            state->noWB = true;
        }

        bool split =
            TheISA::HasUnalignedMemAcc && storeQueue[storeWBIdx].isSplit;

        ThreadContext *thread = cpu->tcBase(lsqID);

        if (req->isMmappedIpr()) {
            assert(!inst->isStoreConditional());
            TheISA::handleIprWrite(thread, data_pkt);
            delete data_pkt;
            if (split) {
                assert(snd_data_pkt->req->isMmappedIpr());
                TheISA::handleIprWrite(thread, snd_data_pkt);
                delete snd_data_pkt;
            }
            delete state;
            completeStore(storeWBIdx);
            incrStIdx(storeWBIdx);
        } else if (!sendStore(data_pkt)) {
            DPRINTF(IEW, "D-Cache became blocked when writing [sn:%lli], will"
                    "retry later\n",
                    inst->seqNum);

            // Need to store the second packet, if split.
            if (split) {
                state->pktToSend = true;
                state->pendingPacket = snd_data_pkt;
            }
        } else {

            // If split, try to send the second packet too
            if (split) {
                assert(snd_data_pkt);

                // Ensure there are enough ports to use.
                if (usedStorePorts < cacheStorePorts) {
                    ++usedStorePorts;
                    if (sendStore(snd_data_pkt)) {
                        storePostSend(snd_data_pkt);
                    } else {
                        DPRINTF(IEW, "D-Cache became blocked when writing"
                                " [sn:%lli] second packet, will retry later\n",
                                inst->seqNum);
                    }
                } else {

                    // Store the packet for when there's free ports.
                    assert(pendingPkt == NULL);
                    pendingPkt = snd_data_pkt;
                    hasPendingPkt = true;
                }
            } else {

                // Not a split store.
                storePostSend(data_pkt);
            }
        }
    }

    // Not sure this should set it to 0.
    usedStorePorts = 0;

    assert(stores >= 0 && storesToWB >= 0);
}

/*template <class Impl>
void
LSQUnit<Impl>::removeMSHR(InstSeqNum seqNum)
{
    list<InstSeqNum>::iterator mshr_it = find(mshrSeqNums.begin(),
                                              mshrSeqNums.end(),
                                              seqNum);

    if (mshr_it != mshrSeqNums.end()) {
        mshrSeqNums.erase(mshr_it);
        DPRINTF(LSQUnit, "Removing MSHR. count = %i\n",mshrSeqNums.size());
    }
}*/

// todo@ implement the two funtions below.
template<class Impl>
void
LSQUnit<Impl>::updateForwardEntry(const DynInstPtr& store_inst)
{
    // todo@ need to indentify the type of base and offset
    // depCheckShift is the offset in block
    uint64_t base = store_inst->readIntRegOperand(
                    store_inst->staticInst.get(),0);
    uint64_t offset = store_inst->staticInst->getOffset();
    uint64_t hash = (base + offset) >> depCheckShift;
    // used for bitEnable, this is start place where bit enable
    uint64_t blockOffset = (base + offset) % (1 << depCheckShift);
    uint64_t index = hash % setsNum;
    uint64_t oldestSsnWay = 0;
    StoreSeqNum ssn = store_inst->SSN;
    StoreSeqNum oldestSsn = ssn;
    for (unsigned i = 0; i < waysNum; ++i)
    {
        DataForwardEntry& entry = dataForwardStruct[index][i];
        if (!entry.valid)
        {
            oldestSsnWay = i;
            break;
        }
        if (ssn < entry.ssn)
            continue;
        if (oldestSsn > entry.ssn)
        {
            oldestSsn = entry.ssn;
            oldestSsnWay = i;
        }
    }
    DataForwardEntry& entry = dataForwardStruct[index][oldestSsnWay];
    entry.valid = true;
    // bitEnable need to determind by offset in block, bitEnable from low to
    // to high represent byte enable from low index to high index in dynamic
    // vector (data);
    uint8_t mask = 1 << blockOffset;
    entry.bitEnable = 0;
    for (uint64_t i = 0;i < store_inst->effSize; i++)
    {
         entry.bitEnable = entry.bitEnable | mask;
         mask = mask << 1;
         // std::cout << "asw: mask: " << (uint64_t)mask << " bitEnable: " << (uint64_t)entry.bitEnable << std::endl;
    }
    store_inst->bitEnable = entry.bitEnable;
    entry.ssn = ssn;
    entry.tag = hash / setsNum;
    std::cout << "asw update: index: "<< index << " tag: " << entry.tag
              << " base: " << base << " offset: " << offset
              << " blockOffset: " << blockOffset << " size: " << store_inst->effSize
              << " bitEnable: " << (uint64_t)entry.bitEnable << " ssn: " << ssn;store_inst->dump();
    // only set the proper byte in data;
    if (entry.data == NULL)
        entry.data = new uint8_t[16];
    //std::cout << "debug: store inst effSize is: " << store_inst->effSize << std::endl;
    memcpy(&entry.data[blockOffset], store_inst->storeData,
           store_inst->effSize);
   for (int i=0;i<store_inst->effSize;i++)
     std::cout << (uint64_t)entry.data[blockOffset+i] << " ";
}

template<class Impl>
void
LSQUnit<Impl>::squashForwardEntry(const DynInstPtr& store_inst)
{
    uint64_t base = store_inst->readIntRegOperand(
                    store_inst->staticInst.get(),0);
    uint64_t offset = store_inst->staticInst->getOffset();
    uint64_t hash = (base + offset) >> depCheckShift;
    uint64_t index = hash % setsNum;
    for (unsigned i = 0; i < waysNum; ++i)
    {
        DataForwardEntry& entry = dataForwardStruct[index][i];
        if (!entry.valid)
        {
            continue;
        }
        if (store_inst->SSN == entry.ssn)
        {
            entry.valid = false;
            return;
        }
    }
}

template<class Impl>
void
LSQUnit<Impl>::squashAllForwardEntry ()
{
   for (auto &entrys:dataForwardStruct)
   {
      for (auto &i:entrys)
      {
         i.valid = false;
      }
   }
}

template<class Impl>
bool
LSQUnit<Impl>::dataForward(DynInstPtr& load_inst)
{
    /*std::cout<<"debug: SN:"<<load_inst->seqNum
    <<" SSN:"<<load_inst->SSN<<" ";load_inst->dump();*/
    //std::cout << "debug: Enter data Forward functio/n!!\n";
    if (load_inst->numDestRegs() != 1)
        return false;
   /* std::cout << "debug: setsNum is: " << setsNum
              << "depCheckShift is: " << depCheckShift << std::endl;*/
    //uint64_t indexMask = setsNum - 1;
    uint64_t base = load_inst->readIntRegOperand(
                    load_inst->staticInst.get(),0);
    uint64_t offset = load_inst->staticInst->getOffset();
    //std::cout << "debug: base: " << base << " offset: " << offset << std::endl;
    uint64_t hash = (base + offset) >> depCheckShift;
    uint64_t index = hash % setsNum;
    uint64_t blockOffset = (base + offset) % (1 << depCheckShift);
    uint64_t youngestSsnWay = 0;
    uint64_t youngestHitSsn = 0;
    bool canForward = false;
    uint64_t tag = hash / setsNum;
    uint8_t mask = 1 << blockOffset;
    load_inst->bitEnable = 0;
    for (uint64_t i = 0;i < load_inst->effSize; i++)
    {
         load_inst->bitEnable = load_inst->bitEnable | mask;
         mask = mask << 1;
         // std::cout << "asw: mask: " << (uint64_t)mask << " bitEnable: " << (uint64_t)entry.bitEnable << std::endl;
    }
    for (unsigned i = 0; i < waysNum; ++i)
    {
        DataForwardEntry& entry = dataForwardStruct[index][i];
        if (!entry.valid)
            continue;
        if (entry.tag != tag)
            continue;
        bool bitFit = true;
        uint8_t mask = 1 << blockOffset;
        for (uint64_t i=0;i<load_inst->effSize;i++)
        {
           if ((mask & entry.bitEnable) == 0)
           {
               bitFit = false;
               break;
           }
           mask = mask << 1;
        }
        if (!bitFit)
            continue;
        canForward = true;
        if (youngestHitSsn <= entry.ssn)
        {
            youngestHitSsn = entry.ssn;
            youngestSsnWay = i;
        }
    }
    if (canForward)
    {
        DataForwardEntry& entry = dataForwardStruct[index][youngestSsnWay];
        std::cout << "asw forward: index: "<< index << " tag: " << tag
                  << " base: " << base << " offset: " << offset
                  << " blockOffset: " << blockOffset << " size: " << load_inst->effSize
                  << " bitEnable: " << (uint64_t)entry.bitEnable << " ssn: " << entry.ssn;load_inst->dump();
        load_inst->forwardSSN = youngestHitSsn;
        uint64_t size = load_inst->effSize;
        if (!load_inst->memData)
            load_inst->memData = new uint8_t[size];
        memcpy(load_inst->memData, &entry.data[blockOffset], size);
        std::cout << "asw size: " <<size << std::endl;
        std::cout << " entry data: ";
        for (int i=0;i<size;i++)
        {
            std::cout << (uint64_t)entry.data[blockOffset+i] << " ";
        }
        std::cout << "ssn : " << entry.ssn;
        load_inst->dump();
        return true;
    }
    std::cout << "asw forward failed: index: "<< index << " tag: " << tag
              << " base: " << base << " offset: " << offset
              << " blockOffset: " << blockOffset << " size: " << load_inst->effSize;load_inst->dump();
    return false;
}

template <class Impl>
void
LSQUnit<Impl>::squash(const InstSeqNum &squashed_num)
{
    DPRINTF(LSQUnit, "Squashing until [sn:%lli]!"
            "(Loads:%i Stores:%i)\n", squashed_num, loads, stores);

    int load_idx = loadTail;
    decrLdIdx(load_idx);

    while (loads != 0 && loadQueue[load_idx]->seqNum > squashed_num) {
        DPRINTF(LSQUnit,"Load Instruction PC %s squashed, "
                "[sn:%lli]\n",
                loadQueue[load_idx]->pcState(),
                loadQueue[load_idx]->seqNum);

        if (isStalled() && load_idx == stallingLoadIdx) {
            stalled = false;
            stallingStoreIsn = 0;
            stallingLoadIdx = 0;
        }

        // Clear the smart pointer to make sure it is decremented.
        loadQueue[load_idx]->setSquashed();
        loadQueue[load_idx] = NULL;
        --loads;

        // Inefficient!
        loadTail = load_idx;

        decrLdIdx(load_idx);
        ++lsqSquashedLoads;
    }

    if (memDepViolator && squashed_num < memDepViolator->seqNum) {
        memDepViolator = NULL;
    }

    int store_idx = storeTail;
    decrStIdx(store_idx);

    while (stores != 0 &&
           storeQueue[store_idx].inst->seqNum > squashed_num) {
        // Instructions marked as can WB are already committed.
        if (storeQueue[store_idx].canWB) {
            break;
        }

        DPRINTF(LSQUnit,"Store Instruction PC %s squashed, "
                "idx:%i [sn:%lli]\n",
                storeQueue[store_idx].inst->pcState(),
                store_idx, storeQueue[store_idx].inst->seqNum);

        // I don't think this can happen.  It should have been cleared
        // by the stalling load.
        if (isStalled() &&
            storeQueue[store_idx].inst->seqNum == stallingStoreIsn) {
            panic("Is stalled should have been cleared by stalling load!\n");
            stalled = false;
            stallingStoreIsn = 0;
        }

        // Clear the smart pointer to make sure it is decremented.

        // clear the entry in asw where squashed store inst updated
        squashForwardEntry(storeQueue[store_idx].inst);
        storeQueue[store_idx].inst->setSquashed();
        storeQueue[store_idx].inst = NULL;
        storeQueue[store_idx].canWB = 0;

        // Must delete request now that it wasn't handed off to
        // memory.  This is quite ugly.  @todo: Figure out the proper
        // place to really handle request deletes.
        storeQueue[store_idx].req.reset();
        if (TheISA::HasUnalignedMemAcc && storeQueue[store_idx].isSplit) {
            storeQueue[store_idx].sreqLow.reset();
            storeQueue[store_idx].sreqHigh.reset();
        }

        --stores;

        // Inefficient!
        storeTail = store_idx;

        decrStIdx(store_idx);
        ++lsqSquashedStores;
    }
}

template <class Impl>
void
LSQUnit<Impl>::storePostSend(PacketPtr pkt)
{
    if (isStalled() &&
        storeQueue[storeWBIdx].inst->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%i\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx]);
    }

    if (!storeQueue[storeWBIdx].inst->isStoreConditional()) {
        // The store is basically completed at this time. This
        // only works so long as the checker doesn't try to
        // verify the value in memory for stores.
        storeQueue[storeWBIdx].inst->setCompleted();

        if (cpu->checker) {
            cpu->checker->verify(storeQueue[storeWBIdx].inst);
        }
    }

    if (needsTSO) {
        storeInFlight = true;
    }

    incrStIdx(storeWBIdx);
}

template <class Impl>
void
LSQUnit<Impl>::writeback(DynInstPtr &inst, PacketPtr pkt)
{
    iewStage->wakeCPU();

    // Squashed instructions do not need to complete their access.
    if (inst->isSquashed()) {
        assert(!inst->isStore());
        ++lsqIgnoredResponses;
        return;
    }

    if (!inst->isExecuted()) {
        inst->setExecuted();
        if (inst->fault == NoFault) {
            // Complete access to copy data to proper place.
            inst->completeAcc(pkt);
            // place the forward data to proper place when the data forward
            // failed.
            /*if (!inst->forwardData)
            {
                 inst->forwardData = new uint8_t[inst->effSize];
                 memcpy(inst->forwardData, inst->memData, inst->effSize);
            }*/
        } else {
            // If the instruction has an outstanding fault, we cannot complete
            // the access as this discards the current fault.

            // If we have an outstanding fault, the fault should only be of
            // type ReExec.
            assert(dynamic_cast<ReExec*>(inst->fault.get()) != nullptr);

            DPRINTF(LSQUnit, "Not completing instruction [sn:%lli] access "
                    "due to pending fault.\n", inst->seqNum);
        }
    }


    if (inst->isReexecuting()) {
        //printf("reex finshed-----------\n");
        //inst->dump();
        inst->completeAcc(pkt);
        //if (inst->reexecute_memData!=nullptr)
        //    delete [] inst->reexecute_memData;
        if (inst->isSquashDueToReexecute()){
          iewStage->squashDueToMemOrder(inst,inst->threadNumber);
          return;
        }
        inst->setReexecuted();
        inst->setCanCommit();
        iewStage->activityThisCycle();

        if (inst->reexecute_memData!=nullptr)
            delete [] inst->reexecute_memData;
        return ;
    }

    // Need to insert instruction into queue to commit
    iewStage->instToCommit(inst);

    iewStage->activityThisCycle();

    // see if this load changed the PC
    iewStage->checkMisprediction(inst);
}

template <class Impl>
void

LSQUnit<Impl>::completeStore(int store_idx)
{
    assert(storeQueue[store_idx].inst);
    storeQueue[store_idx].completed = true;
    --storesToWB;
    // A bit conservative because a store completion may not free up entries,
    // but hopefully avoids two store completions in one cycle from making
    // the CPU tick twice.
    cpu->wakeCPU();
    cpu->activityThisCycle();

    if (store_idx == storeHead) {
        do {
            // update retireSSN & svw
            StoreSeqNum SSN = storeQueue[storeHead].inst->SSN;
            cpu->setRetireSSN(SSN);
            incrStIdx(storeHead);

            --stores;
        } while (storeQueue[storeHead].completed &&
                 storeHead != storeTail);

        iewStage->updateLSQNextCycle = true;
    }

    DPRINTF(LSQUnit, "Completing store [sn:%lli], idx:%i, store head "
            "idx:%i\n",
            storeQueue[store_idx].inst->seqNum, store_idx, storeHead);

#if TRACING_ON
    if (DTRACE(O3PipeView)) {
        storeQueue[store_idx].inst->storeTick =
            curTick() - storeQueue[store_idx].inst->fetchTick;
    }
#endif
    if (isStalled() &&
        storeQueue[store_idx].inst->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%i\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx]);
    }

    storeQueue[store_idx].inst->setCompleted();

    if (needsTSO) {
        storeInFlight = false;
    }

    // Tell the checker we've completed this instruction.  Some stores
    // may get reported twice to the checker, but the checker can
    // handle that case.

    // Store conditionals cannot be sent to the checker yet, they have
    // to update the misc registers first which should take place
    // when they commit
    if (cpu->checker && !storeQueue[store_idx].inst->isStoreConditional()) {
        cpu->checker->verify(storeQueue[store_idx].inst);
    }
}

template <class Impl>
bool
LSQUnit<Impl>::sendStore(PacketPtr data_pkt)
{
    if (!dcachePort->sendTimingReq(data_pkt)) {
        // Need to handle becoming blocked on a store.
        isStoreBlocked = true;
        ++lsqCacheBlocked;
        assert(retryPkt == NULL);
        retryPkt = data_pkt;
        return false;
    }
    return true;
}

template <class Impl>
void
LSQUnit<Impl>::recvRetry()
{
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Receiving retry: store blocked\n");
        assert(retryPkt != NULL);

        LSQSenderState *state =
            dynamic_cast<LSQSenderState *>(retryPkt->senderState);
        if (dcachePort->sendTimingReq(retryPkt)) {
            // Don't finish the store unless this is the last packet.
            if (!TheISA::HasUnalignedMemAcc || !state->pktToSend ||
                    state->pendingPacket == retryPkt) {
                state->pktToSend = false;
                storePostSend(retryPkt);
            }
            retryPkt = NULL;
            isStoreBlocked = false;

            // Send any outstanding packet.
            if (TheISA::HasUnalignedMemAcc && state->pktToSend) {
                assert(state->pendingPacket);
                if (sendStore(state->pendingPacket)) {
                    storePostSend(state->pendingPacket);
                }
            }
        } else {
            // Still blocked!
            ++lsqCacheBlocked;
        }
    }
}

template <class Impl>
inline void
LSQUnit<Impl>::incrStIdx(int &store_idx) const
{
    if (++store_idx >= SQEntries)
        store_idx = 0;
}

template <class Impl>
inline void
LSQUnit<Impl>::decrStIdx(int &store_idx) const
{
    if (--store_idx < 0)
        store_idx += SQEntries;
}

template <class Impl>
inline void
LSQUnit<Impl>::incrLdIdx(int &load_idx) const
{
    if (++load_idx >= LQEntries)
        load_idx = 0;
}

template <class Impl>
inline void
LSQUnit<Impl>::decrLdIdx(int &load_idx) const
{
    if (--load_idx < 0)
        load_idx += LQEntries;
}

template <class Impl>
void
LSQUnit<Impl>::dumpInsts() const
{
    cprintf("Load store queue: Dumping instructions.\n");
    cprintf("Load queue size: %i\n", loads);
    cprintf("Load queue: ");

    int load_idx = loadHead;

    while (load_idx != loadTail && loadQueue[load_idx]) {
        const DynInstPtr &inst(loadQueue[load_idx]);
        cprintf("%s.[sn:%i] ", inst->pcState(), inst->seqNum);

        incrLdIdx(load_idx);
    }
    cprintf("\n");

    cprintf("Store queue size: %i\n", stores);
    cprintf("Store queue: ");

    int store_idx = storeHead;

    while (store_idx != storeTail && storeQueue[store_idx].inst) {
        const DynInstPtr &inst(storeQueue[store_idx].inst);
        cprintf("%s.[sn:%i] ", inst->pcState(), inst->seqNum);

        incrStIdx(store_idx);
    }

    cprintf("\n");
}

#endif//__CPU_O3_LSQ_UNIT_IMPL_HH__
