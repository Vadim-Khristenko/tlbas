#pragma once

// Marks a variable/parameter/function as possibly unused
#if defined(__GNUC__) || defined(__clang__)
#  define MAYBE_UNUSED __attribute__((unused))
#elif defined(_MSC_VER)
#  define MAYBE_UNUSED __pragma(warning(suppress: 4100 4101))
#else
#  define MAYBE_UNUSED
#endif

// Marks a function, variable, or type as deprecated
#if defined(__GNUC__) || defined(__clang__)
#  define TLBAS_DEPRECATED(msg) __attribute__((deprecated(msg)))
#elif defined(_MSC_VER)
#  define TLBAS_DEPRECATED(msg) __declspec(deprecated(msg))
#else
#  define TLBAS_DEPRECATED(msg)
#endif

// Export symbol for shared libraries
#if defined(_WIN32) || defined(_WIN64)
#  ifdef TLBAS_BUILD_DLL
#    define TLBAS_EXPORT __declspec(dllexport)
#  else
#    define TLBAS_EXPORT __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define TLBAS_EXPORT __attribute__((visibility("default")))
#else
#  define TLBAS_EXPORT
#endif

// Marks a function as not implemented (causes warning/error if called)
#if defined(__GNUC__) || defined(__clang__)
#  define TLBAS_NOT_IMPLEMENTED [[deprecated("Not implemented")]] __attribute__((noreturn))
#elif defined(_MSC_VER)
#  define TLBAS_NOT_IMPLEMENTED __declspec(noreturn) __declspec(deprecated("Not implemented"))
#else
#  define TLBAS_NOT_IMPLEMENTED
#endif

// Always inline or never inline
#if defined(__GNUC__) || defined(__clang__)
#  define TLBAS_INLINE inline __attribute__((always_inline))
#  define TLBAS_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#  define TLBAS_INLINE __forceinline
#  define TLBAS_NOINLINE __declspec(noinline)
#else
#  define TLBAS_INLINE inline
#  define TLBAS_NOINLINE
#endif

// Marks a function as pure (no side effects)
#if defined(__GNUC__) || defined(__clang__)
#  define TLBAS_PURE __attribute__((pure))
#  define TLBAS_CONST __attribute__((const))
#else
#  define TLBAS_PURE
#  define TLBAS_CONST
#endif

// Fallthrough annotation for switch cases
#if __cplusplus >= 201703L
#  define TLBAS_FALLTHROUGH [[fallthrough]]
#else
#  define TLBAS_FALLTHROUGH
#endif

// May alias (for optimization)
#if defined(__GNUC__) || defined(__clang__)
#  define TLBAS_MAY_ALIAS __attribute__((may_alias))
#else
#  define TLBAS_MAY_ALIAS
#endif

// Explicitly mark something as unused
#define TLBAS_UNUSED(x) ((void)(x))

// Mark function as force inline (for situations where inline is critical)
#if defined(__GNUC__) || defined(__clang__)
#  define TLBAS_FORCE_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#  define TLBAS_FORCE_INLINE __forceinline
#else
#  define TLBAS_FORCE_INLINE inline
#endif

// Mark function or return value as [[nodiscard]]
#if __cplusplus >= 201703L
#  define TLBAS_NODISCARD [[nodiscard]]
#else
#  define TLBAS_NODISCARD
#endif

// Mark function as [[noreturn]]
#if __cplusplus >= 201103L
#  define TLBAS_NORETURN [[noreturn]]
#else
#  define TLBAS_NORETURN
#endif

// Weak symbol (for plugin/override scenarios)
#if defined(__GNUC__) || defined(__clang__)
#  define TLBAS_WEAK __attribute__((weak))
#else
#  define TLBAS_WEAK
#endif

// Internal, public, private, protected visibility (for large modular codebases)
#if defined(__GNUC__) || defined(__clang__)
#  define TLBAS_INTERNAL __attribute__((visibility("hidden")))
#  define TLBAS_PUBLIC   __attribute__((visibility("default")))
#  define TLBAS_PRIVATE  __attribute__((visibility("hidden")))
#  define TLBAS_PROTECTED
#else
#  define TLBAS_INTERNAL
#  define TLBAS_PUBLIC
#  define TLBAS_PRIVATE
#  define TLBAS_PROTECTED
#endif

// Example Usage:
// TLBAS_DEPRECATED("Use newFunc instead") void oldFunc();
// TLBAS_EXPORT void exportedFunc();
// TLBAS_NOT_IMPLEMENTED void mustOverride();
// TLBAS_FORCE_INLINE int fastFunc();
// TLBAS_NODISCARD int importantReturn();
// TLBAS_UNUSED(variable);
// TLBAS_FALLTHROUGH;
