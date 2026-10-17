#include "game/Game.h"

#ifdef WWII_FLOW_LOG
#include <cstdio>
#endif

#include <algorithm>

namespace ww {

Game::Game(const GameData& data) : data_(data), sim_(data, 0xC0FFEEu) {
    highScores_.assign(8, HighScoreEntry{});
}

void Game::submitInput(const GameInput& in) { input_ = in; }

bool Game::pressed(bool now, bool& prev) {
    const bool edge = now && !prev;
    prev = now;
    return edge;
}

void Game::enterState(GameState s) {
#ifdef WWII_FLOW_LOG
    std::fprintf(stderr, "[flow] t=%.1f state -> %d\n", sim_.director().time(),
                 static_cast<int>(s));
#endif
    state_ = s;
    stateTimer_ = 0.0f;
    menuSel_ = 0;
}

DifficultyMods Game::modsFor(Difficulty d) {
    switch (d) {
    case Difficulty::Easy:   return {0.8f, 1.35f, 0.9f};
    case Difficulty::Normal: return {1.0f, 1.0f, 1.0f};
    case Difficulty::Hard:   return {1.25f, 0.72f, 1.1f};
    }
    return {};
}

std::string Game::stageName() const {
    if (stageIndex_ < static_cast<int>(data_.stages.size()))
        return data_.stages[static_cast<std::size_t>(stageIndex_)].name;
    return "Stage";
}

int Game::highScore() const {
    if (highScores_.empty()) return 0;
    int best = 0;
    for (const auto& e : highScores_) best = std::max(best, e.score);
    return best;
}

bool Game::scoreQualifies(int score) const {
    if (score <= 0) return false;
    if (highScores_.empty()) return true;
    // Table is kept sorted descending, padded with zero-score rows.
    return score > highScores_.back().score;
}

void Game::submitHighScore(const std::string& initials, int score) {
    HighScoreEntry e;
    e.score = score;
    // Sanitize: up to 3 characters, upper-case A-Z only, padded with A.
    std::string ini;
    for (char c : initials) {
        if (ini.size() < 3 && c >= 'A' && c <= 'Z')
            ini += c;
    }
    if (ini.empty()) ini = "AAA";
    while (ini.size() < 3) ini += 'A';
    e.initials = ini;
    highScores_.push_back(e);
    std::sort(highScores_.begin(), highScores_.end(),
              [](const HighScoreEntry& a, const HighScoreEntry& b) {
                  return a.score > b.score;
              });
    highScores_.resize(8);
    events_.push_back({GameEvent::Type::SaveHighScores, ""});
}

void Game::onTextInput(char c) {
    if (state_ != GameState::EnterInitials) return;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c < 'A' || c > 'Z' || initialsLen_ >= 3) return;
    initials_[static_cast<std::size_t>(initialsLen_)] = c;
    ++initialsLen_;
    if (initialsLen_ >= 3) {
        // Classic behaviour: the third letter commits the entry.
        submitHighScore(initialsEntry(), pendingScore_);
        enterState(GameState::HighScores);
    }
}

void Game::onInitialsBackspace() {
    if (state_ != GameState::EnterInitials) return;
    if (initialsLen_ > 0) --initialsLen_;
}

std::string Game::initialsEntry() const {
    std::string s;
    for (int i = 0; i < 3; ++i)
        s += i < initialsLen_ ? initials_[static_cast<std::size_t>(i)] : 'A';
    return s;
}

void Game::loadHighScores(std::vector<HighScoreEntry> scores) {
    highScores_ = std::move(scores);
    if (highScores_.size() < 8) highScores_.resize(8);
    std::sort(highScores_.begin(), highScores_.end(),
              [](const HighScoreEntry& a, const HighScoreEntry& b) {
                  return a.score > b.score;
              });
    highScores_.resize(8);
}

bool Game::popEvent(GameEvent& out) {
    if (events_.empty()) return false;
    out = events_.front();
    events_.erase(events_.begin());
    return true;
}

void Game::startGame() {
    stageIndex_ = 0;
    score_ = 0;
    startStage();
}

void Game::startStage() {
    sim_.startStage(stageIndex_, modsFor(settings_.difficulty));
    bossDefeated_ = false;
    gameOverDelay_ = 0.0f;
    enterState(GameState::StageIntro);
}

void Game::finishStage() {
    score_ = sim_.score();
    if (stageIndex_ + 1 < static_cast<int>(data_.stages.size())) {
        const int carried = score_;
        ++stageIndex_;
        startStage();
        // The simulation resets its score for the new stage; carry the
        // previous stage's points across so the run total is preserved.
        sim_.setScore(carried);
        score_ = carried;
    } else {
        // Final mission complete: record the run (this path previously
        // skipped high-score recording entirely).
        if (scoreQualifies(score_)) {
            pendingScore_ = score_;
            pendingWasVictory_ = true;
            initialsLen_ = 0;
            enterState(GameState::EnterInitials);
        } else {
            enterState(GameState::Victory);
        }
    }
}

void Game::gameOver() {
    score_ = sim_.score();
    if (scoreQualifies(score_)) {
        // Arcade flow: enter initials, then show the table.
        pendingScore_ = score_;
        pendingWasVictory_ = false;
        initialsLen_ = 0;
        enterState(GameState::EnterInitials);
    } else {
        enterState(GameState::GameOver);
    }
}

void Game::processSimEvents() {
    SimEvent ev;
    while (sim_.popEvent(ev)) {
        switch (ev.type) {
        case SimEvent::Type::Sound:
            events_.push_back({GameEvent::Type::Sound, ev.name, ev.pos});
            break;
        case SimEvent::Type::BossDefeated:
            bossDefeated_ = true;
            stageCompleteDelay_ = 2.4f;
            break;
        case SimEvent::Type::BombUsed:
        case SimEvent::Type::PlayerHit:
        case SimEvent::Type::PlayerDied:
        case SimEvent::Type::EnemyKilled:
        case SimEvent::Type::FormationCleared:
        case SimEvent::Type::PowerupTaken:
        case SimEvent::Type::BossSpawned:
        case SimEvent::Type::BossComponentDestroyed:
        case SimEvent::Type::StageComplete:
            break;
        }
    }
}

void Game::update(float dt) {
    stateTimer_ += dt;
    processSimEvents();

    const bool up = pressed(input_.up, prevInput_.up);
    const bool down = pressed(input_.down, prevInput_.down);
    const bool left = pressed(input_.left, prevInput_.left);
    const bool right = pressed(input_.right, prevInput_.right);
    const bool confirm = pressed(input_.confirm, prevInput_.confirm);
    const bool back = pressed(input_.back, prevInput_.back);
    const bool pause = pressed(input_.pause, prevInput_.pause);
    if (pressed(input_.debugToggle, prevInput_.debugToggle))
        debugOverlay = !debugOverlay;

    switch (state_) {
    case GameState::Title: {
        constexpr int kItems = 5;  // Start / Scores / Help / Options / Quit
        if (up) menuSel_ = (menuSel_ + kItems - 1) % kItems;
        if (down) menuSel_ = (menuSel_ + 1) % kItems;
        if (confirm) {
            if (menuSel_ == 0) startGame();
            else if (menuSel_ == 1) enterState(GameState::HighScores);
            else if (menuSel_ == 2) enterState(GameState::Help);
            else if (menuSel_ == 3) enterState(GameState::Options);
            else events_.push_back({GameEvent::Type::Quit, ""});
        }
        break;
    }

    case GameState::HighScores:
        if (back || confirm) enterState(GameState::Title);
        break;

    case GameState::Help:
        if (back || confirm) enterState(GameState::Title);
        break;

    case GameState::Options: {
        constexpr int kItems = 6;  // difficulty/master/music/sfx/fullscreen/back
        if (up) menuSel_ = (menuSel_ + kItems - 1) % kItems;
        if (down) menuSel_ = (menuSel_ + 1) % kItems;
        auto volStep = [](float& v, bool l, bool r) {
            if (l) v = clamp(v - 0.1f, 0.0f, 1.0f);
            if (r) v = clamp(v + 0.1f, 0.0f, 1.0f);
        };
        bool changed = false;
        switch (menuSel_) {
        case 0:
            if (left) settings_.difficulty = static_cast<Difficulty>(
                              std::max(0, static_cast<int>(settings_.difficulty) - 1)),
                changed = true;
            if (right) settings_.difficulty = static_cast<Difficulty>(
                              std::min(2, static_cast<int>(settings_.difficulty) + 1)),
                changed = true;
            break;
        case 1: volStep(settings_.masterVolume, left, right); changed = true; break;
        case 2: volStep(settings_.musicVolume, left, right); changed = true; break;
        case 3: volStep(settings_.sfxVolume, left, right); changed = true; break;
        case 4:
            if (left || right) settings_.fullscreen = !settings_.fullscreen, changed = true;
            break;
        case 5:
            if (confirm || back) {
                if (changed) events_.push_back({GameEvent::Type::SaveSettings, ""});
                enterState(GameState::Title);
            }
            break;
        default: break;
        }
        if (changed) events_.push_back({GameEvent::Type::SaveSettings, ""});
        if (back && menuSel_ != 5) enterState(GameState::Title);
        break;
    }

    case GameState::StageIntro:
        if (confirm || stateTimer_ > 3.0f) enterState(GameState::Playing);
        break;

    case GameState::Playing: {
        if (pause) {
            enterState(GameState::Paused);
            menuSel_ = 0;
            break;
        }
        sim_.setPlayerIntent({input_.moveX, input_.moveY}, input_.fire,
                             input_.special);
        sim_.update();
        score_ = sim_.score();

        const auto p = sim_.player();
        if (!p.alive && p.lives < 0) {
            gameOverDelay_ += dt;
            if (gameOverDelay_ > 1.4f) gameOver();
        }
        if (bossDefeated_) {
            stageCompleteDelay_ -= dt;
            if (stageCompleteDelay_ <= 0.0f) finishStage();
        }
        break;
    }

    case GameState::Paused:
        if (pause || confirm) enterState(GameState::Playing);
        if (back) enterState(GameState::Title);
        break;

    case GameState::StageComplete:
        if (confirm || stateTimer_ > 3.5f) finishStage();
        break;

    case GameState::GameOver:
        if ((confirm || back) && stateTimer_ > 1.0f)
            enterState(GameState::HighScores);
        break;

    case GameState::Victory:
        if ((confirm || back) && stateTimer_ > 1.2f)
            enterState(GameState::Title);
        break;

    case GameState::EnterInitials: {
        if (stateTimer_ < 0.5f) break;  // swallow the pass-through press
        if (back) {
            // Skip recording; show the plain result screen.
            enterState(pendingWasVictory_ ? GameState::Victory
                                          : GameState::GameOver);
            break;
        }
        if (confirm) {  // commit with the letters typed so far (padded)
            submitHighScore(initialsEntry(), pendingScore_);
            enterState(GameState::HighScores);
        }
        break;
    }
    }
}

}  // namespace ww
