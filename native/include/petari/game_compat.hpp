#pragma once

// Native replacements for the MSL C++ library extensions that game code uses.
//
// On the Wii the game includes MSL's <algorithm>, which pulls in functional.hpp
// (libs/MSL_C++/include). Those headers provide adaptors removed from C++17
// (unary_function, binder2nd, mem_fun, ptr_fun, ...) and MSL-only extensions
// (mem_func, for_each_array, find_if_array). Native builds use libc++, so this
// header restores the same names with the same semantics as the MSL versions.
// It deliberately does not replace any algorithm libc++ already provides.

#include <algorithm>
#include <functional>

#if !defined(_LIBCPP_VERSION)
#error "petari/game_compat.hpp expects libc++"
#endif
#if __cplusplus != 201703L || defined(_LIBCPP_ENABLE_CXX17_REMOVED_BINDERS) ||                     \
    defined(_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION) ||                                \
    defined(_LIBCPP_ENABLE_CXX17_REMOVED_FEATURES)
#error "petari/game_compat.hpp targets C++17 libc++ (own adaptors, libc++ std::not1)"
#endif

namespace std {
    template < class Arg, class Result >
    struct unary_function {
        typedef Arg argument_type;
        typedef Result result_type;
    };

    template < class Arg1, class Arg2, class Result >
    struct binary_function {
        typedef Arg1 first_argument_type;
        typedef Arg2 second_argument_type;
        typedef Result result_type;
    };

    template < class Func, class Type >
    class binder1st : public unary_function< typename Func::second_argument_type, typename Func::result_type > {
    public:
        typedef typename Func::second_argument_type Arg;
        typedef typename Func::result_type Result;

        binder1st(const Func& rFunc, const Type& rValue) : mf_(rFunc), v_(rValue) {}

        Result operator()(const Arg& rArg) const { return mf_(v_, rArg); }
        Result operator()(Arg& rArg) const { return mf_(v_, rArg); }

    private:
        Func mf_;
        Type v_;
    };

    template < class Func, class Type >
    class binder2nd : public unary_function< typename Func::first_argument_type, typename Func::result_type > {
    public:
        typedef typename Func::first_argument_type Arg;
        typedef typename Func::result_type Result;

        binder2nd(const Func& rFunc, const Type& rValue) : mf_(rFunc), v_(rValue) {}

        Result operator()(const Arg& rArg) const { return mf_(rArg, v_); }
        Result operator()(Arg& rArg) const { return mf_(rArg, v_); }

    private:
        Func mf_;
        Type v_;
    };

    template < class Func, class Type >
    binder1st< Func, typename Func::first_argument_type > bind1st(const Func& rFunc, const Type& rArg) {
        return binder1st< Func, typename Func::first_argument_type >(rFunc, rArg);
    }

    template < class Func, class Type >
    binder2nd< Func, typename Func::second_argument_type > bind2nd(const Func& rFunc, const Type& rArg) {
        return binder2nd< Func, typename Func::second_argument_type >(rFunc, rArg);
    }

    template < class Return, class Type >
    class mem_fun_t : public unary_function< Type*, Return > {
    public:
        explicit mem_fun_t(Return (Type::*pFunction)()) : mf_(pFunction) {}
        Return operator()(Type* pObject) const { return (pObject->*mf_)(); }

    private:
        Return (Type::*mf_)();
    };

    template < class Return, class Type >
    class mem_fun_ref_t : public unary_function< Type, Return > {
    public:
        explicit mem_fun_ref_t(Return (Type::*pFunction)()) : mf_(pFunction) {}
        Return operator()(Type& rObject) const { return (rObject.*mf_)(); }

    private:
        Return (Type::*mf_)();
    };

    template < class Return, class Type >
    class const_mem_fun_t : public unary_function< const Type*, Return > {
    public:
        explicit const_mem_fun_t(Return (Type::*pFunction)() const) : mf_(pFunction) {}
        Return operator()(const Type* pObject) const { return (pObject->*mf_)(); }

    private:
        Return (Type::*mf_)() const;
    };

    template < class Result, class Type, class Arg >
    class mem_fun1_t : public binary_function< Type*, Arg, Result > {
    public:
        explicit mem_fun1_t(Result (Type::*pFunction)(Arg)) : mf_(pFunction) {}
        Result operator()(Type* pObject, Arg a) const { return (pObject->*mf_)(a); }

    private:
        Result (Type::*mf_)(Arg);
    };

    template < class Result, class Type, class Arg >
    class mem_fun1_ref_t : public binary_function< Type, Arg, Result > {
    public:
        explicit mem_fun1_ref_t(Result (Type::*pFunction)(Arg)) : mf_(pFunction) {}
        Result operator()(Type& rObject, Arg a) const { return (rObject.*mf_)(a); }

    private:
        Result (Type::*mf_)(Arg);
    };

    template < class Result, class Type, class Arg >
    class const_mem_fun1_t : public binary_function< const Type*, Arg, Result > {
    public:
        explicit const_mem_fun1_t(Result (Type::*pFunction)(Arg) const) : mf_(pFunction) {}
        Result operator()(const Type* pObject, Arg a) const { return (pObject->*mf_)(a); }

    private:
        Result (Type::*mf_)(Arg) const;
    };

    template < class Result, class Type >
    mem_fun_t< Result, Type > mem_fun(Result (Type::*pFunction)()) {
        return mem_fun_t< Result, Type >(pFunction);
    }

    template < class Result, class Type >
    const_mem_fun_t< Result, Type > mem_fun(Result (Type::*pFunction)() const) {
        return const_mem_fun_t< Result, Type >(pFunction);
    }

    template < class Result, class Type, class Arg >
    mem_fun1_t< Result, Type, Arg > mem_fun(Result (Type::*pFunction)(Arg)) {
        return mem_fun1_t< Result, Type, Arg >(pFunction);
    }

    template < class Result, class Type, class Arg >
    const_mem_fun1_t< Result, Type, Arg > mem_fun(Result (Type::*pFunction)(Arg) const) {
        return const_mem_fun1_t< Result, Type, Arg >(pFunction);
    }

    // MSL extension.
    template < class Result, class Type >
    inline mem_fun_t< Result, Type > mem_func(Result (Type::*pFunction)()) {
        return mem_fun_t< Result, Type >(pFunction);
    }

    template < class Result, class Type >
    inline const_mem_fun_t< Result, Type > mem_func(Result (Type::*pFunction)() const) {
        return const_mem_fun_t< Result, Type >(pFunction);
    }

    template < class Result, class Type, class Arg >
    inline mem_fun1_t< Result, Type, Arg > mem_func(Result (Type::*pFunction)(Arg)) {
        return mem_fun1_t< Result, Type, Arg >(pFunction);
    }

    template < class Result, class Type, class Arg >
    inline const_mem_fun1_t< Result, Type, Arg > mem_func(Result (Type::*pFunction)(Arg) const) {
        return const_mem_fun1_t< Result, Type, Arg >(pFunction);
    }

    template < class Arg, class Result >
    class pointer_to_unary_function : public unary_function< Arg, Result > {
    public:
        explicit pointer_to_unary_function(Result (*pFunction)(Arg)) : mF(pFunction) {}
        Result operator()(Arg x) const { return mF(x); }

        Result (*mF)(Arg);
    };

    template < class Arg, class Result >
    pointer_to_unary_function< Arg, Result > ptr_fun(Result (*pFunction)(Arg)) {
        return pointer_to_unary_function< Arg, Result >(pFunction);
    }

    template < class Arg1, class Arg2, class Result >
    class pointer_to_binary_function : public binary_function< Arg1, Arg2, Result > {
    public:
        explicit pointer_to_binary_function(Result (*pFunction)(Arg1, Arg2)) : mF(pFunction) {}
        Result operator()(Arg1 x, Arg2 y) const { return mF(x, y); }

        Result (*mF)(Arg1, Arg2);
    };

    template < class Arg1, class Arg2, class Result >
    pointer_to_binary_function< Arg1, Arg2, Result > ptr_fun(Result (*pFunction)(Arg1, Arg2)) {
        return pointer_to_binary_function< Arg1, Arg2, Result >(pFunction);
    }

    // std::not1 and std::unary_negate still come from libc++ in C++17 and match MSL.

    // MSL extensions: iterate over an array of objects, passing each element's address.
    template < class InputIterator, class Function >
    inline Function for_each_array(InputIterator* pFirst, InputIterator* pLast, Function f) {
        for (; pFirst != pLast; pFirst++) {
            f(pFirst);
        }

        return f;
    }

    template < class InputIt, class UnaryPredicate >
    InputIt* find_if_array(InputIt* pFirst, InputIt* pLast, UnaryPredicate p) {
        for (; pFirst != pLast && !p(pFirst); pFirst++) {
        }

        return pFirst;
    }

    template < class InputIt, class UnaryPredicate >
    InputIt rfind_if(InputIt first, InputIt last, UnaryPredicate p) {
        for (; first != last && !p(*first); first--) {
        }

        return first;
    }
}  // namespace std
