// Frame Perfect Checker
//
// Workflow:
//   1. Pause menu -> "Record", beat the level in ONE attempt (normal mode, from 0%).
//      The macro always holds your most recent attempt from the start.
//   2. Re-enter the level, pause -> "Analyze".
//      The mod re-simulates the level once per input shifted -1 and +1 frame.
//      An input is "frame perfect" if BOTH shifts make you die (window = 1 frame).
//   3. A popup shows the count and the frames.
//
// Analysis runs never show the end screen or submit anything.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <vector>

using namespace geode::prelude;

namespace {
    struct Input {
        int frame;
        bool down;
        int button;
        bool p1;
    };

    enum class Mode { Idle, Recording, Analyzing };

    struct Trial {
        size_t idx;  // npos = unmodified baseline
        int shift;   // -1 or +1
    };

    constexpr size_t npos = std::numeric_limits<size_t>::max();

    struct State {
        Mode mode = Mode::Idle;
        std::vector<Input> macro;

        // playback
        std::vector<Input> active;
        size_t nextInput = 0;
        bool trialDone = false;

        // analysis
        std::vector<Trial> queue;
        size_t qpos = 0;
        bool baselineOk = false;
        std::map<size_t, std::pair<bool, bool>> results; // idx -> {-1 survived, +1 survived}
    };

    State& S() {
        static State s;
        return s;
    }

    bool isPlayLayer(GJBaseGameLayer* l) {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == l;
    }

    void runNextTrial();

    void finishAnalysis() {
        auto& s = S();
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
        s.mode = Mode::Idle;

        if (!s.baselineOk) {
            FLAlertLayer::create(
                "Analysis failed",
                "The unmodified macro did not beat the level on replay (desync, or the run wasn't "
                "recorded from 0% in normal mode). Re-record and try again.",
                "OK"
            )->show();
            return;
        }

        std::vector<int> fpFrames;
        for (auto& [idx, r] : s.results) {
            if (!r.first && !r.second) fpFrames.push_back(s.macro[idx].frame);
        }

        std::string list;
        for (size_t i = 0; i < fpFrames.size() && i < 20; i++) {
            list += fmt::format("{}{}", i ? ", " : "", fpFrames[i]);
        }
        if (fpFrames.size() > 20) list += ", ...";

        auto msg = fmt::format(
            "Inputs analyzed: {}\nFrame perfect: {}\n\nFrames: {}",
            s.macro.size(), fpFrames.size(), list.empty() ? "none" : list
        );
        FLAlertLayer::create("Frame Perfects", msg, "OK")->show();
    }

    void finishTrial(bool survived) {
        auto& s = S();
        if (s.trialDone) return;
        s.trialDone = true;

        auto t = s.queue[s.qpos];
        if (t.idx == npos) {
            s.baselineOk = survived;
            if (!survived) {
                s.qpos = s.queue.size(); // abort
            }
        } else {
            auto& r = s.results[t.idx];
            (t.shift < 0 ? r.first : r.second) = survived;
        }
        s.qpos++;

        // never reset from inside a death/complete hook
        Loader::get()->queueInMainThread([] {
            if (S().mode == Mode::Analyzing) runNextTrial();
        });
    }

    void runNextTrial() {
        auto& s = S();
        auto pl = PlayLayer::get();
        if (!pl) {
            CCDirector::get()->getScheduler()->setTimeScale(1.f);
            s.mode = Mode::Idle;
            return;
        }
        if (s.qpos >= s.queue.size()) {
            finishAnalysis();
            return;
        }

        auto t = s.queue[s.qpos];
        s.active = s.macro;
        bool valid = true;
        if (t.idx != npos) {
            if (s.active[t.idx].frame + t.shift < 0) valid = false;
            else s.active[t.idx].frame += t.shift;
            std::stable_sort(s.active.begin(), s.active.end(),
                [](const Input& a, const Input& b) { return a.frame < b.frame; });
        }

        if (!valid) { // can't shift before frame 0 -> treat as blocked
            s.trialDone = false;
            finishTrial(false);
            return;
        }

        s.nextInput = 0;
        s.trialDone = false;
        pl->resetLevel();
    }

    void startAnalysis() {
        auto& s = S();
        if (s.macro.empty()) {
            FLAlertLayer::create("No macro", "Record a run first.", "OK")->show();
            return;
        }
        s.queue.clear();
        s.results.clear();
        s.qpos = 0;
        s.baselineOk = false;

        s.queue.push_back({npos, 0});
        for (size_t i = 0; i < s.macro.size(); i++) {
            s.queue.push_back({i, -1});
            s.queue.push_back({i, +1});
        }

        s.mode = Mode::Analyzing;
        auto speed = Mod::get()->getSettingValue<double>("analysis-speed");
        CCDirector::get()->getScheduler()->setTimeScale(static_cast<float>(speed));
        Loader::get()->queueInMainThread([] { runNextTrial(); });
    }
}

class $modify(FPBaseLayer, GJBaseGameLayer) {
    // Record user input / block user input during analysis.
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = S();
        if (isPlayLayer(this)) {
            if (s.mode == Mode::Analyzing) return; // ignore real input
            if (s.mode == Mode::Recording) {
                s.macro.push_back({
                    static_cast<int>(this->m_gameState.m_currentProgress),
                    down, button, isPlayer1
                });
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Feed macro inputs on the exact physics step.
    void processCommands(float dt) {
        auto& s = S();
        if (s.mode == Mode::Analyzing && isPlayLayer(this)) {
            int frame = static_cast<int>(this->m_gameState.m_currentProgress);
            while (s.nextInput < s.active.size() && s.active[s.nextInput].frame <= frame) {
                auto& in = s.active[s.nextInput++];
                GJBaseGameLayer::handleButton(in.down, in.button, in.p1);
            }
        }
        GJBaseGameLayer::processCommands(dt);
    }
};

class $modify(FPPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        auto& s = S();
        s.nextInput = 0;
        s.trialDone = false;
        if (s.mode == Mode::Recording) s.macro.clear(); // keep only the latest attempt
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        auto& s = S();
        if (s.mode == Mode::Analyzing) {
            finishTrial(false);
            return; // don't actually die
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void levelComplete() {
        auto& s = S();
        if (s.mode == Mode::Analyzing) {
            finishTrial(true);
            return; // no end screen, nothing submitted
        }
        if (s.mode == Mode::Recording) {
            s.mode = Mode::Idle;
            Notification::create(
                fmt::format("Macro saved: {} inputs", s.macro.size()),
                NotificationIcon::Success
            )->show();
        }
        PlayLayer::levelComplete();
    }
};

class $modify(FPPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({win.width - 60.f, win.height / 2.f});

        auto rec = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Record"), this, menu_selector(FPPauseLayer::onFPRecord));
        rec->setPosition({0.f, 25.f});
        menu->addChild(rec);

        auto ana = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Analyze"), this, menu_selector(FPPauseLayer::onFPAnalyze));
        ana->setPosition({0.f, -25.f});
        menu->addChild(ana);

        this->addChild(menu);
    }

    void onFPRecord(CCObject*) {
        S().mode = Mode::Recording;
        S().macro.clear();
        this->onRestart(nullptr); // restart from 0%; recording starts clean
    }

    void onFPAnalyze(CCObject*) {
        this->onResume(nullptr);
        startAnalysis();
    }
};
