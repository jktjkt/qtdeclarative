CONFIG += exceptions

CONFIG += warn_off

INCLUDEPATH += $$PWD
INCLUDEPATH += $$OUT_PWD

SOURCES += \
    $$PWD/qv4engine.cpp \
    $$PWD/qv4context.cpp \
    $$PWD/qv4runtime.cpp \
    $$PWD/qv4value.cpp \
    $$PWD/qv4debugging.cpp \
    $$PWD/qv4lookup.cpp \
    $$PWD/qv4identifier.cpp \
    $$PWD/qv4identifiertable.cpp \
    $$PWD/qv4mm.cpp \
    $$PWD/qv4managed.cpp \
    $$PWD/qv4internalclass.cpp \
    $$PWD/qv4sparsearray.cpp \
    $$PWD/qv4arrayobject.cpp \
    $$PWD/qv4argumentsobject.cpp \
    $$PWD/qv4booleanobject.cpp \
    $$PWD/qv4dateobject.cpp \
    $$PWD/qv4errorobject.cpp \
    $$PWD/qv4function.cpp \
    $$PWD/qv4functionobject.cpp \
    $$PWD/qv4globalobject.cpp \
    $$PWD/qv4jsonobject.cpp \
    $$PWD/qv4mathobject.cpp \
    $$PWD/qv4numberobject.cpp \
    $$PWD/qv4object.cpp \
    $$PWD/qv4objectproto.cpp \
    $$PWD/qv4regexpobject.cpp \
    $$PWD/qv4stringobject.cpp \
    $$PWD/qv4variantobject.cpp \
    $$PWD/qv4string.cpp \
    $$PWD/qv4objectiterator.cpp \
    $$PWD/qv4regexp.cpp \
    $$PWD/qv4unwindhelper.cpp \
    $$PWD/qv4serialize.cpp \
    $$PWD/qv4script.cpp \
    $$PWD/qv4executableallocator.cpp \
    $$PWD/qv4sequenceobject.cpp \
    $$PWD/qv4include.cpp \
    $$PWD/qv4qobjectwrapper.cpp \
    $$PWD/qv4qmlextensions.cpp \
    $$PWD/qv4stacktrace.cpp \
    $$PWD/qv4exception.cpp \
    $$PWD/qv4vme_moth.cpp

HEADERS += \
    $$PWD/qv4global_p.h \
    $$PWD/qv4engine_p.h \
    $$PWD/qv4context_p.h \
    $$PWD/qv4runtime_p.h \
    $$PWD/qv4math_p.h \
    $$PWD/qv4value_p.h \
    $$PWD/qv4value_def_p.h \
    $$PWD/qv4debugging_p.h \
    $$PWD/qv4lookup_p.h \
    $$PWD/qv4identifier_p.h \
    $$PWD/qv4identifiertable_p.h \
    $$PWD/qv4mm_p.h \
    $$PWD/qv4managed_p.h \
    $$PWD/qv4internalclass_p.h \
    $$PWD/qv4sparsearray_p.h \
    $$PWD/qv4arrayobject_p.h \
    $$PWD/qv4argumentsobject_p.h \
    $$PWD/qv4booleanobject_p.h \
    $$PWD/qv4dateobject_p.h \
    $$PWD/qv4errorobject_p.h \
    $$PWD/qv4function_p.h \
    $$PWD/qv4functionobject_p.h \
    $$PWD/qv4globalobject_p.h \
    $$PWD/qv4jsonobject_p.h \
    $$PWD/qv4mathobject_p.h \
    $$PWD/qv4numberobject_p.h \
    $$PWD/qv4object_p.h \
    $$PWD/qv4objectproto_p.h \
    $$PWD/qv4regexpobject_p.h \
    $$PWD/qv4stringobject_p.h \
    $$PWD/qv4variantobject_p.h \
    $$PWD/qv4string_p.h \
    $$PWD/qv4property_p.h \
    $$PWD/qv4objectiterator_p.h \
    $$PWD/qv4regexp_p.h \
    $$PWD/qv4unwindhelper_p.h \
    $$PWD/qv4unwindhelper_dw2_p.h \
    $$PWD/qv4unwindhelper_arm_p.h \
    $$PWD/qv4serialize_p.h \
    $$PWD/qv4script_p.h \
    $$PWD/qv4scopedvalue_p.h \
    $$PWD/qv4util_p.h \
    $$PWD/qv4executableallocator_p.h \
    $$PWD/qv4sequenceobject_p.h \
    $$PWD/qv4include_p.h \
    $$PWD/qv4qobjectwrapper_p.h \
    $$PWD/qv4qmlextensions_p.h \
    $$PWD/qv4stacktrace_p.h \
    $$PWD/qv4exception_p.h \
    $$PWD/qv4vme_moth_p.h

# Use SSE2 floating point math on 32 bit instead of the default
# 387 to make test results pass on 32 and on 64 bit builds.
linux-g++*:isEqual(QT_ARCH,i386) {
    QMAKE_CFLAGS += -march=pentium4 -msse2 -mfpmath=sse
    QMAKE_CXXFLAGS += -march=pentium4 -msse2 -mfpmath=sse
}

linux*|mac {
    LIBS += -ldl
}

# Only on Android/ARM at the moment, because only there we have issues
# replacing __gnu_Unwind_Find_exidx with our own implementation,
# and thus require static libgcc linkage.
android:equals(QT_ARCH, "arm"):*g++* {
    static_libgcc = $$system($$QMAKE_CXX -print-file-name=libgcc.a)
    LIBS += $$static_libgcc
    SOURCES += $$PWD/qv4exception_gcc.cpp
    DEFINES += V4_CXX_ABI_EXCEPTION
}

debug-with-libunwind {
    UW_INC=$$(LIBUNWIND_INCLUDES)
    isEmpty(UW_INC): error("Please set LIBUNWIND_INCLUDES")
    INCLUDEPATH += $$UW_INC
    UW_LIBS=$$(LIBUNWIND_LIBS)
    isEmpty(UW_LIBS): error("Please set LIBUNWIND_LIBS")
    LIBS += -L$$UW_LIBS
    equals(QT_ARCH, arm): LIBS += -lunwind-arm
    LIBS += -lunwind-dwarf-common -lunwind-dwarf-local -lunwind-elf32 -lunwind
    DEFINES += WTF_USE_LIBUNWIND_DEBUG=1
}

valgrind {
    DEFINES += V4_USE_VALGRIND
}

ios: DEFINES += ENABLE_ASSEMBLER_WX_EXCLUSIVE=1

win32 {
    LIBS_PRIVATE += -lDbgHelp
}

include(../../3rdparty/double-conversion/double-conversion.pri)