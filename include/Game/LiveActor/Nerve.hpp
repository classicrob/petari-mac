#pragma once

#include "Game/LiveActor/Spine.hpp"

/// @brief Used for executing states of a LiveActor.
class Nerve {
public:
    /// @brief Executes a state based on the host actor.
    /// @param pSpine The spine that contains the host LiveActor to execute the state for.
    virtual void execute(Spine* pSpine) const = 0;
    /// @brief Executes after the last iteration of a state before it executes another state.
    /// @param pSpine The spine that contains the host LiveActor to execute the state for.
    virtual void executeOnEnd(Spine* pSpine) const;
};

#ifdef PETARI_NATIVE
#define NERVE_INSTANCE(name) inline name name::sInstance;
#else
#define NERVE_INSTANCE(name) name name::sInstance ATTRIBUTE_WEAK;
#endif

#define NEW_NERVE(name, parent_class, executor_name)                                                                                                 \
    class name : public Nerve {                                                                                                                      \
    public:                                                                                                                                          \
        virtual void execute(Spine* pSpine) const {                                                                                                  \
            parent_class* actor = reinterpret_cast< parent_class* >(pSpine->mExecutor);                                                              \
            actor->exe##executor_name();                                                                                                             \
        };                                                                                                                                           \
        static name sInstance;                                                                                                                       \
    };                                                                                                                                               \
    NERVE_INSTANCE(name)

#define NEW_NERVE_ONEND(name, parent_class, executor_name, executorOnEnd_name)                                                                       \
    class name : public Nerve {                                                                                                                      \
    public:                                                                                                                                          \
        virtual void execute(Spine* pSpine) const {                                                                                                  \
            parent_class* actor = reinterpret_cast< parent_class* >(pSpine->mExecutor);                                                              \
            actor->exe##executor_name();                                                                                                             \
        };                                                                                                                                           \
        virtual void executeOnEnd(Spine* pSpine) const {                                                                                             \
            parent_class* actor = reinterpret_cast< parent_class* >(pSpine->mExecutor);                                                              \
            actor->end##executorOnEnd_name();                                                                                                        \
        };                                                                                                                                           \
        static name sInstance;                                                                                                                       \
    };                                                                                                                                               \
    NERVE_INSTANCE(name)

/* easy alternative to get a nerve instance (in the standard format) */
#define GET_NERVE(cls, nerve) (&Nrv##cls::nerve::sInstance)

/* easy alternative to get a nerve instance (with an anonymous namespace) */
#define GET_NERVE_ANON(nerve) (&::nerve::sInstance)

/* easy alternative to get a nerve instance (with no namespace) */
#define GET_NERVE_GLOBAL(nerve) (&nerve::sInstance)

/* easy alternative to get a nerve instance (where namespace does not follow standard) */
#define GET_NERVE_DIRECT(nmspc, nerve) (&nmspc::nerve::sInstance)
