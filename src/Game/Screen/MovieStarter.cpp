#include "Game/Screen/MovieStarter.hpp"
#include "Game/Screen/MoviePlayingSequence.hpp"
#include "Game/Util/ActorSwitchUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"

#ifdef PETARI_NATIVE
// Progress telemetry and the automated smoke run (native/app); observation only.
#include <petari/milestone.hpp>

namespace {
    // Milestone names need static storage; only the two prologue movies are placed with
    // MovieStarter (PeachCastleGardenGalaxy).
    void noteMovieMilestone(s32 movieType, bool isEnd) {
        if (movieType == 0) {
            petari_milestone(isEnd ? "Movie.PrologueA.End" : "Movie.PrologueA.Start");
        } else if (movieType == 1) {
            petari_milestone(isEnd ? "Movie.PrologueB.End" : "Movie.PrologueB.Start");
        }
    }
}  // namespace
#endif

MovieStarter::MovieStarter(const char* pName) : LiveActor(pName), mMovieType(-1) {
}

void MovieStarter::init(const JMapInfoIter& rIter) {
    MR::connectToSceneLayoutMovement(this);
    MR::invalidateClipping(this);

    if (MR::useStageSwitchReadAppear(this, rIter)) {
        MR::listenStageSwitchOnAppear(this, MR::Functor(this, &MovieStarter::appear));
    }

    MR::useStageSwitchWriteDead(this, rIter);
    MR::createMoviePlayingSequence();
    MR::getJMapInfoArg0WithInit(rIter, &mMovieType);
    makeActorDead();
}

void MovieStarter::appear() {
    LiveActor::appear();

    if (mMovieType == 0) {
        if (MR::isOnGameEventFlagPlayMoviePrologueA()) {
            kill();
            return;
        }

        MR::onGameEventFlagPlayMoviePrologueA();
    }

    if (mMovieType == 1) {
        MR::onGameEventFlagPlayMoviePrologueB();
    }

#ifdef PETARI_NATIVE
    noteMovieMilestone(mMovieType, false);
#endif
    MR::startMovie(mMovieType);
}

void MovieStarter::kill() {
    if (MR::isValidSwitchDead(this)) {
        MR::onSwitchDead(this);
    }

    LiveActor::kill();
}

void MovieStarter::control() {
    if (MR::isEndMovie(mMovieType)) {
#ifdef PETARI_NATIVE
        noteMovieMilestone(mMovieType, true);
#endif
        kill();
    }
}
