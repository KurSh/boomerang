/*
 * Copyright (C) 1998-2001, The University of Queensland
 * Copyright (C) 2002, Trent Waddington
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/*==============================================================================
 * FILE:       machine/pentium/frontendsrc.cc
 * OVERVIEW:   This file contains routines to manage the decoding of pentium
 *             instructions and the instantiation to RTLs. These functions
 *             replace frontend.cc for decoding pentium instructions.
 *============================================================================*/

/*
 * $Revision$
 * 21 Oct 98 - Mike: converted from frontsparc.cc
 * 21 May 02 - Mike: Mods for boomerang
 * 27 Nov 02 - Mike: Fixed a bug in the floating point fixup code, which was
 *                  screwing up registers in flag calls
 * 16 Apr 03 - Mike: processFloatCode accepts test 0x45 where and 0x45 expected
*/

#include <assert.h>
#if defined(_MSC_VER) && _MSC_VER <= 1200
#pragma warning(disable:4786)
#endif

#include <sstream>
#include "types.h"
#include "BinaryFile.h"
#include "frontend.h"
#include "pentiumfrontend.h"
#include "rtl.h"
#include "decoder.h"        // prototype for decodeInstruction()
#include "pentiumdecoder.h"
#include "register.h"
#include "type.h"
#include "cfg.h"
#include "exp.h"
#include "proc.h"
#include "prog.h"           // For findProc()
#include "BinaryFile.h"     // For SymbolByAddress()

/*==============================================================================
 * Forward declarations.
 *============================================================================*/

#define FSW 40              // Numeric registers
#define AH 12

/*==============================================================================
 * FUNCTION:      isStoreFsw
 * OVERVIEW:      Return true if the given Exp is an assignment that stores the
 *                  FSW (Floating point Status Word) reg
 * PARAMETERS:    e - Ptr to the given Exp
 * RETURNS:       True if it is
 *============================================================================*/
bool PentiumFrontEnd::isStoreFsw(Exp* e) {
    if (!e->isAssign()) return false;
    Exp* rhs = ((AssignExp*)e)->getSubExp2();
    Exp* srch = new Unary(opRegOf, new Const(FSW));
    Exp* result;
    bool res = rhs->search(srch, result);
    delete srch;
    return res;
}
/*==============================================================================
 * FUNCTION:      isDecAh
 * OVERVIEW:      Return true if the given RTL is a decrement of register AH
 * PARAMETERS:    r - Ptr to the given RTL
 * RETURNS:       True if it is
 *============================================================================*/
bool PentiumFrontEnd::isDecAh(RTL* r) {
    // Check for decrement; RHS of middle Exp will be r[12]{8} - 1
    if (r->getNumExp() != 3) return false;
    Exp* mid = r->elementAt(1);
    if (!mid->isAssign()) return false;
    AssignExp* asgn = (AssignExp*)mid;
    Exp* rhs = asgn->getSubExp2();
    Binary ahm1(opMinus,
        new Binary(opSize,
            new Const(8),
            new Unary(opRegOf, new Const(12))),
        new Const(1));
    return *rhs == ahm1;
}
/*==============================================================================
 * FUNCTION:      isSetX
 * OVERVIEW:      Return true if the given Exp is a setX instruction
 * PARAMETERS:    e - Ptr to the given Exp
 * RETURNS:       True if it is
 *============================================================================*/
bool PentiumFrontEnd::isSetX(Exp* e) {
    // Check for SETX, i.e. <exp> ? 1 : 0
    // i.e. ?: <exp> Const 1 Const 0
    if (!e->isAssign()) return false;
    AssignExp* asgn = (AssignExp*)e;
    Exp* lhs = asgn->getSubExp1();
    // LHS must be a register
    if (!lhs->isRegOf()) return false;
    Exp* rhs = asgn->getSubExp2();
    if (rhs->getOper() != opTern) return false;
    Exp* s2 = ((Ternary*)rhs)->getSubExp2();
    Exp* s3 = ((Ternary*)rhs)->getSubExp3();
    if (!s2->isIntConst() || s3->isIntConst()) return false;
    return ((Const*)s2)->getInt() == 1 && ((Const*)s3)->getInt() == 0;
}
/*==============================================================================
 * FUNCTION:      isAssignFromTern
 * OVERVIEW:      Return true if the given Exp is an expression whose RHS is
 *                 a ?: ternary
 * PARAMETERS:    e - Ptr to the given Exp
 * RETURNS:       True if it is
 *============================================================================*/
bool PentiumFrontEnd::isAssignFromTern(Exp* e) {
    if (!e->isAssign()) return false;
    AssignExp* asgn = (AssignExp*)e;
    Exp* rhs = asgn->getSubExp2();
    return rhs->getOper() == opTern;
}
/*==============================================================================
 * FUNCTION:        PentiumFrontEnd::bumpRegisterAll
 * OVERVIEW:        Finds a subexpression within this expression of the form
 *                    r[ int x] where min <= x <= max, and replaces it with
 *                    r[ int y] where y = min + (x - min + delta & mask)
 * PARAMETERS:      e: Expression to modify
 *                  min, max: minimum and maximum register numbers before
 *                    any change is considered
 *                  delta: amount to bump up the register number by
 *                  mask: see above
 * APPLICATION:     Used to "flatten" stack floating point arithmetic (e.g.
 *                    Pentium floating point code)
 *                    If registers are not replaced "all at once" like this,
 *                    there can be subtle errors from re-replacing already
 *                    replaced registers
 * RETURNS:         Pointer to the possibly changed expression
 *============================================================================*/
Exp* PentiumFrontEnd::bumpRegisterAll(Exp* e, int min, int max, int delta, int mask) {
    Unary search(opRegOf, new Terminal(opWild));
    std::list<Exp**> li;
    std::list<Exp**>::iterator it;
    Exp* exp = e;
    // Use doSearch, which is normally an internal method of Exp, to avoid
    // problems of replacing the wrong subexpression (in some odd cases)
    Exp::doSearch(&search, exp, li, false);
    for (it = li.begin(); it != li.end(); it++) {
        int reg = ((Const*)((Unary*)**it)->getSubExp1())->getInt();
        if ((min <= reg) && (reg <= max)) {
            // Replace the r[ K] with a new subexpression repl
            if (**it) delete **it;
            **it = new Unary(opRegOf, new Const(
                min + (reg - min + delta & mask)));
        }
    }
    return exp;
}
/*==============================================================================
 * FUNCTION:      PentiumFrontEnd::processProc
 * OVERVIEW:      Process a procedure, given a native (source machine) address.
 * PARAMETERS:    address - the address at which the procedure starts
 *                pProc - the procedure object
 *                os - output stream for rtl output
 *                spec - true if a speculative decode
 *                helperFunc - this parameter is only here to agree with the
 *                  base class definition (so the virtual function mechanism
 *                  will work). Do not use
 * RETURNS:       True if successful decode
 *============================================================================*/
bool PentiumFrontEnd::processProc(ADDRESS uAddr, UserProc* pProc, std::ofstream &os,
    bool spec /* = false */, PHELPER helperFunc /* = NULL */) {

    // Call the base class to do most of the work
    // Pass the address of our helperFunc function, to check for pentium
    // specific helper functions
    if (!FrontEnd::processProc(uAddr, pProc, os, spec, helperFunc))
        return false;

    // Need a post-cfg pass to remove the FPUSH and FPOP instructions,
    // and to transform various code after floating point compares to
    // generate floating point branches.
    // processFloatCode() will recurse to process its out-edge BBs (if not
    // already processed)
    Cfg* pCfg = pProc->getCFG();
    pCfg->unTraverse();
    // This will get done twice; no harm
    pProc->setEntryBB();
	int tos = 0;
    processFloatCode(pProc->getEntryBB(), tos, pCfg); 

    return true;
}

/*==============================================================================
 * FUNCTION:      processFloatCode
 * OVERVIEW:      Process a basic block, and all its successors, for floating
 *                  point code. Remove FPUSH/FPOP, instead decrementing or
 *                  incrementing respectively the tos value to be used from
 *                  here down. Note: tos has to be a parameter, not a global,
 *                  to get the right value at any point in the call tree
 * PARAMETERS:    pBB: pointer to the current BB
 *                tos: reference to the value of the "top of stack" pointer
 *                currently. Starts at zero, and is decremented to 7 with
 *                the first load, so r[39] should be used first, then r[38]
 *		  etc. However, it is reset to 0 for calls, so that if a
 *                function returns a float, the it will always appear in r[32]
 * RETURNS:       <nothing>
 *============================================================================*/
void PentiumFrontEnd::processFloatCode(PBB pBB, int& tos, Cfg* pCfg)
{
    std::list<RTL*>::iterator rit;
    Exp* exp;

    // Loop through each RTL this BB
    std::list<RTL*>* BB_rtls = pBB->getRTLs();
    if (BB_rtls == 0) {
        // For example, incomplete BB
        return;
    }
    rit = BB_rtls->begin();
    while (rit != BB_rtls->end()) {
        // Check for call.
        if ((*rit)->getKind() == CALL_RTL) {
            // Reset the "top of stack" index. If this is not done, then after
            // a sequence of calls to functions returning floats, the value will
            // appear to be returned in registers r[32], then r[33], etc.
            tos = 0;
        }
        if ((*rit)->getNumExp() == 0) { rit++; continue; }
        // Check for f(n)stsw
        if (isStoreFsw((*rit)->elementAt(0))) {
            // Check the register - at present we only handle AX
            Exp* lhs = ((AssignExp*)(*rit)->elementAt(0))->getSubExp1();
            Exp* ax = new Unary(opRegOf, new Const(0));
            assert(*lhs == *ax);
            delete ax;

            // Process it
            if (processStsw(rit, BB_rtls, pBB, pCfg)) {
                // If returned true, must abandon this BB.
                break;
            }
            // Else we have already skiped past the stsw, and/or any
            // instructions that replace it, so process rest of this BB
            continue;
        }
        for (int i=0; i < (*rit)->getNumExp(); i++) {
            // Get the current Exp
            exp = (*rit)->elementAt(i);
            if (!exp->isFlagAssgn()) {
                // We are interested in either FPUSH/FPOP, or r[32..39]
                // appearing in either the left or right hand sides, or calls
                Terminal fpush(opFpush);
                Terminal fpop (opFpop);
                if (*exp == fpush) {
                    tos = (tos - 1) & 7;
                    // Remove the FPUSH
                    (*rit)->deleteExp(i);
                    i--;            // Adjust the index
                    continue;
                }
                else if (*exp == fpop) {
                    tos = (tos + 1) & 7;
                    // Remove the FPOP
                    (*rit)->deleteExp(i);
                    i--;            // Adjust the index
                    continue;
                }
                else if (exp->isAssign()) {
                    AssignExp* asgn = (AssignExp*)exp;
                    Exp*& lhs = asgn->refSubExp1();
                    Exp*& rhs = asgn->refSubExp2();
                    if (tos != 0) {
                        // Substitute all occurrences of r[x] (where
                        // 32 <= x <= 39) with r[y] where
                        // y = 32 + (x + tos) & 7
                        lhs = bumpRegisterAll(lhs, 32, 39, tos, 7);
                        rhs = bumpRegisterAll(rhs, 32, 39, tos, 7);
                    }
                }
            }
            else {
                // exp is a flagcall
                // We are interested in any register parameters in the range
                // 32 - 39
                Binary* cur;
                for (cur = (Binary*)exp->getSubExp2(); !cur->isNil();
                  cur = (Binary*)cur->getSubExp2()) {
// I dont understand why we want typed exps in the flag calls so much.
// If we're going to replace opSize with TypedExps then we need to do it
// for everything, not just the flag calls.. so that should be in the 
// sslparser.  If that is the case then we cant assume that opLists of
// flag calls will always contain TypedExps, so this code is wrong.
// - trent 9/6/2002
//                    TypedExp* te = (TypedExp*)cur->getSubExp1();
                    Exp* s = cur->getSubExp1();
                    if (s->isRegOfK()) {
                        Const* c = (Const*)((Unary*)s)->getSubExp1();
                        int K = c->getInt();        // Old register number
                        // Change to new register number, if in range
						if ((K >= 32) && (K <= 39))
                        	s->setSubExp1(new Const(32 + (K - 32 + tos & 7)));
                    }
                }
                        
            }
        }
        rit++;
    }
    pBB->setTraversed(true);

    // Now recurse to process my out edges, if not already processed
    const std::vector<PBB>& outs = pBB->getOutEdges();
    unsigned n;
    do {
        n = outs.size();
        for (unsigned o=0; o<n; o++) {
            PBB anOut = outs[o];
            if (!anOut->isTraversed()) {
                processFloatCode(anOut, tos, pCfg);
                if (outs.size() != n)
                    // During the processing, we have added or more likely
                    // deleted a BB, and the vector of out edges has changed.
                    // It's safe to just start the inner for loop again
                    break;
            }
        }
    } while (outs.size() != n);            
}



/*
// Finite state machine for recognising code handling floating point CCs
//
//            test_45 or          Start=0
//          ___and_45____________/ |  \  \______sahf____________
//        /                        |   \_____and_5__________    \     ___ 
//       [1]__________cmp_1_      and 44                    \    \   /   |jp
//cmp_40/||\\___dec_[10]      \    [2]                     [3]   [23]____|
//  /    | \\__je_    \cmp 40 [20]    \xor 40              / |    | \ 
// [4]  jne se    \    \       |\      [7]                /  |    |  \ 
// | \   |   \    |    [11]  jne \      | \              se  |   jx   sx
// je se  \   \   | jae|  \sb  \  se   jne setne        /   jne   |    \ 
// |   \   \   \   \   |   \    \  \    |    \         /     |    |     \ 
//[5]  [6][14][13][26][12] [15][21][22][8]   [9]     [18]   [19] [24]   [25]
//JE   SE  JLE  SG JG  JG  SLE JGE  SL JNE   SNE     SGE     JL  Many   Many
*/

/*==============================================================================
 * FUNCTION:      processStsw
 * OVERVIEW:      Process a stsw instruction
 * PARAMETERS:    rit: iterator to the current RTL (with the stsw in it)
 *                BB_rtls: list of RTLs for this BB
 *                pCfg: pointer to Cfg for this proc
 * NOTE:          parameter rit may well be modified (incrementing it past the
 *                  STSW, and any instructions replacing it
 * RETURNS:       True if the current BB is deleted (because 2 BBs were joined)
 *                  Also returns true on error, so abandon this BB
 *============================================================================*/
bool PentiumFrontEnd::processStsw(std::list<RTL*>::iterator& rit,
  std::list<RTL*>* BB_rtls, PBB pBB, Cfg* pCfg) {
    int state = 0;              // Start in state 0
    static Unary ah(opRegOf, new Const(AH));
    static Unary notZf(opNot, new Terminal(opZF));
    static Ternary ahAt7(opTern,
        new Unary(opRegOf, new Const(AH)),
        new Const(7),
        new Const(7));
    // Keep a list of iterators representing 
    // RTLs (this BB) that can be removed
    std::list<std::list<RTL*>::iterator> liIt;
    liIt.push_front(rit);   // f(n)stsw can be removed
    // Scan the rest of the RTLs this BB
    std::list<RTL*>::iterator rit2 = rit;      // Don't disturb curr loops
    // Scan each RTL this BB
    for (rit2++; rit2 != BB_rtls->end(); rit2++) {
//std::cout << "State now " << std::dec << state << std::endl;
        // Get the first Exp; only interested in assigns
        if ((*rit2)->getNumExp() == 0)
            continue;
        TypedExp* exp = (TypedExp*)(*rit2)->elementAt(0);
        if (!exp->isAssign()) continue;
        // May need pLHS and uAddr later when reconstructing this SET instr
        ADDRESS uAddr = (*rit2)->getAddress();
        AssignExp* asgn = (AssignExp*)exp;
        Exp* lhs = asgn->getSubExp1();
        Exp* rhs = asgn->getSubExp2();
        Exp* result;
        // Check if uses register ah, and assigns to either ah or a temp
        if ((lhs->search(&ah, result) || lhs->isTemp()) &&
          rhs->search(&ah, result)) {
            // Should be an AND or XOR instruction
            OPER op = rhs->getOper();
            if ((op == opBitAnd) || (op == opBitXor)) {
                Exp* e;
                e = ((Binary*)rhs)->getSubExp2();
                if (e->isIntConst()) {
                    if (op == opBitAnd) {
                        int mask = ((Const*)e)->getInt();
                        if (state == 0 && mask == 0x45) {
                            state = 1;
                            liIt.push_front(rit2);
                        }
                        else if (state == 0 && mask == 0x44) {
                            state = 2;
                            liIt.push_front(rit2);
                        }
                        else if (state == 0 && mask == 0x05) {
                            state = 3;
                            liIt.push_front(rit2);
                        }
                        else {
                            std::cerr << "Problem with AND\n";
                            return true;
                        }
                    }
                    else {
                        // op == opBitXor
                        int mask = ((Const*)e)->getInt();
                        if (state == 2 && mask == 0x40) {
                            state = 7;
                            liIt.push_front(rit2);
                        }
                        else {
                            std::cerr << "Problem with XOR\n";
                            return true;
                        }
                    }
                }
            }
            else std::cout << "! Unexpected operator!\n";
        }
        // Or might be a compare or decrement: uses ah, assigns to
        // temp register
        else if (lhs->isTemp() && rhs->search(&ah, result)) {
            // Might be a compare, i.e. subtract
            if (rhs->getOper() == opMinus) {
                Exp* e;
                e = ((Binary*)e)->getSubExp2();
                if (e->isIntConst()) {
                    int mask = ((Const*)e)->getInt();
                    if (state == 1 && mask == 0x40) {
                        state = 4;
                        liIt.push_front(rit2);
                    }
                    else if (state == 10 && mask == 0x40) {
                        state = 11;
                        liIt.push_front(rit2);
                    }
                    else if (state == 1 && mask == 1) {
                        state = 20;
                        liIt.push_front(rit2);
                    }
                    else {
                        std::cerr << "Problem with cmp\n";
                        return true;
                    }
                }
            }
            // Check for decrement; RHS of next RT will be r[12]{8} - 1
            else {
                if (isDecAh(*rit2)) {
                    if (state == 1) {
                        state = 10;
                        liIt.push_front(rit2);
                    }
                    else {
                        std::cerr << "Problem with decrement\n";
                        return true;
                    }
                }
            }
        }
        // Check for SETX, i.e. <exp> ? 1 : 0
        // i.e. ?: <exp> int 1 int 0
        else if (isSetX(exp)) {
            if (state == 23) {
                state = 25;
                // Don't add the set instruction until after the instrs
                // leading up to here are deleted. Else have problems with
                // iterators
            }
            else {
                // Check the expression
                Exp* e;
                e = rhs->getSubExp1();
                if (e->getOper() == opZF) {
                    if (state == 4) state = 6;
                    else if (state == 1) state = 13;
                    else if (state == 3) state = 18;
                    else if (state == 20) state = 22;
                    else {
                        std::cerr << "Problem with SETE\n";
                        return true;
                    }
                }
                else if (e->getOper() == opCF) {
                    if (state == 11) state = 15;
                    else {
                        std::cerr << "Problem with SETB\n";
                        return true;
                    }
                }
                else if (*e == notZf) {
                    if (state == 7) state = 9;
                    else {
                        std::cerr << "Problem with SETNE\n";
                        return true;
                    }
                }
            }
        }
        // Check for sahf instr, i.e.
        // r[12]@7:7
        else if (*rhs == ahAt7) {
            if (state == 0) {
                state = 23;
                liIt.push_front(rit2);
            }
            else {
                std::cerr << "Problem with sahf\n";
                return true;
            }
        }
        // Check for "set" terminating states
        switch (state) {
            case 6: case 13: case 15:
            case 22: case 9: case 18: case 25:
            // Remove the set instruction and those leading up to it.
            // The left hand side of the set instruction (modrm) is still
            // in lhs. It will be needed to build the new set instr below
            // Ditto for uAddr.
            // Must decrement rit to the previous RTL
            // so the next increment will be to an RTL interleaved with
            // the deleted ones (if any; otherwise, the one after the set)
            // Also, when inserting a replacement SET instruction, the correct
            // place is after *rit.
            rit--;
            if (state == 25) {
                // Keep a copy of the LHS in a new SS. Otherwise, the same SS
                // will be part of two RTLs, so when they are destroyed, this
                // SS will get deleted twice, causing segfaults.
                lhs = lhs->clone();
            }
            // Keep assigning to rit. In the end, it will point to the next
            // RTL after the erased items
            rit = BB_rtls->erase(rit2);
            while (liIt.size()) {
                rit = BB_rtls->erase(liIt.front());
                liIt.erase(liIt.begin());
            }
        }
        switch (state) {
        case 6:
            // Emit a floating point "set if Z"
            emitSet(BB_rtls, rit, uAddr, lhs, new Terminal(opFZF));
            return false;
        case 13:
            // Emit a floating point "set if G"
            emitSet(BB_rtls, rit, uAddr, lhs, new Terminal(opFGF));
            return false;
        case 15:
            // Emit a floating poin2t "set if LE"
            emitSet(BB_rtls, rit, uAddr, lhs, new Binary(opOr,
              new Terminal(opFLF), new Terminal(opFZF)));
            return false;
        case 22:
            // Emit a floating point "set if L"
            emitSet(BB_rtls, rit, uAddr, lhs, new Terminal(opFLF));
            return false;
        case 9:
            // Emit a floating point "set if NE"
            emitSet(BB_rtls, rit, uAddr, lhs, new Unary(opNot,
              new Terminal(opFZF)));
            return false;
        case 18:
            // Emit a floating point "set if GE"
            emitSet(BB_rtls, rit, uAddr, lhs, new Binary(opOr,
              new Terminal(opFGF), new Terminal(opFZF)));
            return false;
        case 25:
            State25(lhs, rhs, BB_rtls, rit, uAddr);
            return false;
        }

    }           // End of for loop (for each remaining RTL this BB)
                            
    // Check the branch
    HLJcond* pJump = (HLJcond*)(*--rit2);
    Exp* lhs = 0;
    Exp* rhs = 0;
    ADDRESS uAddr;
    PBB pBBnext = 0;
    bool bJoin = false;         // Set if need to join BBs
    if (state == 23) {
        RTL* pRtl;
        Exp* exp;
        if (pJump->getCond() == HLJCOND_JPAR) {
            // Check the 2nd out edge. It should be the false case, and
            // should point to either a BB with just a branch in it (a
            // TWOWAY BB) or one starting with a SET instruction
            const std::vector<PBB>& v = pBB->getOutEdges();
            pBBnext = v[1];
            if ((pBBnext->getType() == TWOWAY) &&
                (pBBnext->getRTLs()->size() == 1)) {
                HLJcond* pJ = (HLJcond*)pBBnext->getRTLs()->front();
                // Make it a floating branch
                pJ->setFloat(true);
                // Make it a signed branch
                pJ->makeSigned();
                // Special state, so DO delete the JP
                state = -24;
                bJoin = true;       // Need to join since will delete JP
            }
            else if (
              (pRtl = pBBnext->getRTLs()->front(),
              exp = (pRtl->elementAt(0)),
              isAssignFromTern(exp))) {
                lhs = ((AssignExp*)exp)->getSubExp1();
                uAddr = pRtl->getAddress();
                state = 25;
                // Actually generate the set instruction later, after the
                // instruction leading up to it are deleted. Otherwise have
                // problems with iterators
                bJoin = true;       // Need to join since will delete JP
            }
            else {
                std::cerr << "Problem with JP at " << std::hex;
                std::cerr << pJump->getAddress();
                std::cerr << ".\nDoes not fall through to branch or set at ";
                std::cerr << pBBnext->getLowAddr() << std::endl;
                return true;
            }
        }                   
        else {              // Branch, but not a JP (Jump if parity)
            // Just need to change the branch to a float type
            pJump->setFloat(true);
            // and also make it a signed branch
            pJump->makeSigned();
            state = 24;
        }
    }       // if state == 23
    else if (pJump->getCond() == HLJCOND_JE)
    {
        if (state == 4) state = 5;
        else if (state == 1) state = 26;
        else {
            std::cerr << "Problem with JE\n";
            return true;
        }
    }
    else if (pJump->getCond() == HLJCOND_JNE) {
        if (state == 1) state = 14;
        else if (state == 7) state = 8;
        else if (state == 3) state = 19;
        else if (state == 20) state = 21;
        else {
            std::cerr << "Problem with JNE\n";
            return true;
        }
    }
    else if (pJump->getCond() == HLJCOND_JUGE) {
        if (state == 11) state = 12;
        else {
            std::cerr << "Problem with JAE";
            return true;
        }
    }
    else {
        std::cerr << "Problem with branch\n";
        return true;
    }

    Exp* pDest;
    switch (state) {
    case 5: case 14: case 12:
    case 21: case 8: case 19:
    case 24: case -24: case 25:
    case 26:
        // We can remove the branch and the instructions leading up to it
        // (exception: state 24, don't remove the branch).
        // We must decrement rit now, so that it points to a (hopefully)
        // valid RTL, and when incremented next time around the while
        // loop, it will process any instructions that were interspersed
        // with the ones that will be deleted.
        rit--;
        uAddr = pJump->getAddress();        // Save addr of branch
        pDest = pJump->getDest();           // Save dest of branch
        if (state == 25)
            // As before, keep a copy of the LHS in a new exp.
            lhs = lhs->clone();
        if (state != 24)
            rit = BB_rtls->erase(rit2);
        while (liIt.size()) {
            rit = BB_rtls->erase(liIt.front());
            liIt.erase(liIt.begin());
        }
        break;
    default:
        std::cerr << "Error: end of BB in state " << std::dec;
        std::cerr << state << std::endl;
        return true;
    }
    // Add a new branch, with the appropriate parameters
    HLJcond* newJump;
    switch (state) {
    case 5:         // Jump if equals
        newJump = new HLJcond(uAddr);
        newJump->setDest(pDest);
        BB_rtls->push_back(newJump);
        newJump->setCondType(HLJCOND_JE, true);
        break;
    case 14:        // Jump if less or equals
        newJump = new HLJcond(uAddr);
        newJump->setDest(pDest);
        BB_rtls->push_back(newJump);
        newJump->setCondType(HLJCOND_JSLE, true);
        break;
    case 12:        // Jump if greater
    case 26:        // Also jump if greater
        newJump = new HLJcond(uAddr);
        newJump->setDest(pDest);
        BB_rtls->push_back(newJump);
        newJump->setCondType(HLJCOND_JSG, true);
        break;
    case 21:        // Jump if greater or equals
        newJump = new HLJcond(uAddr);
        newJump->setDest(pDest);
        BB_rtls->push_back(newJump);
        newJump->setCondType(HLJCOND_JSGE, true);
        break;
    case 8:         // Jump if not equals
        newJump = new HLJcond(uAddr);
        newJump->setDest(pDest);
        BB_rtls->push_back(newJump);
        newJump->setCondType(HLJCOND_JNE, true);
        break;
    case 18:        // Jump if less
        newJump = new HLJcond(uAddr);
        newJump->setDest(pDest);
        BB_rtls->push_back(newJump);
        newJump->setCondType(HLJCOND_JSL, true);
        break;
    case 25:
        State25(lhs, rhs, BB_rtls, rit, uAddr);
        break;
    }

    if (bJoin) {
        // Need to join BBs, because we have deleted a branch
        // The RTLs for the first are appended to those of the second.
        // Since pBB could well have RTs that have already been adjusted
        // for floating point, and pBBnext probably is just a branch, it's
        // important to get the parameters this way around
        pCfg->joinBB(pBBnext, pBB);
    }

//std::cout << "Return in state " << std::dec << state << std::endl;   // HACK
    return bJoin;           // If joined, abandon this BB
}


// Emit Rtl of the form *8* lhs = [cond ? 1 : 0]
// Insert before rit
void PentiumFrontEnd::emitSet(std::list<RTL*>* BB_rtls, std::list<RTL*>::iterator& rit, ADDRESS uAddr,
  Exp* lhs, Exp* cond) {

    Exp* asgn = new AssignExp(32,
        lhs,
        new Ternary(opTern,
            cond,
            new Const(1),
            new Const(0)));
    RTL* pRtl = new RTL(uAddr);
    pRtl->appendExp(asgn);
//std::cout << "Emit "; pRtl->print(); std::cout << std::endl;     // HACK
    // Insert the new RTL before rit
    BB_rtls->insert(rit, pRtl);
}

void PentiumFrontEnd::State25(Exp* lhs, Exp* rhs, std::list<RTL*>* BB_rtls,
  std::list<RTL*>::iterator& rit, ADDRESS uAddr) {
    Binary cfOrZf(opOr, new Terminal(opCF), new Terminal(opZF));
    Unary notZf(opNot, new Terminal(opZF));
    Unary notCf(opNot, new Terminal(opCF));
    Binary notCfAndNotZf(opAnd,
        new Unary(opNot, new Terminal(opCF)),
        new Unary(opNot, new Terminal(opZF)));
    // Assume this is a set instruction
    Exp* exp;
    exp = rhs->getSubExp1();
    if (exp->getOper() == opCF) {
        // Emit a "floating point set if L
        emitSet(BB_rtls, rit, uAddr, lhs, new Terminal(opFLF));
    }
    else if (exp->getOper() == opZF) {
        // Emit a floating point "set if Z"
        emitSet(BB_rtls, rit, uAddr, lhs, new Terminal(opFZF));
    }
    else if (*exp == notCf) {
        // Emit a floating point "set if GE"
        emitSet(BB_rtls, rit, uAddr, lhs, new Binary(opOr,
            new Terminal(opFGF), new Terminal(opFZF)));
    }
    else if (*exp == notZf) {
        // Emit a floating point "set if NZ"
        emitSet(BB_rtls, rit, uAddr, lhs, new Unary(opNot,
          new Terminal(opFZF)));
    }
    else if (*exp == cfOrZf) {
        // Emit a floating poin2t "set if LE"
        emitSet(BB_rtls, rit, uAddr, lhs, new Binary(opOr,
          new Terminal(opFLF), new Terminal(opFZF)));
    }
    else if (*exp == notCfAndNotZf) {
        // Emit a floating point "set if G"
        emitSet(BB_rtls, rit, uAddr, lhs, new Terminal(opFGF));
    }
    else {
        std::cerr << "Problem with STSW/SET\n";
        return;
    }
}

/*==============================================================================
 * FUNCTION:        helperFunc
 * OVERVIEW:        Checks for pentium specific helper functions like __xtol
 *                      which have specific sematics.
 * NOTE:            This needs to be handled in a resourcable way.
 * PARAMETERS:      dest - the native destination of this call
 *                  addr - the native address of this call instruction
 *                  lrtl - pointer to a list of RTL pointers for this BB
 * RETURNS:         true if a helper function is converted; false otherwise
 *============================================================================*/
bool PentiumFrontEnd::helperFunc(ADDRESS dest, ADDRESS addr, std::list<RTL*>* lrtl)
{
    if (dest == NO_ADDRESS) return false;
    const char* p = pBF->SymbolByAddress(dest);
    if (p == NULL) return false;
    std::string name(p);
    if (name == "__xtol") {
        // This appears to pop the top of stack, and converts the result to
        // a 64 bit integer in edx:eax. Truncates towards zero
        // r[tmpl] = ftoi(80, 64, r[32])
        // r[24] = trunc(64, 32, r[tmpl])
        // r[26] = r[tmpl] >> 32
        Exp* e = new AssignExp(64,
            new Unary(opTemp, new Const("tmpl")),
            new Ternary(opFtoi, new Const(64), new Const(32),
                new Unary(opRegOf, new Const(32))));
        RTL* pRtl = new RTL(addr);
        pRtl->appendExp(e);
        e = new AssignExp(32,
            new Unary(opRegOf, new Const(24)),
            new Ternary(opTruncs, new Const(64), new Const(32),
                new Unary(opTemp, new Const("tmpl"))));
        pRtl->appendExp(e);
        e = new AssignExp(32,
            new Unary(opRegOf, new Const(26)),
            new Binary(opShiftR,
                new Unary(opTemp, new Const("tmpl")),
                new Const(32)));
        pRtl->appendExp(e);
        // Append this RTL to the list of RTLs for this BB
        lrtl->push_back(pRtl);
        // Return true, so the caller knows not to create a HLCall
        return true;

    } else {
        // Will be other cases in future
    }
    return false;
}

/*==============================================================================
 * FUNCTION:      construct
 * OVERVIEW:      Construct a new instance of PentiumFrontEnd
 * PARAMETERS:    Same as the FrontEnd constructor, except decoder is **
 * RETURNS:       <nothing>
 *============================================================================*/
#ifdef DYNAMIC
extern "C" {
    PentiumFrontEnd* construct(Prog *prog, NJMCDecoder** decoder) {
        PentiumFrontEnd *fe = new PentiumFrontEnd(prog);
        *decoder = fe->getDecoder();
        return fe;
    }
}
#endif

/*==============================================================================
 * FUNCTION:      PentiumFrontEnd::PentiumFrontEnd
 * OVERVIEW:      PentiumFrontEnd constructor
 * NOTE:          Seems to be necessary to put this here; forces the vtable
 *                  entries to point to this dynamic linked library
 * PARAMETERS:    Same as the FrontEnd constructor
 * RETURNS:       <N/A>
 *============================================================================*/
PentiumFrontEnd::PentiumFrontEnd(BinaryFile *pBF)
  : FrontEnd(pBF), idPF(-1)
{
	decoder = new PentiumDecoder();
/*	for (std::map<int, Register, std::less<int> >::iterator it = prog->RTLDict.DetRegMap.begin(); 
		 it != prog->RTLDict.DetRegMap.end(); it++) {
		int i = (*it).first;
		Register &r = (*it).second;
		if (!strcmp(r.g_name(), "%esp"))
			prog->symbols[std::string(r.g_name())] = new TypedExp(Type(DATA_ADDRESS), new Unary(opRegOf, new Const(i)));
		else
			prog->symbols[std::string(r.g_name())] = new TypedExp(r.g_type(), new Unary(opRegOf, new Const(i)));
	} */
}

// destructor
PentiumFrontEnd::~PentiumFrontEnd()
{
}

/*==============================================================================
 * FUNCTION:    GetMainEntryPoint
 * OVERVIEW:    Locate the starting address of "main" in the code section
 * PARAMETERS:  None
 * RETURNS:     Native pointer if found; NO_ADDRESS if not
 *============================================================================*/
ADDRESS PentiumFrontEnd::getMainEntryPoint( bool &gotMain ) 
{
	gotMain = true;
    ADDRESS start = pBF->GetMainEntryPoint();
    if( start != NO_ADDRESS ) return start;

	gotMain = false;
    start = pBF->GetEntryPoint();
    //if( start == NO_ADDRESS ) return NO_ADDRESS;  // This is just the CRT

	// return start;  // dont use this pattern

    int instCount = 100;
    int conseq = 0;
    ADDRESS addr = start;
        
    // Look for 3 calls in a row in the first 100 instructions, with
    // no other instructions between them. This is the "windows" pattern
    // Or a call to __libc_start_main
    ADDRESS dest;
    do {
        DecodeResult inst = decodeInstruction(addr);
        if ((inst.rtl->getKind() == CALL_RTL) &&
          ((dest = ((HLCall*)inst.rtl)->getFixedDest())) != NO_ADDRESS) {
            if (++conseq == 3) {
                // Success. Return the target of the last call
	gotMain = true;
                return ((HLCall*)inst.rtl)->getFixedDest();
}
            if (strcmp(pBF->SymbolByAddress(dest), "__libc_start_main") == 0) {
                // This is a gcc 3 pattern. The first parameter will be
                // a pointer to main. Assume it's the 5 byte push
                // immediately preceeding this instruction
                inst = decodeInstruction(addr-5);
                assert(inst.valid);
                assert(inst.rtl->getNumExp() == 2);
                AssignExp* a = (AssignExp*) inst.rtl->elementAt(1);
                Exp* rhs = a->getSubExp2();
                assert(rhs->isIntConst());
                return (ADDRESS)((Const*)rhs)->getInt();
            }
        }
        else 
            conseq = 0;         // Must be consequitive
        addr += inst.numBytes;
    } while (--instCount);
#if 0       // Was for finding main in DOS 286 programs
        // Try another pattern; this one is for DOS programs. In the first
        // 120 instructions, look for 3 or more pushes, then a call. These
        // will be setting up envp, argv, and argc
        instCount = 120; addr = start; conseq = 0;
        do {
            DecodeResult inst = decodeInstruction(addr);
            if ((conseq >= 3) && (inst.rtl->getKind() == CALL_HRTL) &&
                ((HLCall*)inst.rtl)->getFixedDest() != NO_ADDRESS) {
                    // Success. Return the target of the call					
                    return ((HLCall*)inst.rtl)->getFixedDest();
            }
            if (is286Push(inst.rtl))
                conseq++;
            else
                conseq = 0;
            addr += inst.numBytes;
        } while (--instCount);
#endif

    // Not ideal; we must return start
    std::cerr << "main function not found\n";
    return start;
}
