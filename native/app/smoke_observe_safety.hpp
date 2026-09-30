#pragma once

namespace PetariNative::App::Smoke {

template <class Player>
bool canObservePlayer(const Player* player) {
    return player && player->getMario() &&
           (!player->_934 || (player->_924 && player->_924->mHost));
}

// Warp status counts as "in rush" without an actor-bound rush sensor.
// Unlike gameplay callers in race/bind states, observers run in every state.
template <class Player>
const char* observedRushActorName(const Player* player, bool inRush) {
    if (!player || !inRush || !player->_924 || !player->_924->mHost || !player->_924->mHost->mName) {
        return "";
    }
    return player->_924->mHost->mName;
}

} // namespace PetariNative::App::Smoke
