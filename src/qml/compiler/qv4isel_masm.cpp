/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qv4isel_masm_p.h"
#include "qv4runtime_p.h"
#include "qv4object_p.h"
#include "qv4functionobject_p.h"
#include "qv4regexpobject_p.h"
#include "qv4unwindhelper_p.h"
#include "qv4lookup_p.h"
#include "qv4function_p.h"
#include "qv4ssa_p.h"
#include "qv4exception_p.h"
#include "qv4regalloc_p.h"

#include <assembler/LinkBuffer.h>
#include <WTFStubs.h>

#include <iostream>
#include <cassert>

#if USE(UDIS86)
#  include <udis86.h>
#endif

using namespace QQmlJS;
using namespace QQmlJS::MASM;
using namespace QV4;

CompilationUnit::~CompilationUnit()
{
    UnwindHelper::deregisterFunctions(runtimeFunctions);
}

void CompilationUnit::linkBackendToEngine(ExecutionEngine *engine)
{
    runtimeFunctions.resize(data->functionTableSize);
    for (int i = 0 ;i < runtimeFunctions.size(); ++i) {
        const CompiledData::Function *compiledFunction = data->functionAt(i);

        QV4::Function *runtimeFunction = new QV4::Function(engine, this, compiledFunction,
                                                           (Value (*)(QV4::ExecutionContext *, const uchar *)) codeRefs[i].code().executableAddress(),
                                                           codeRefs[i].size());
        runtimeFunctions[i] = runtimeFunction;
    }

    UnwindHelper::registerFunctions(runtimeFunctions);
}

QV4::ExecutableAllocator::ChunkOfPages *CompilationUnit::chunkForFunction(int functionIndex)
{
    if (functionIndex < 0 || functionIndex >= codeRefs.count())
        return 0;
    JSC::ExecutableMemoryHandle *handle = codeRefs[functionIndex].executableMemory();
    if (!handle)
        return 0;
    return handle->chunk();
}

namespace {
class ConvertTemps: protected V4IR::StmtVisitor, protected V4IR::ExprVisitor
{
    int _nextFreeStackSlot;
    QHash<V4IR::Temp, int> _stackSlotForTemp;

    void renumber(V4IR::Temp *t)
    {
        if (t->kind != V4IR::Temp::VirtualRegister)
            return;

        int stackSlot = _stackSlotForTemp.value(*t, -1);
        if (stackSlot == -1) {
            stackSlot = _nextFreeStackSlot++;
            _stackSlotForTemp[*t] = stackSlot;
        }

        t->kind = V4IR::Temp::StackSlot;
        t->index = stackSlot;
    }

public:
    ConvertTemps()
        : _nextFreeStackSlot(0)
    {}

    void toStackSlots(V4IR::Function *function)
    {
        _stackSlotForTemp.reserve(function->tempCount);

        foreach (V4IR::BasicBlock *bb, function->basicBlocks)
            foreach (V4IR::Stmt *s, bb->statements)
                s->accept(this);

        function->tempCount = _nextFreeStackSlot;
    }

protected:
    virtual void visitConst(V4IR::Const *) {}
    virtual void visitString(V4IR::String *) {}
    virtual void visitRegExp(V4IR::RegExp *) {}
    virtual void visitName(V4IR::Name *) {}
    virtual void visitTemp(V4IR::Temp *e) { renumber(e); }
    virtual void visitClosure(V4IR::Closure *) {}
    virtual void visitConvert(V4IR::Convert *e) { e->expr->accept(this); }
    virtual void visitUnop(V4IR::Unop *e) { e->expr->accept(this); }
    virtual void visitBinop(V4IR::Binop *e) { e->left->accept(this); e->right->accept(this); }
    virtual void visitCall(V4IR::Call *e) {
        e->base->accept(this);
        for (V4IR::ExprList *it = e->args; it; it = it->next)
            it->expr->accept(this);
    }
    virtual void visitNew(V4IR::New *e) {
        e->base->accept(this);
        for (V4IR::ExprList *it = e->args; it; it = it->next)
            it->expr->accept(this);
    }
    virtual void visitSubscript(V4IR::Subscript *e) { e->base->accept(this); e->index->accept(this); }
    virtual void visitMember(V4IR::Member *e) { e->base->accept(this); }
    virtual void visitExp(V4IR::Exp *s) { s->expr->accept(this); }
    virtual void visitMove(V4IR::Move *s) { s->target->accept(this); s->source->accept(this); }
    virtual void visitJump(V4IR::Jump *) {}
    virtual void visitCJump(V4IR::CJump *s) { s->cond->accept(this); }
    virtual void visitRet(V4IR::Ret *s) { s->expr->accept(this); }
    virtual void visitTry(V4IR::Try *s) { s->exceptionVar->accept(this); }
    virtual void visitPhi(V4IR::Phi *) { Q_UNREACHABLE(); }
};
} // anonymous namespace

/* Platform/Calling convention/Architecture specific section */

#if CPU(X86_64)
static const Assembler::RegisterID calleeSavedRegisters[] = {
    // Not used: JSC::X86Registers::rbx,
    // Not used: JSC::X86Registers::r10,
    JSC::X86Registers::r12, // LocalsRegister
    // Not used: JSC::X86Registers::r13,
    JSC::X86Registers::r14 // ContextRegister
    // Not used: JSC::X86Registers::r15,
};
#endif

#if CPU(X86)
static const Assembler::RegisterID calleeSavedRegisters[] = {
    // Not used: JSC::X86Registers::ebx,
    JSC::X86Registers::esi, // ContextRegister
    JSC::X86Registers::edi  // LocalsRegister
};
#endif

#if CPU(ARM)
static const Assembler::RegisterID calleeSavedRegisters[] = {
    // ### FIXME: remove unused registers.
    // Keep these in reverse order and make sure to also edit the unwind program in
    // qv4unwindhelper_p-arm.h when changing this list.
    JSC::ARMRegisters::r12,
    JSC::ARMRegisters::r10,
    JSC::ARMRegisters::r9,
    JSC::ARMRegisters::r8,
    JSC::ARMRegisters::r7,
    JSC::ARMRegisters::r6,
    JSC::ARMRegisters::r5,
    JSC::ARMRegisters::r4
};
#endif

const int Assembler::calleeSavedRegisterCount = sizeof(calleeSavedRegisters) / sizeof(calleeSavedRegisters[0]);

/* End of platform/calling convention/architecture specific section */


const Assembler::VoidType Assembler::Void;

Assembler::Assembler(InstructionSelection *isel, V4IR::Function* function, QV4::ExecutableAllocator *executableAllocator,
                     int maxArgCountForBuiltins)
    : _stackLayout(function, maxArgCountForBuiltins)
    , _constTable(this)
    , _function(function)
    , _nextBlock(0)
    , _executableAllocator(executableAllocator)
    , _isel(isel)
{
}

void Assembler::registerBlock(V4IR::BasicBlock* block, V4IR::BasicBlock *nextBlock)
{
    _addrs[block] = label();
    _nextBlock = nextBlock;
}

void Assembler::jumpToBlock(V4IR::BasicBlock* current, V4IR::BasicBlock *target)
{
    if (target != _nextBlock)
        _patches[target].append(jump());
}

void Assembler::addPatch(V4IR::BasicBlock* targetBlock, Jump targetJump)
{
    _patches[targetBlock].append(targetJump);
}

void Assembler::addPatch(DataLabelPtr patch, Label target)
{
    DataLabelPatch p;
    p.dataLabel = patch;
    p.target = target;
    _dataLabelPatches.append(p);
}

void Assembler::addPatch(DataLabelPtr patch, V4IR::BasicBlock *target)
{
    _labelPatches[target].append(patch);
}

Assembler::Pointer Assembler::loadTempAddress(RegisterID reg, V4IR::Temp *t)
{
    int32_t offset = 0;
    int scope = t->scope;
    RegisterID context = ContextRegister;
    if (scope) {
        loadPtr(Address(ContextRegister, offsetof(ExecutionContext, outer)), ScratchRegister);
        --scope;
        context = ScratchRegister;
        while (scope) {
            loadPtr(Address(context, offsetof(ExecutionContext, outer)), context);
            --scope;
        }
    }
    switch (t->kind) {
    case V4IR::Temp::Formal:
    case V4IR::Temp::ScopedFormal: {
        loadPtr(Address(context, offsetof(CallContext, arguments)), reg);
        offset = t->index * sizeof(Value);
    } break;
    case V4IR::Temp::Local:
    case V4IR::Temp::ScopedLocal: {
        loadPtr(Address(context, offsetof(CallContext, locals)), reg);
        offset = t->index * sizeof(Value);
    } break;
    case V4IR::Temp::StackSlot: {
        return stackSlotPointer(t);
    } break;
    default:
        Q_UNIMPLEMENTED();
    }
    return Pointer(reg, offset);
}

Assembler::Pointer Assembler::loadStringAddress(RegisterID reg, const QString &string)
{
    loadPtr(Address(Assembler::ContextRegister, offsetof(QV4::ExecutionContext, runtimeStrings)), reg);
    const int id = _isel->registerString(string);
    return Pointer(reg, id * sizeof(QV4::String*));
}

template <typename Result, typename Source>
void Assembler::copyValue(Result result, Source source)
{
#ifdef VALUE_FITS_IN_REGISTER
    // Use ReturnValueRegister as "scratch" register because loadArgument
    // and storeArgument are functions that may need a scratch register themselves.
    loadArgumentInRegister(source, ReturnValueRegister, 0);
    storeReturnValue(result);
#else
    loadDouble(source, FPGpr0);
    storeDouble(FPGpr0, result);
#endif
}

template <typename Result>
void Assembler::copyValue(Result result, V4IR::Expr* source)
{
#ifdef VALUE_FITS_IN_REGISTER
    if (source->type == V4IR::DoubleType) {
        storeDouble(toDoubleRegister(source), result);
    } else {
        // Use ReturnValueRegister as "scratch" register because loadArgument
        // and storeArgument are functions that may need a scratch register themselves.
        loadArgumentInRegister(source, ReturnValueRegister, 0);
        storeReturnValue(result);
    }
#else
    if (V4IR::Temp *temp = source->asTemp()) {
        loadDouble(temp, FPGpr0);
        storeDouble(FPGpr0, result);
    } else if (V4IR::Const *c = source->asConst()) {
        QV4::Value v = convertToValue(c);
        storeValue(v, result);
    } else {
        assert(! "not implemented");
    }
#endif
}


void Assembler::storeValue(QV4::Value value, V4IR::Temp* destination)
{
    Address addr = loadTempAddress(ScratchRegister, destination);
    storeValue(value, addr);
}

void Assembler::enterStandardStackFrame(bool withLocals)
{
    platformEnterStandardStackFrame();

    // ### FIXME: Handle through calleeSavedRegisters mechanism
    // or eliminate StackFrameRegister altogether.
    push(StackFrameRegister);
    move(StackPointerRegister, StackFrameRegister);

    int frameSize = _stackLayout.calculateStackFrameSize(withLocals);

    subPtr(TrustedImm32(frameSize), StackPointerRegister);

    for (int i = 0; i < calleeSavedRegisterCount; ++i)
        storePtr(calleeSavedRegisters[i], Address(StackFrameRegister, -(i + 1) * sizeof(void*)));

    move(StackFrameRegister, LocalsRegister);
}

void Assembler::leaveStandardStackFrame(bool withLocals)
{
    // restore the callee saved registers
    for (int i = calleeSavedRegisterCount - 1; i >= 0; --i)
        loadPtr(Address(StackFrameRegister, -(i + 1) * sizeof(void*)), calleeSavedRegisters[i]);

    int frameSize = _stackLayout.calculateStackFrameSize(withLocals);
    // Work around bug in ARMv7Assembler.h where add32(imm, sp, sp) doesn't
    // work well for large immediates.
#if CPU(ARM_THUMB2)
    move(TrustedImm32(frameSize), JSC::ARMRegisters::r3);
    add32(JSC::ARMRegisters::r3, StackPointerRegister);
#else
    addPtr(TrustedImm32(frameSize), StackPointerRegister);
#endif

    pop(StackFrameRegister);
    platformLeaveStandardStackFrame();
}



#define OP(op) \
    { isel_stringIfy(op), op, 0, 0, 0 }
#define OPCONTEXT(op) \
    { isel_stringIfy(op), 0, op, 0, 0 }

#define INLINE_OP(op, memOp, immOp) \
    { isel_stringIfy(op), op, 0, memOp, immOp }
#define INLINE_OPCONTEXT(op, memOp, immOp) \
    { isel_stringIfy(op), 0, op, memOp, immOp }

#define NULL_OP \
    { 0, 0, 0, 0, 0 }

const Assembler::BinaryOperationInfo Assembler::binaryOperations[QQmlJS::V4IR::LastAluOp + 1] = {
    NULL_OP, // OpInvalid
    NULL_OP, // OpIfTrue
    NULL_OP, // OpNot
    NULL_OP, // OpUMinus
    NULL_OP, // OpUPlus
    NULL_OP, // OpCompl
    NULL_OP, // OpIncrement
    NULL_OP, // OpDecrement

    INLINE_OP(__qmljs_bit_and, &Assembler::inline_and32, &Assembler::inline_and32), // OpBitAnd
    INLINE_OP(__qmljs_bit_or, &Assembler::inline_or32, &Assembler::inline_or32), // OpBitOr
    INLINE_OP(__qmljs_bit_xor, &Assembler::inline_xor32, &Assembler::inline_xor32), // OpBitXor

    INLINE_OPCONTEXT(__qmljs_add, &Assembler::inline_add32, &Assembler::inline_add32), // OpAdd
    INLINE_OP(__qmljs_sub, &Assembler::inline_sub32, &Assembler::inline_sub32), // OpSub
    INLINE_OP(__qmljs_mul, &Assembler::inline_mul32, &Assembler::inline_mul32), // OpMul

    OP(__qmljs_div), // OpDiv
    OP(__qmljs_mod), // OpMod

    INLINE_OP(__qmljs_shl, &Assembler::inline_shl32, &Assembler::inline_shl32), // OpLShift
    INLINE_OP(__qmljs_shr, &Assembler::inline_shr32, &Assembler::inline_shr32), // OpRShift
    INLINE_OP(__qmljs_ushr, &Assembler::inline_ushr32, &Assembler::inline_ushr32), // OpURShift

    OP(__qmljs_gt), // OpGt
    OP(__qmljs_lt), // OpLt
    OP(__qmljs_ge), // OpGe
    OP(__qmljs_le), // OpLe
    OP(__qmljs_eq), // OpEqual
    OP(__qmljs_ne), // OpNotEqual
    OP(__qmljs_se), // OpStrictEqual
    OP(__qmljs_sne), // OpStrictNotEqual

    OPCONTEXT(__qmljs_instanceof), // OpInstanceof
    OPCONTEXT(__qmljs_in), // OpIn

    NULL_OP, // OpAnd
    NULL_OP // OpOr
};

#if OS(LINUX) || OS(MAC_OS_X)
static void printDisassembledOutputWithCalls(const char* output, const QHash<void*, const char*>& functions)
{
    QByteArray processedOutput(output);
    for (QHash<void*, const char*>::ConstIterator it = functions.begin(), end = functions.end();
         it != end; ++it) {
        QByteArray ptrString = QByteArray::number(quintptr(it.key()), 16);
        ptrString.prepend("0x");
        processedOutput = processedOutput.replace(ptrString, it.value());
    }
    fprintf(stderr, "%s\n", processedOutput.constData());
}
#endif

void Assembler::recordLineNumber(int lineNumber)
{
    CodeLineNumerMapping mapping;
    mapping.location = label();
    mapping.lineNumber = lineNumber;
    codeLineNumberMappings << mapping;
}


JSC::MacroAssemblerCodeRef Assembler::link()
{
#if defined(Q_PROCESSOR_ARM) && !defined(Q_OS_IOS)
    Label endOfCode = label();
    // Let the ARM exception table follow right after that
    for (int i = 0, nops = UnwindHelper::unwindInfoSize() / 2; i < nops; ++i)
        nop();
#endif

    {
        QHashIterator<V4IR::BasicBlock *, QVector<Jump> > it(_patches);
        while (it.hasNext()) {
            it.next();
            V4IR::BasicBlock *block = it.key();
            Label target = _addrs.value(block);
            assert(target.isSet());
            foreach (Jump jump, it.value())
                jump.linkTo(target, this);
        }
    }

    JSC::JSGlobalData dummy(_executableAllocator);
    JSC::LinkBuffer linkBuffer(dummy, this, 0);

    QVector<uint> lineNumberMapping(codeLineNumberMappings.count() * 2);

    for (int i = 0; i < codeLineNumberMappings.count(); ++i) {
        lineNumberMapping[i * 2] = linkBuffer.offsetOf(codeLineNumberMappings.at(i).location);
        lineNumberMapping[i * 2 + 1] = codeLineNumberMappings.at(i).lineNumber;
    }
    _isel->registerLineNumberMapping(_function, lineNumberMapping);

    QHash<void*, const char*> functions;
    foreach (CallToLink ctl, _callsToLink) {
        linkBuffer.link(ctl.call, ctl.externalFunction);
        functions[ctl.externalFunction.value()] = ctl.functionName;
    }

    foreach (const DataLabelPatch &p, _dataLabelPatches)
        linkBuffer.patch(p.dataLabel, linkBuffer.locationOf(p.target));

    {
        QHashIterator<V4IR::BasicBlock *, QVector<DataLabelPtr> > it(_labelPatches);
        while (it.hasNext()) {
            it.next();
            V4IR::BasicBlock *block = it.key();
            Label target = _addrs.value(block);
            assert(target.isSet());
            foreach (DataLabelPtr label, it.value())
                linkBuffer.patch(label, linkBuffer.locationOf(target));
        }
    }
    _constTable.finalize(linkBuffer, _isel);

#if defined(Q_PROCESSOR_ARM) && !defined(Q_OS_IOS)
    UnwindHelper::writeARMUnwindInfo(linkBuffer.debugAddress(), linkBuffer.offsetOf(endOfCode));
#endif

    JSC::MacroAssemblerCodeRef codeRef;

    static bool showCode = !qgetenv("SHOW_CODE").isNull();
    if (showCode) {
#if OS(LINUX) && !defined(Q_OS_ANDROID)
        char* disasmOutput = 0;
        size_t disasmLength = 0;
        FILE* disasmStream = open_memstream(&disasmOutput, &disasmLength);
        WTF::setDataFile(disasmStream);
#elif OS(MAC_OS_X)
        struct MemStream {
            QByteArray buf;
            static int write(void *cookie, const char *buf, int len) {
                MemStream *stream = reinterpret_cast<MemStream *>(cookie);
                stream->buf.append(buf, len);
                return len;
            }
        };
        MemStream memStream;

        FILE* disasmStream = fwopen(&memStream, MemStream::write);
        WTF::setDataFile(disasmStream);
#endif

        QByteArray name = _function->name->toUtf8();
        if (name.isEmpty()) {
            name = QByteArray::number(quintptr(_function), 16);
            name.prepend("IR::Function(0x");
            name.append(")");
        }
        codeRef = linkBuffer.finalizeCodeWithDisassembly("%s", name.data());

        WTF::setDataFile(stderr);
#if (OS(LINUX) && !defined(Q_OS_ANDROID)) || OS(MAC_OS_X)
        fclose(disasmStream);
#  if OS(MAC_OS_X)
        char *disasmOutput = memStream.buf.data();
#  endif
#  if CPU(X86) || CPU(X86_64)
        QHash<void*, String*> idents;
        printDisassembledOutputWithCalls(disasmOutput, functions);
#  endif
#  if OS(LINUX)
        free(disasmOutput);
#  endif
#endif
    } else {
        codeRef = linkBuffer.finalizeCodeWithoutDisassembly();
    }

    return codeRef;
}

InstructionSelection::InstructionSelection(QV4::ExecutableAllocator *execAllocator, V4IR::Module *module)
    : EvalInstructionSelection(execAllocator, module)
    , _block(0)
    , _function(0)
    , _as(0)
{
    compilationUnit = new CompilationUnit;
}

InstructionSelection::~InstructionSelection()
{
    delete _as;
}

void InstructionSelection::run(V4IR::Function *function)
{
    QVector<Lookup> lookups;
    QSet<V4IR::BasicBlock*> reentryBlocks;
    qSwap(_function, function);
    qSwap(_reentryBlocks, reentryBlocks);

    V4IR::Optimizer opt(_function);
    opt.run();
    if (opt.isInSSA()) {
#if CPU(X86_64) && (OS(MAC_OS_X) || OS(LINUX))
        static const QVector<int> intRegisters = QVector<int>()
                << JSC::X86Registers::edi
                << JSC::X86Registers::esi
                << JSC::X86Registers::edx
                << JSC::X86Registers::r9
                << JSC::X86Registers::r8
                << JSC::X86Registers::r13
                << JSC::X86Registers::r15;
        static const QVector<int> fpRegisters = QVector<int>()
                << JSC::X86Registers::xmm1
                << JSC::X86Registers::xmm2
                << JSC::X86Registers::xmm3
                << JSC::X86Registers::xmm4
                << JSC::X86Registers::xmm5
                << JSC::X86Registers::xmm6
                << JSC::X86Registers::xmm7;
        RegisterAllocator(intRegisters, fpRegisters).run(_function, opt);
#else
        // No register allocator available for this platform, so:
        opt.convertOutOfSSA();
        ConvertTemps().toStackSlots(_function);
#endif
    } else {
        ConvertTemps().toStackSlots(_function);
    }
    V4IR::Optimizer::showMeTheCode(_function);

    Assembler* oldAssembler = _as;
    _as = new Assembler(this, _function, executableAllocator, 6); // 6 == max argc for calls to built-ins with an argument array

    _as->enterStandardStackFrame(/*withLocals*/true);

    int contextPointer = 0;
#if !defined(RETURN_VALUE_IN_REGISTER)
    // When the return VM value doesn't fit into a register, then
    // the caller provides a pointer for storage as first argument.
    // That shifts the index the context pointer argument by one.
    contextPointer++;
#endif

#ifdef ARGUMENTS_IN_REGISTERS
    _as->move(_as->registerForArgument(contextPointer), Assembler::ContextRegister);
#else
    _as->loadPtr(addressForArgument(contextPointer), Assembler::ContextRegister);
#endif

    for (int i = 0, ei = _function->basicBlocks.size(); i != ei; ++i) {
        V4IR::BasicBlock *nextBlock = (i < ei - 1) ? _function->basicBlocks[i + 1] : 0;
        _block = _function->basicBlocks[i];
        _as->registerBlock(_block, nextBlock);

        if (_reentryBlocks.contains(_block)) {
            _as->enterStandardStackFrame(/*locals*/false);
#ifdef ARGUMENTS_IN_REGISTERS
            _as->move(Assembler::registerForArgument(0), Assembler::ContextRegister);
            _as->move(Assembler::registerForArgument(1), Assembler::LocalsRegister);
#else
            _as->loadPtr(addressForArgument(0), Assembler::ContextRegister);
            _as->loadPtr(addressForArgument(1), Assembler::LocalsRegister);
#endif
        }

        foreach (V4IR::Stmt *s, _block->statements) {
            if (s->location.isValid())
                _as->recordLineNumber(s->location.startLine);
            s->accept(this);
        }
    }

    JSC::MacroAssemblerCodeRef codeRef =_as->link();
    codeRefs[_function] = codeRef;

    qSwap(_function, function);
    qSwap(_reentryBlocks, reentryBlocks);
    delete _as;
    _as = oldAssembler;
}

void *InstructionSelection::addConstantTable(QVector<Value> *values)
{
    compilationUnit->constantValues.append(*values);
    values->clear();

    QVector<QV4::Value> &finalValues = compilationUnit->constantValues.last();
    finalValues.squeeze();
    return finalValues.data();
}

QV4::CompiledData::CompilationUnit *InstructionSelection::backendCompileStep()
{
    compilationUnit->data = generateUnit();
    compilationUnit->codeRefs.resize(irModule->functions.size());
    int i = 0;
    foreach (V4IR::Function *irFunction, irModule->functions)
        compilationUnit->codeRefs[i++] = codeRefs[irFunction];
    return compilationUnit;
}

void InstructionSelection::callBuiltinInvalid(V4IR::Name *func, V4IR::ExprList *args, V4IR::Temp *result)
{
    int argc = prepareVariableArguments(args);

    if (useFastLookups && func->global) {
        uint index = registerGlobalGetterLookup(*func->id);
        generateFunctionCall(Assembler::Void, __qmljs_call_global_lookup,
                             Assembler::ContextRegister, Assembler::PointerToValue(result),
                             Assembler::TrustedImm32(index),
                             baseAddressForCallArguments(),
                             Assembler::TrustedImm32(argc));
    } else {
        generateFunctionCall(Assembler::Void, __qmljs_call_activation_property,
                             Assembler::ContextRegister, Assembler::PointerToValue(result),
                             Assembler::PointerToString(*func->id),
                             baseAddressForCallArguments(),
                             Assembler::TrustedImm32(argc));
    }
}

void InstructionSelection::callBuiltinTypeofMember(V4IR::Expr *base, const QString &name,
                                                   V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_typeof_member, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::PointerToValue(base),
                         Assembler::PointerToString(name));
}

void InstructionSelection::callBuiltinTypeofSubscript(V4IR::Expr *base, V4IR::Expr *index,
                                                      V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_typeof_element,
                         Assembler::ContextRegister, Assembler::PointerToValue(result),
                         Assembler::PointerToValue(base), Assembler::PointerToValue(index));
}

void InstructionSelection::callBuiltinTypeofName(const QString &name, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_typeof_name, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::PointerToString(name));
}

void InstructionSelection::callBuiltinTypeofValue(V4IR::Expr *value, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_typeof, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::PointerToValue(value));
}

void InstructionSelection::callBuiltinDeleteMember(V4IR::Temp *base, const QString &name, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_delete_member, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::Reference(base),
                         Assembler::PointerToString(name));
}

void InstructionSelection::callBuiltinDeleteSubscript(V4IR::Temp *base, V4IR::Expr *index,
                                                      V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_delete_subscript, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::Reference(base),
                         Assembler::PointerToValue(index));
}

void InstructionSelection::callBuiltinDeleteName(const QString &name, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_delete_name, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::PointerToString(name));
}

void InstructionSelection::callBuiltinDeleteValue(V4IR::Temp *result)
{
    _as->storeValue(Value::fromBoolean(false), result);
}

void InstructionSelection::callBuiltinPostIncrementMember(V4IR::Temp *base, const QString &name, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_post_increment_member,
                         Assembler::ContextRegister, Assembler::PointerToValue(result),
                         Assembler::PointerToValue(base), Assembler::PointerToString(name));
}

void InstructionSelection::callBuiltinPostIncrementSubscript(V4IR::Temp *base, V4IR::Temp *index, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_post_increment_element,
                         Assembler::ContextRegister, Assembler::PointerToValue(result),
                         Assembler::Reference(base), Assembler::PointerToValue(index));
}

void InstructionSelection::callBuiltinPostIncrementName(const QString &name, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_post_increment_name, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::PointerToString(name));
}

void InstructionSelection::callBuiltinPostIncrementValue(V4IR::Temp *value, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_post_increment,
                         Assembler::PointerToValue(result), Assembler::PointerToValue(value));
}

void InstructionSelection::callBuiltinPostDecrementMember(V4IR::Temp *base, const QString &name, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_post_decrement_member, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::Reference(base), Assembler::PointerToString(name));
}

void InstructionSelection::callBuiltinPostDecrementSubscript(V4IR::Temp *base, V4IR::Temp *index, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_post_decrement_element, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::Reference(base),
                         Assembler::Reference(index));
}

void InstructionSelection::callBuiltinPostDecrementName(const QString &name, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_post_decrement_name, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::PointerToString(name));
}

void InstructionSelection::callBuiltinPostDecrementValue(V4IR::Temp *value, V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_post_decrement,
                         Assembler::PointerToValue(result), Assembler::PointerToValue(value));
}

void InstructionSelection::callBuiltinThrow(V4IR::Expr *arg)
{
    generateFunctionCall(Assembler::Void, __qmljs_throw, Assembler::ContextRegister,
                         Assembler::PointerToValue(arg));
}

typedef void *(*MiddleOfFunctionEntryPoint(ExecutionContext *, void *localsPtr));
static void *tryWrapper(ExecutionContext *context, void *localsPtr, MiddleOfFunctionEntryPoint tryBody, MiddleOfFunctionEntryPoint catchBody,
                        QV4::String *exceptionVarName, Value *exceptionVar)
{
    *exceptionVar = Value::undefinedValue();
    void *addressToContinueAt = 0;
    try {
        addressToContinueAt = tryBody(context, localsPtr);
    } catch (Exception& ex) {
        ex.accept(context);
        *exceptionVar = ex.value();
        try {
            ExecutionContext *catchContext = __qmljs_builtin_push_catch_scope(exceptionVarName, ex.value(), context);
            addressToContinueAt = catchBody(catchContext, localsPtr);
            context = __qmljs_builtin_pop_scope(catchContext);
        } catch (Exception& ex) {
            *exceptionVar = ex.value();
            ex.accept(context);
            addressToContinueAt = catchBody(context, localsPtr);
        }
    }
    return addressToContinueAt;
}

void InstructionSelection::visitTry(V4IR::Try *t)
{
    // Call tryWrapper, which is going to re-enter the same function at the address of the try block. At then end
    // of the try function the JIT code will return with the address of the sub-sequent instruction, which tryWrapper
    // returns and to which we jump to.

    _reentryBlocks.insert(t->tryBlock);
    _reentryBlocks.insert(t->catchBlock);

    generateFunctionCall(Assembler::ReturnValueRegister, tryWrapper, Assembler::ContextRegister, Assembler::LocalsRegister,
                         Assembler::ReentryBlock(t->tryBlock), Assembler::ReentryBlock(t->catchBlock),
                         Assembler::PointerToString(*t->exceptionVarName), Assembler::PointerToValue(t->exceptionVar));
    _as->jump(Assembler::ReturnValueRegister);
}

void InstructionSelection::callBuiltinFinishTry()
{
    // This assumes that we're in code that was called by tryWrapper, so we return to try wrapper
    // with the address that we'd like to continue at, which is right after the ret below.
    Assembler::DataLabelPtr continuation = _as->moveWithPatch(Assembler::TrustedImmPtr(0), Assembler::ReturnValueRegister);
    _as->leaveStandardStackFrame(/*locals*/false);
    _as->ret();
    _as->addPatch(continuation, _as->label());
}

void InstructionSelection::callBuiltinForeachIteratorObject(V4IR::Temp *arg, V4IR::Temp *result)
{
    Q_ASSERT(arg);
    Q_ASSERT(result);

    generateFunctionCall(Assembler::Void, __qmljs_foreach_iterator_object, Assembler::ContextRegister, Assembler::PointerToValue(result), Assembler::Reference(arg));
}

void InstructionSelection::callBuiltinForeachNextPropertyname(V4IR::Temp *arg, V4IR::Temp *result)
{
    Q_ASSERT(arg);
    Q_ASSERT(result);

    generateFunctionCall(Assembler::Void, __qmljs_foreach_next_property_name, Assembler::PointerToValue(result), Assembler::Reference(arg));
}

void InstructionSelection::callBuiltinPushWithScope(V4IR::Temp *arg)
{
    Q_ASSERT(arg);

    generateFunctionCall(Assembler::ContextRegister, __qmljs_builtin_push_with_scope, Assembler::Reference(arg), Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinPopScope()
{
    generateFunctionCall(Assembler::ContextRegister, __qmljs_builtin_pop_scope, Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinDeclareVar(bool deletable, const QString &name)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_declare_var, Assembler::ContextRegister,
                         Assembler::TrustedImm32(deletable), Assembler::PointerToString(name));
}

void InstructionSelection::callBuiltinDefineGetterSetter(V4IR::Temp *object, const QString &name, V4IR::Temp *getter, V4IR::Temp *setter)
{
    Q_ASSERT(object);
    Q_ASSERT(getter);
    Q_ASSERT(setter);
    generateFunctionCall(Assembler::Void, __qmljs_builtin_define_getter_setter, Assembler::ContextRegister,
                         Assembler::Reference(object), Assembler::PointerToString(name), Assembler::PointerToValue(getter), Assembler::PointerToValue(setter));
}

void InstructionSelection::callBuiltinDefineProperty(V4IR::Temp *object, const QString &name,
                                                     V4IR::Expr *value)
{
    Q_ASSERT(object);
    Q_ASSERT(value->asTemp() || value->asConst());

    generateFunctionCall(Assembler::Void, __qmljs_builtin_define_property,
                         Assembler::ContextRegister, Assembler::Reference(object), Assembler::PointerToString(name),
                         Assembler::PointerToValue(value));
}

void InstructionSelection::callBuiltinDefineArray(V4IR::Temp *result, V4IR::ExprList *args)
{
    Q_ASSERT(result);

    int length = prepareVariableArguments(args);
    generateFunctionCall(Assembler::Void, __qmljs_builtin_define_array, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), baseAddressForCallArguments(),
                         Assembler::TrustedImm32(length));
}

void InstructionSelection::callBuiltinDefineObjectLiteral(V4IR::Temp *result, V4IR::ExprList *args)
{
    Q_ASSERT(result);

    int argc = 0;

    const int classId = registerJSClass(args);

    V4IR::ExprList *it = args;
    while (it) {
        it = it->next;

        bool isData = it->expr->asConst()->value;
        it = it->next;

        _as->copyValue(_as->stackLayout().argumentAddressForCall(argc++), it->expr);

        if (!isData) {
            it = it->next;
            _as->copyValue(_as->stackLayout().argumentAddressForCall(argc++), it->expr);
        }

        it = it->next;
    }

    generateFunctionCall(Assembler::Void, __qmljs_builtin_define_object_literal, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), baseAddressForCallArguments(),
                         Assembler::TrustedImm32(classId));
}

void InstructionSelection::callBuiltinSetupArgumentObject(V4IR::Temp *result)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_setup_arguments_object, Assembler::ContextRegister,
                         Assembler::PointerToValue(result));
}

void InstructionSelection::callValue(V4IR::Temp *value, V4IR::ExprList *args, V4IR::Temp *result)
{
    Q_ASSERT(value);

    int argc = prepareVariableArguments(args);
    V4IR::Temp* thisObject = 0;
    generateFunctionCall(Assembler::Void, __qmljs_call_value, Assembler::ContextRegister,
            Assembler::PointerToValue(result), Assembler::PointerToValue(thisObject),
            Assembler::Reference(value), baseAddressForCallArguments(), Assembler::TrustedImm32(argc));
}

void InstructionSelection::loadThisObject(V4IR::Temp *temp)
{
#if defined(VALUE_FITS_IN_REGISTER)
    _as->load64(Pointer(Assembler::ContextRegister, offsetof(ExecutionContext, thisObject)),
                Assembler::ReturnValueRegister);
    _as->storeReturnValue(temp);
#else
    _as->copyValue(temp, Pointer(Assembler::ContextRegister, offsetof(ExecutionContext, thisObject)));
#endif
}

void InstructionSelection::loadConst(V4IR::Const *sourceConst, V4IR::Temp *targetTemp)
{
    if (targetTemp->kind == V4IR::Temp::PhysicalRegister) {
        if (targetTemp->type == V4IR::DoubleType) {
            Q_ASSERT(sourceConst->type == V4IR::DoubleType);
            _as->toDoubleRegister(sourceConst, (Assembler::FPRegisterID) targetTemp->index);
        } else if (targetTemp->type == V4IR::SInt32Type) {
            Q_ASSERT(sourceConst->type == V4IR::SInt32Type);
            _as->toInt32Register(sourceConst, (Assembler::RegisterID) targetTemp->index);
        } else if (targetTemp->type == V4IR::UInt32Type) {
            Q_ASSERT(sourceConst->type == V4IR::UInt32Type);
            _as->toUInt32Register(sourceConst, (Assembler::RegisterID) targetTemp->index);
        } else if (targetTemp->type == V4IR::BoolType) {
            Q_ASSERT(sourceConst->type == V4IR::BoolType);
            _as->move(Assembler::TrustedImm32(convertToValue(sourceConst).int_32),
                      (Assembler::RegisterID) targetTemp->index);
        } else {
            Q_UNIMPLEMENTED();
        }
    } else {
        _as->storeValue(convertToValue(sourceConst), targetTemp);
    }
}

void InstructionSelection::loadString(const QString &str, V4IR::Temp *targetTemp)
{
    generateFunctionCall(Assembler::Void, __qmljs_value_from_string, Assembler::PointerToValue(targetTemp), Assembler::PointerToString(str));
}

void InstructionSelection::loadRegexp(V4IR::RegExp *sourceRegexp, V4IR::Temp *targetTemp)
{
    int id = registerRegExp(sourceRegexp);
    generateFunctionCall(Assembler::Void, __qmljs_lookup_runtime_regexp, Assembler::ContextRegister, Assembler::PointerToValue(targetTemp), Assembler::TrustedImm32(id));
}

void InstructionSelection::getActivationProperty(const V4IR::Name *name, V4IR::Temp *temp)
{
    if (useFastLookups && name->global) {
        uint index = registerGlobalGetterLookup(*name->id);
        generateLookupCall(index, offsetof(QV4::Lookup, globalGetter), Assembler::ContextRegister, Assembler::PointerToValue(temp));
        return;
    }
    generateFunctionCall(Assembler::Void, __qmljs_get_activation_property, Assembler::ContextRegister, Assembler::PointerToValue(temp), Assembler::PointerToString(*name->id));
}

void InstructionSelection::setActivationProperty(V4IR::Expr *source, const QString &targetName)
{
    generateFunctionCall(Assembler::Void, __qmljs_set_activation_property,
                         Assembler::ContextRegister, Assembler::PointerToString(targetName), Assembler::PointerToValue(source));
}

void InstructionSelection::initClosure(V4IR::Closure *closure, V4IR::Temp *target)
{
    int id = irModule->functions.indexOf(closure->value);
    generateFunctionCall(Assembler::Void, __qmljs_init_closure, Assembler::ContextRegister, Assembler::PointerToValue(target), Assembler::TrustedImm32(id));
}

void InstructionSelection::getProperty(V4IR::Expr *base, const QString &name, V4IR::Temp *target)
{
    if (useFastLookups) {
        uint index = registerGetterLookup(name);
        generateLookupCall(index, offsetof(QV4::Lookup, getter), Assembler::PointerToValue(target),
                           Assembler::PointerToValue(base));
    } else {
        generateFunctionCall(Assembler::Void, __qmljs_get_property, Assembler::ContextRegister,
                             Assembler::PointerToValue(target), Assembler::PointerToValue(base),
                             Assembler::PointerToString(name));
    }
}

void InstructionSelection::setProperty(V4IR::Expr *source, V4IR::Expr *targetBase,
                                       const QString &targetName)
{
    if (useFastLookups) {
        uint index = registerSetterLookup(targetName);
        generateLookupCall(index, offsetof(QV4::Lookup, setter),
                           Assembler::PointerToValue(targetBase),
                           Assembler::PointerToValue(source));
    } else {
        generateFunctionCall(Assembler::Void, __qmljs_set_property, Assembler::ContextRegister,
                             Assembler::PointerToValue(targetBase), Assembler::PointerToString(targetName),
                             Assembler::PointerToValue(source));
    }
}

void InstructionSelection::getElement(V4IR::Expr *base, V4IR::Expr *index, V4IR::Temp *target)
{
    generateFunctionCall(Assembler::Void, __qmljs_get_element, Assembler::ContextRegister,
                         Assembler::PointerToValue(target), Assembler::PointerToValue(base),
                         Assembler::PointerToValue(index));
}

void InstructionSelection::setElement(V4IR::Expr *source, V4IR::Expr *targetBase, V4IR::Expr *targetIndex)
{
    generateFunctionCall(Assembler::Void, __qmljs_set_element, Assembler::ContextRegister,
                         Assembler::PointerToValue(targetBase), Assembler::PointerToValue(targetIndex),
                         Assembler::PointerToValue(source));
}

void InstructionSelection::copyValue(V4IR::Temp *sourceTemp, V4IR::Temp *targetTemp)
{
    if (*sourceTemp == *targetTemp)
        return;

    if (sourceTemp->kind == V4IR::Temp::PhysicalRegister) {
        if (targetTemp->kind == V4IR::Temp::PhysicalRegister) {
            if (sourceTemp->type == V4IR::DoubleType)
                _as->moveDouble((Assembler::FPRegisterID) sourceTemp->index,
                                (Assembler::FPRegisterID) targetTemp->index);
            else
                _as->move((Assembler::RegisterID) sourceTemp->index,
                          (Assembler::RegisterID) targetTemp->index);
            return;
        } else {
            Assembler::Pointer addr = _as->loadTempAddress(Assembler::ScratchRegister, targetTemp);
            switch (sourceTemp->type) {
            case V4IR::DoubleType:
                _as->storeDouble((Assembler::FPRegisterID) sourceTemp->index, addr);
                break;
            case V4IR::SInt32Type:
                _as->storeInt32((Assembler::RegisterID) sourceTemp->index, addr);
                break;
            case V4IR::UInt32Type:
                _as->storeUInt32((Assembler::RegisterID) sourceTemp->index, addr);
                break;
            case V4IR::BoolType:
                _as->storeBool((Assembler::RegisterID) sourceTemp->index, addr);
                break;
            default:
                Q_ASSERT(!"Unreachable");
                break;
            }
            return;
        }
    } else if (targetTemp->kind == V4IR::Temp::PhysicalRegister) {
        switch (targetTemp->type) {
        case V4IR::DoubleType:
            Q_ASSERT(sourceTemp->type == V4IR::DoubleType);
            _as->toDoubleRegister(sourceTemp, (Assembler::FPRegisterID) targetTemp->index);
            return;
        case V4IR::BoolType:
            Q_ASSERT(sourceTemp->type == V4IR::BoolType);
            _as->toInt32Register(sourceTemp, (Assembler::RegisterID) targetTemp->index);
            return;
        case V4IR::SInt32Type:
            Q_ASSERT(sourceTemp->type == V4IR::SInt32Type);
            _as->toInt32Register(sourceTemp, (Assembler::RegisterID) targetTemp->index);
            return;
        case V4IR::UInt32Type:
            Q_ASSERT(sourceTemp->type == V4IR::UInt32Type);
            _as->toUInt32Register(sourceTemp, (Assembler::RegisterID) targetTemp->index);
            return;
        default:
            Q_ASSERT(!"Unreachable");
            break;
        }
    }

    _as->copyValue(targetTemp, sourceTemp);
}

void InstructionSelection::swapValues(V4IR::Temp *sourceTemp, V4IR::Temp *targetTemp)
{
    Q_ASSERT(sourceTemp->type == targetTemp->type);

    if (sourceTemp->kind == V4IR::Temp::PhysicalRegister) {
        if (targetTemp->kind == V4IR::Temp::PhysicalRegister) {
            if (sourceTemp->type == V4IR::DoubleType) {
                _as->moveDouble((Assembler::FPRegisterID) targetTemp->index, Assembler::FPGpr0);
                _as->moveDouble((Assembler::FPRegisterID) sourceTemp->index,
                                (Assembler::FPRegisterID) targetTemp->index);
                _as->moveDouble(Assembler::FPGpr0, (Assembler::FPRegisterID) sourceTemp->index);
            } else {
                _as->swap((Assembler::RegisterID) sourceTemp->index,
                          (Assembler::RegisterID) targetTemp->index);
            }
            return;
        }
    } else if (sourceTemp->kind == V4IR::Temp::StackSlot) {
        if (targetTemp->kind == V4IR::Temp::StackSlot) {
            Assembler::FPRegisterID tReg = _as->toDoubleRegister(targetTemp);
#if CPU(X86_64)
            _as->load64(_as->stackSlotPointer(sourceTemp), Assembler::ScratchRegister);
            _as->store64(Assembler::ScratchRegister, _as->stackSlotPointer(targetTemp));
#else
            Assembler::Pointer sAddr = _as->stackSlotPointer(sourceTemp);
            Assembler::Pointer tAddr = _as->stackSlotPointer(targetTemp);
            _as->load32(sAddr, Assembler::ScratchRegister);
            _as->store32(Assembler::ScratchRegister, tAddr);
            sAddr.offset += 4;
            tAddr.offset += 4;
            _as->load32(sAddr, Assembler::ScratchRegister);
            _as->store32(Assembler::ScratchRegister, tAddr);
#endif
            _as->storeDouble(tReg, _as->stackSlotPointer(sourceTemp));
            return;
        }
    }

    // FIXME: TODO!
    Q_UNIMPLEMENTED();
}

#define setOp(op, opName, operation) \
    do { op = operation; opName = isel_stringIfy(operation); } while (0)
#define setOpContext(op, opName, operation) \
    do { opContext = operation; opName = isel_stringIfy(operation); } while (0)

void InstructionSelection::unop(V4IR::AluOp oper, V4IR::Temp *sourceTemp, V4IR::Temp *targetTemp)
{
    UnaryOpName op = 0;
    const char *opName = 0;
    switch (oper) {
    case V4IR::OpIfTrue: assert(!"unreachable"); break;
    case V4IR::OpNot: setOp(op, opName, __qmljs_not); break;
    case V4IR::OpUMinus: setOp(op, opName, __qmljs_uminus); break;
    case V4IR::OpUPlus: setOp(op, opName, __qmljs_uplus); break;
    case V4IR::OpCompl: setOp(op, opName, __qmljs_compl); break;
    case V4IR::OpIncrement: setOp(op, opName, __qmljs_increment); break;
    case V4IR::OpDecrement: setOp(op, opName, __qmljs_decrement); break;
    default: assert(!"unreachable"); break;
    } // switch

    if (op) {
        _as->generateFunctionCallImp(Assembler::Void, opName, op,
                                     Assembler::PointerToValue(targetTemp),
                                     Assembler::PointerToValue(sourceTemp));
        storeTarget(0, targetTemp);
    }
}

void InstructionSelection::binop(V4IR::AluOp oper, V4IR::Expr *leftSource, V4IR::Expr *rightSource, V4IR::Temp *target)
{
    const Assembler::BinaryOperationInfo& info = Assembler::binaryOperation(oper);
    if (info.fallbackImplementation) {
        _as->generateFunctionCallImp(Assembler::Void, info.name, info.fallbackImplementation,
                                     Assembler::PointerToValue(target),
                                     Assembler::PointerToValue(leftSource),
                                     Assembler::PointerToValue(rightSource));
        storeTarget(0, target);
    } else if (info.contextImplementation) {
        _as->generateFunctionCallImp(Assembler::Void, info.name, info.contextImplementation,
                                     Assembler::ContextRegister,
                                     Assembler::PointerToValue(target),
                                     Assembler::PointerToValue(leftSource),
                                     Assembler::PointerToValue(rightSource));
        storeTarget(1, target);
    } else {
        assert(!"unreachable");
    }
}

void InstructionSelection::inplaceNameOp(V4IR::AluOp oper, V4IR::Temp *rightSource, const QString &targetName)
{
    InplaceBinOpName op = 0;
    const char *opName = 0;
    switch (oper) {
    case V4IR::OpBitAnd: setOp(op, opName, __qmljs_inplace_bit_and_name); break;
    case V4IR::OpBitOr: setOp(op, opName, __qmljs_inplace_bit_or_name); break;
    case V4IR::OpBitXor: setOp(op, opName, __qmljs_inplace_bit_xor_name); break;
    case V4IR::OpAdd: setOp(op, opName, __qmljs_inplace_add_name); break;
    case V4IR::OpSub: setOp(op, opName, __qmljs_inplace_sub_name); break;
    case V4IR::OpMul: setOp(op, opName, __qmljs_inplace_mul_name); break;
    case V4IR::OpDiv: setOp(op, opName, __qmljs_inplace_div_name); break;
    case V4IR::OpMod: setOp(op, opName, __qmljs_inplace_mod_name); break;
    case V4IR::OpLShift: setOp(op, opName, __qmljs_inplace_shl_name); break;
    case V4IR::OpRShift: setOp(op, opName, __qmljs_inplace_shr_name); break;
    case V4IR::OpURShift: setOp(op, opName, __qmljs_inplace_ushr_name); break;
    default:
        Q_UNREACHABLE();
        break;
    }
    if (op) {
        _as->generateFunctionCallImp(Assembler::Void, opName, op, Assembler::ContextRegister,
                                     Assembler::PointerToString(targetName), Assembler::Reference(rightSource));
    }
}

void InstructionSelection::inplaceElementOp(V4IR::AluOp oper, V4IR::Temp *source, V4IR::Temp *targetBaseTemp, V4IR::Temp *targetIndexTemp)
{
    InplaceBinOpElement op = 0;
    const char *opName = 0;
    switch (oper) {
    case V4IR::OpBitAnd: setOp(op, opName, __qmljs_inplace_bit_and_element); break;
    case V4IR::OpBitOr: setOp(op, opName, __qmljs_inplace_bit_or_element); break;
    case V4IR::OpBitXor: setOp(op, opName, __qmljs_inplace_bit_xor_element); break;
    case V4IR::OpAdd: setOp(op, opName, __qmljs_inplace_add_element); break;
    case V4IR::OpSub: setOp(op, opName, __qmljs_inplace_sub_element); break;
    case V4IR::OpMul: setOp(op, opName, __qmljs_inplace_mul_element); break;
    case V4IR::OpDiv: setOp(op, opName, __qmljs_inplace_div_element); break;
    case V4IR::OpMod: setOp(op, opName, __qmljs_inplace_mod_element); break;
    case V4IR::OpLShift: setOp(op, opName, __qmljs_inplace_shl_element); break;
    case V4IR::OpRShift: setOp(op, opName, __qmljs_inplace_shr_element); break;
    case V4IR::OpURShift: setOp(op, opName, __qmljs_inplace_ushr_element); break;
    default:
        Q_UNREACHABLE();
        break;
    }

    if (op) {
        _as->generateFunctionCallImp(Assembler::Void, opName, op, Assembler::ContextRegister,
                                     Assembler::Reference(targetBaseTemp), Assembler::Reference(targetIndexTemp),
                                     Assembler::Reference(source));
    }
}

void InstructionSelection::inplaceMemberOp(V4IR::AluOp oper, V4IR::Temp *source, V4IR::Temp *targetBase, const QString &targetName)
{
    InplaceBinOpMember op = 0;
    const char *opName = 0;
    switch (oper) {
    case V4IR::OpBitAnd: setOp(op, opName, __qmljs_inplace_bit_and_member); break;
    case V4IR::OpBitOr: setOp(op, opName, __qmljs_inplace_bit_or_member); break;
    case V4IR::OpBitXor: setOp(op, opName, __qmljs_inplace_bit_xor_member); break;
    case V4IR::OpAdd: setOp(op, opName, __qmljs_inplace_add_member); break;
    case V4IR::OpSub: setOp(op, opName, __qmljs_inplace_sub_member); break;
    case V4IR::OpMul: setOp(op, opName, __qmljs_inplace_mul_member); break;
    case V4IR::OpDiv: setOp(op, opName, __qmljs_inplace_div_member); break;
    case V4IR::OpMod: setOp(op, opName, __qmljs_inplace_mod_member); break;
    case V4IR::OpLShift: setOp(op, opName, __qmljs_inplace_shl_member); break;
    case V4IR::OpRShift: setOp(op, opName, __qmljs_inplace_shr_member); break;
    case V4IR::OpURShift: setOp(op, opName, __qmljs_inplace_ushr_member); break;
    default:
        Q_UNREACHABLE();
        break;
    }

    if (op) {
        _as->generateFunctionCallImp(Assembler::Void, opName, op, Assembler::ContextRegister,
                                     Assembler::Reference(targetBase), Assembler::PointerToString(targetName),
                                     Assembler::Reference(source));
    }
}

void InstructionSelection::callProperty(V4IR::Expr *base, const QString &name, V4IR::ExprList *args,
                                        V4IR::Temp *result)
{
    assert(base != 0);

    int argc = prepareVariableArguments(args);

    if (useFastLookups) {
        uint index = registerGetterLookup(name);
        generateFunctionCall(Assembler::Void, __qmljs_call_property_lookup,
                             Assembler::ContextRegister, Assembler::PointerToValue(result),
                             Assembler::PointerToValue(base), Assembler::TrustedImm32(index),
                             baseAddressForCallArguments(),
                             Assembler::TrustedImm32(argc));
    } else {
        generateFunctionCall(Assembler::Void, __qmljs_call_property, Assembler::ContextRegister,
                             Assembler::PointerToValue(result), Assembler::PointerToValue(base), Assembler::PointerToString(name),
                             baseAddressForCallArguments(), Assembler::TrustedImm32(argc));
    }
}

void InstructionSelection::callSubscript(V4IR::Expr *base, V4IR::Expr *index, V4IR::ExprList *args,
                                         V4IR::Temp *result)
{
    assert(base != 0);

    int argc = prepareVariableArguments(args);
    generateFunctionCall(Assembler::Void, __qmljs_call_element, Assembler::ContextRegister,
                         Assembler::PointerToValue(result), Assembler::PointerToValue(base),
                         Assembler::PointerToValue(index), baseAddressForCallArguments(),
                         Assembler::TrustedImm32(argc));
}

void InstructionSelection::convertType(V4IR::Temp *source, V4IR::Temp *target)
{
    switch (target->type) {
    case V4IR::DoubleType:
        convertTypeToDouble(source, target);
        break;
    case V4IR::BoolType:
        convertTypeToBool(source, target);
        break;
    case V4IR::SInt32Type:
        convertTypeToSInt32(source, target);
        break;
    default:
        convertTypeSlowPath(source, target);
        break;
    }
}

void InstructionSelection::convertTypeSlowPath(V4IR::Temp *source, V4IR::Temp *target)
{
    Q_ASSERT(target->type != V4IR::BoolType);

    if (target->type & V4IR::NumberType)
        unop(V4IR::OpUPlus, source, target);
    else
        copyValue(source, target);
}

void InstructionSelection::convertTypeToDouble(V4IR::Temp *source, V4IR::Temp *target)
{
    switch (source->type) {
    case V4IR::SInt32Type:
    case V4IR::BoolType:
    case V4IR::NullType:
        convertIntToDouble(source, target);
        break;
    case V4IR::UInt32Type:
        convertUIntToDouble(source, target);
        break;
    case V4IR::UndefinedType:
    case V4IR::StringType:
    case V4IR::ObjectType: {
        // load the tag:
        Assembler::Pointer tagAddr = _as->loadTempAddress(Assembler::ScratchRegister, source);
        tagAddr.offset += 4;
        _as->load32(tagAddr, Assembler::ScratchRegister);

        // check if it's an int32:
        Assembler::Jump isNoInt = _as->branch32(Assembler::NotEqual, Assembler::ScratchRegister,
                                                Assembler::TrustedImm32(Value::_Integer_Type));
        convertIntToDouble(source, target);
        Assembler::Jump intDone = _as->jump();

        // not an int, check if it's NOT a double:
        isNoInt.link(_as);
        _as->and32(Assembler::TrustedImm32(Value::NotDouble_Mask), Assembler::ScratchRegister);
        Assembler::Jump isDbl = _as->branch32(Assembler::NotEqual, Assembler::ScratchRegister,
                                              Assembler::TrustedImm32(Value::NotDouble_Mask));

        generateFunctionCall(Assembler::Void, __qmljs_value_to_double,
                             Assembler::PointerToValue(target),
                             Assembler::PointerToValue(source));
        storeTarget(0, target);
        Assembler::Jump noDoubleDone = _as->jump();

        // it is a double:
        isDbl.link(_as);
        Assembler::Pointer addr2 = _as->loadTempAddress(Assembler::ScratchRegister, source);
        if (target->kind == V4IR::Temp::StackSlot) {
            _as->loadDouble(addr2, Assembler::FPGpr0);
            _as->storeDouble(Assembler::FPGpr0, _as->stackSlotPointer(target));
        } else {
            _as->loadDouble(addr2, (Assembler::FPRegisterID) target->index);
        }

        noDoubleDone.link(_as);
        intDone.link(_as);
    } break;
    default:
        convertTypeSlowPath(source, target);
        break;
    }
}

void InstructionSelection::convertTypeToBool(V4IR::Temp *source, V4IR::Temp *target)
{
    switch (source->type) {
    case V4IR::SInt32Type:
    case V4IR::UInt32Type:
        convertIntToBool(source, target);
        break;
    case V4IR::DoubleType: {
        // The source is in a register if the register allocator is used. If the register
        // allocator was not used, then that means that we can use any register for to
        // load the double into.
        Assembler::FPRegisterID reg;
        if (source->kind == V4IR::Temp::PhysicalRegister)
            reg = (Assembler::FPRegisterID) source->index;
        else
            reg = _as->toDoubleRegister(source, (Assembler::FPRegisterID) 1);
        Assembler::Jump nonZero = _as->branchDoubleNonZero(reg, Assembler::FPGpr0);

        // it's 0, so false:
        _as->storeBool(false, target);
        Assembler::Jump done = _as->jump();

        // it's non-zero, so true:
        nonZero.link(_as);
        _as->storeBool(true, target);

        // done:
        done.link(_as);
    } break;
    case V4IR::UndefinedType:
    case V4IR::NullType:
        _as->storeBool(false, target);
        break;
    case V4IR::StringType:
    case V4IR::ObjectType:
    default:
        generateFunctionCall(Assembler::ReturnValueRegister, __qmljs_to_boolean,
                             Assembler::PointerToValue(source));
        _as->storeBool(Assembler::ReturnValueRegister, target);
        break;
    }
}

void InstructionSelection::convertTypeToSInt32(V4IR::Temp *source, V4IR::Temp *target)
{
    switch (source->type) {
    case V4IR::ObjectType: {
        // load the tag:
        Assembler::Pointer tagAddr = _as->loadTempAddress(Assembler::ScratchRegister, source);
        tagAddr.offset += 4;
        _as->load32(tagAddr, Assembler::ScratchRegister);

        // check if it's an int32:
        Assembler::Jump isNoInt = _as->branch32(Assembler::NotEqual, Assembler::ScratchRegister,
                                                Assembler::TrustedImm32(Value::_Integer_Type));
        Assembler::Pointer addr = _as->loadTempAddress(Assembler::ScratchRegister, source);
        if (target->kind == V4IR::Temp::StackSlot) {
            _as->load32(addr, Assembler::ScratchRegister);
            Assembler::Pointer targetAddr = _as->stackSlotPointer(target);
            _as->store32(Assembler::ScratchRegister, targetAddr);
            targetAddr.offset += 4;
            _as->store32(Assembler::TrustedImm32(Value::_Integer_Type), targetAddr);
        } else {
            _as->load32(addr, (Assembler::RegisterID) target->index);
        }
        Assembler::Jump intDone = _as->jump();

        // not an int:
        isNoInt.link(_as);
        generateFunctionCall(Assembler::ReturnValueRegister, __qmljs_value_to_int32,
                             _as->loadTempAddress(Assembler::ScratchRegister, source));
        _as->storeInt32(Assembler::ReturnValueRegister, target);

        intDone.link(_as);
    } break;
    case V4IR::DoubleType: {
        Assembler::FPRegisterID reg = _as->toDoubleRegister(source);
        Assembler::Jump success =
                _as->branchTruncateDoubleToInt32(reg, Assembler::ReturnValueRegister,
                                                 Assembler::BranchIfTruncateSuccessful);
        generateFunctionCall(Assembler::ReturnValueRegister, __qmljs_double_to_int32,
                             Assembler::PointerToValue(source));
        success.link(_as);
        _as->storeInt32(Assembler::ReturnValueRegister, target);
    } break;
    case V4IR::UInt32Type: {
        Assembler::RegisterID reg = _as->toUInt32Register(source, Assembler::ReturnValueRegister);
        Assembler::Jump easy = _as->branch32(Assembler::GreaterThanOrEqual, reg, Assembler::TrustedImm32(0));
        generateFunctionCall(Assembler::ReturnValueRegister, __qmljs_value_to_int32,
                             Assembler::PointerToValue(source));
        easy.link(_as);
        _as->storeInt32(Assembler::ReturnValueRegister, target);
    } break;
    case V4IR::NullType:
    case V4IR::UndefinedType:
        _as->move(Assembler::TrustedImm32(0), Assembler::ReturnValueRegister);
        _as->storeInt32(Assembler::ReturnValueRegister, target);
        break;
    case V4IR::StringType:
        generateFunctionCall(Assembler::ReturnValueRegister, __qmljs_value_to_int32,
                             _as->loadTempAddress(Assembler::ScratchRegister, source));
        _as->storeInt32(Assembler::ReturnValueRegister, target);
        break;
    case V4IR::BoolType:
        _as->storeInt32(_as->toInt32Register(source, Assembler::ReturnValueRegister), target);
        break;
    default:
        break;
    } // switch (source->type)
}

void InstructionSelection::constructActivationProperty(V4IR::Name *func, V4IR::ExprList *args, V4IR::Temp *result)
{
    assert(func != 0);

    if (useFastLookups && func->global) {
        int argc = prepareVariableArguments(args);
        uint index = registerGlobalGetterLookup(*func->id);
        generateFunctionCall(Assembler::Void, __qmljs_construct_global_lookup,
                             Assembler::ContextRegister, Assembler::PointerToValue(result),
                             Assembler::TrustedImm32(index),
                             baseAddressForCallArguments(),
                             Assembler::TrustedImm32(argc));
        return;
    }

    callRuntimeMethod(result, __qmljs_construct_activation_property, func, args);
}

void InstructionSelection::constructProperty(V4IR::Temp *base, const QString &name, V4IR::ExprList *args, V4IR::Temp *result)
{
    int argc = prepareVariableArguments(args);
    generateFunctionCall(Assembler::Void, __qmljs_construct_property, Assembler::ContextRegister,
            Assembler::PointerToValue(result), Assembler::Reference(base), Assembler::PointerToString(name), baseAddressForCallArguments(), Assembler::TrustedImm32(argc));
}

void InstructionSelection::constructValue(V4IR::Temp *value, V4IR::ExprList *args, V4IR::Temp *result)
{
    assert(value != 0);

    int argc = prepareVariableArguments(args);
    generateFunctionCall(Assembler::Void, __qmljs_construct_value, Assembler::ContextRegister,
            Assembler::PointerToValue(result), Assembler::Reference(value), baseAddressForCallArguments(), Assembler::TrustedImm32(argc));
}

void InstructionSelection::visitJump(V4IR::Jump *s)
{
    _as->jumpToBlock(_block, s->target);
}

void InstructionSelection::visitCJump(V4IR::CJump *s)
{
    if (V4IR::Temp *t = s->cond->asTemp()) {
        Assembler::RegisterID reg;
        if (t->kind == V4IR::Temp::PhysicalRegister) {
            Q_ASSERT(t->type == V4IR::BoolType);
            reg = (Assembler::RegisterID) t->index;
        } else if (t->kind == V4IR::Temp::StackSlot && t->type == V4IR::BoolType) {
            reg = Assembler::ReturnValueRegister;
            _as->toInt32Register(t, reg);
        } else {
            Address temp = _as->loadTempAddress(Assembler::ScratchRegister, t);
            Address tag = temp;
            tag.offset += offsetof(QV4::Value, tag);
            Assembler::Jump booleanConversion = _as->branch32(Assembler::NotEqual, tag, Assembler::TrustedImm32(QV4::Value::Boolean_Type));

            Address data = temp;
            data.offset += offsetof(QV4::Value, int_32);
            _as->load32(data, Assembler::ReturnValueRegister);
            Assembler::Jump testBoolean = _as->jump();

            booleanConversion.link(_as);
            reg = Assembler::ReturnValueRegister;
            generateFunctionCall(reg, __qmljs_to_boolean, Assembler::Reference(t));

            testBoolean.link(_as);
        }

        Assembler::Jump target = _as->branch32(Assembler::NotEqual, reg, Assembler::TrustedImm32(0));
        _as->addPatch(s->iftrue, target);
        _as->jumpToBlock(_block, s->iffalse);
        return;
    } else if (V4IR::Const *c = s->cond->asConst()) {
        // TODO: SSA optimization for constant condition evaluation should remove this.
        // See also visitCJump() in RegAllocInfo.
        generateFunctionCall(Assembler::ReturnValueRegister, __qmljs_to_boolean,
                             Assembler::PointerToValue(c));
        Assembler::Jump target = _as->branch32(Assembler::NotEqual, Assembler::ReturnValueRegister,
                                               Assembler::TrustedImm32(0));
        _as->addPatch(s->iftrue, target);
        _as->jumpToBlock(_block, s->iffalse);
        return;
    } else if (V4IR::Binop *b = s->cond->asBinop()) {
        CmpOp op = 0;
        CmpOpContext opContext = 0;
        const char *opName = 0;
        switch (b->op) {
        default: Q_UNREACHABLE(); assert(!"todo"); break;
        case V4IR::OpGt: setOp(op, opName, __qmljs_cmp_gt); break;
        case V4IR::OpLt: setOp(op, opName, __qmljs_cmp_lt); break;
        case V4IR::OpGe: setOp(op, opName, __qmljs_cmp_ge); break;
        case V4IR::OpLe: setOp(op, opName, __qmljs_cmp_le); break;
        case V4IR::OpEqual: setOp(op, opName, __qmljs_cmp_eq); break;
        case V4IR::OpNotEqual: setOp(op, opName, __qmljs_cmp_ne); break;
        case V4IR::OpStrictEqual: setOp(op, opName, __qmljs_cmp_se); break;
        case V4IR::OpStrictNotEqual: setOp(op, opName, __qmljs_cmp_sne); break;
        case V4IR::OpInstanceof: setOpContext(op, opName, __qmljs_cmp_instanceof); break;
        case V4IR::OpIn: setOpContext(op, opName, __qmljs_cmp_in); break;
        } // switch

        // TODO: in SSA optimization, do constant expression evaluation.
        // The case here is, for example:
        //   if (true === true) .....
        // Of course, after folding the CJUMP to a JUMP, dead-code (dead-basic-block)
        // elimination (which isn't there either) would remove the whole else block.
        if (opContext)
            _as->generateFunctionCallImp(Assembler::ReturnValueRegister, opName, opContext,
                                         Assembler::ContextRegister,
                                         Assembler::PointerToValue(b->left),
                                         Assembler::PointerToValue(b->right));
        else
            _as->generateFunctionCallImp(Assembler::ReturnValueRegister, opName, op,
                                         Assembler::PointerToValue(b->left),
                                         Assembler::PointerToValue(b->right));

        Assembler::Jump target = _as->branch32(Assembler::NotEqual, Assembler::ReturnValueRegister,
                                               Assembler::TrustedImm32(0));
        _as->addPatch(s->iftrue, target);
        _as->jumpToBlock(_block, s->iffalse);
        return;
    }
    Q_UNIMPLEMENTED();
    assert(!"TODO");
}

void InstructionSelection::visitRet(V4IR::Ret *s)
{
    if (V4IR::Temp *t = s->expr->asTemp()) {
#if defined(RETURN_VALUE_IN_REGISTER)
#if CPU(X86)
       Address addr = _as->loadTempAddress(Assembler::ScratchRegister, t);
       _as->load32(addr, JSC::X86Registers::eax);
       addr.offset += 4;
       _as->load32(addr, JSC::X86Registers::edx);
#else
        if (t->kind == V4IR::Temp::PhysicalRegister) {
            if (t->type == V4IR::DoubleType) {
                _as->moveDoubleTo64((Assembler::FPRegisterID) t->index,
                                    Assembler::ReturnValueRegister);
            } else {
                _as->zeroExtend32ToPtr((Assembler::RegisterID) t->index,
                                       Assembler::ReturnValueRegister);
                QV4::Value upper;
                switch (t->type) {
                case V4IR::SInt32Type:
                case V4IR::UInt32Type:
                    upper = QV4::Value::fromInt32(0);
                    break;
                case V4IR::BoolType:
                    upper = QV4::Value::fromBoolean(false);
                    break;
                default:
                    upper = QV4::Value::undefinedValue();
                    Q_UNIMPLEMENTED();
                }
                _as->or64(Assembler::TrustedImm64(((int64_t) upper.tag) << 32),
                          Assembler::ReturnValueRegister);
            }
        } else {
            _as->copyValue(Assembler::ReturnValueRegister, t);
        }
#endif
#else
        _as->loadPtr(addressForArgument(0), Assembler::ReturnValueRegister);
        _as->copyValue(Address(Assembler::ReturnValueRegister, 0), t);
#endif
    } else if (V4IR::Const *c = s->expr->asConst()) {
        QV4::Value retVal = convertToValue(c);
#if defined(RETURN_VALUE_IN_REGISTER)
#if CPU(X86)
        _as->move(Assembler::TrustedImm32(retVal.int_32), JSC::X86Registers::eax);
        _as->move(Assembler::TrustedImm32(retVal.tag), JSC::X86Registers::edx);
#else
        _as->move(Assembler::TrustedImm64(retVal.val), Assembler::ReturnValueRegister);
#endif
#else // !RETURN_VALUE_IN_REGISTER
        _as->loadPtr(addressForArgument(0), Assembler::ReturnValueRegister);
        _as->storeValue(retVal, Assembler::Address(Assembler::ReturnValueRegister));
#endif
    } else {
        Q_UNIMPLEMENTED();
        Q_UNREACHABLE();
        Q_UNUSED(s);
    }

    _as->leaveStandardStackFrame(/*withLocals*/true);
#if !defined(ARGUMENTS_IN_REGISTERS) && !defined(RETURN_VALUE_IN_REGISTER)
    // Emulate ret(n) instruction
    // Pop off return address into scratch register ...
    _as->pop(Assembler::ScratchRegister);
    // ... and overwrite the invisible argument with
    // the return address.
    _as->poke(Assembler::ScratchRegister);
#endif
    _as->ret();
}

int InstructionSelection::prepareVariableArguments(V4IR::ExprList* args)
{
    int argc = 0;
    for (V4IR::ExprList *it = args; it; it = it->next) {
        ++argc;
    }

    int i = 0;
    for (V4IR::ExprList *it = args; it; it = it->next, ++i) {
        V4IR::Expr *arg = it->expr;
        Q_ASSERT(arg != 0);
        _as->copyValue(_as->stackLayout().argumentAddressForCall(i), arg);
    }

    return argc;
}

void InstructionSelection::callRuntimeMethodImp(V4IR::Temp *result, const char* name, ActivationMethod method, V4IR::Expr *base, V4IR::ExprList *args)
{
    V4IR::Name *baseName = base->asName();
    assert(baseName != 0);

    int argc = prepareVariableArguments(args);
    _as->generateFunctionCallImp(Assembler::Void, name, method, Assembler::ContextRegister, Assembler::PointerToValue(result),
                                 Assembler::PointerToString(*baseName->id), baseAddressForCallArguments(),
                                 Assembler::TrustedImm32(argc));
}

QT_BEGIN_NAMESPACE
namespace QV4 {
bool operator==(const Value &v1, const Value &v2)
{
    return v1.rawValue() == v2.rawValue();
}
} // QV4 namespace
QT_END_NAMESPACE

int Assembler::ConstantTable::add(const Value &v)
{
    int idx = _values.indexOf(v);
    if (idx == -1) {
        idx = _values.size();
        _values.append(v);
    }
    return idx;
}

Assembler::ImplicitAddress Assembler::ConstantTable::loadValueAddress(V4IR::Const *c,
                                                                      RegisterID baseReg)
{
    return loadValueAddress(convertToValue(c), baseReg);
}

Assembler::ImplicitAddress Assembler::ConstantTable::loadValueAddress(const Value &v,
                                                                      RegisterID baseReg)
{
    _toPatch.append(_as->moveWithPatch(TrustedImmPtr(0), baseReg));
    ImplicitAddress addr(baseReg);
    addr.offset = add(v) * sizeof(QV4::Value);
    Q_ASSERT(addr.offset >= 0);
    return addr;
}

void Assembler::ConstantTable::finalize(JSC::LinkBuffer &linkBuffer, InstructionSelection *isel)
{
    void *tablePtr = isel->addConstantTable(&_values);

    foreach (DataLabelPtr label, _toPatch)
        linkBuffer.patch(label, tablePtr);
}