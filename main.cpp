#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <random>
#include <string>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template <class T>
static T S(char const* key) {
	return Mod::get()->template getSettingValue<T>(key);
}

static double nowMs() {
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static std::mt19937& rng() {
	static std::mt19937 r{std::random_device{}()};
	return r;
}

static double rand01() {
	return std::uniform_real_distribution<double>(0.0, 1.0)(rng());
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

struct SpamState {
	std::deque<double> downs;   // timestamps (ms) of recent REAL presses
	bool active = false;        // assist currently driving the input
	bool physDown = false;      // is the real button held right now
	bool gameDown = false;      // is the game currently receiving "down"
	bool injecting = false;     // guard so our own clicks bypass the hook
	bool pressWasAlt = false;   // last injected press used player alternation
	bool targetP1 = true;       // which player got the last alternated press
	double physDownAt = 0.0;
	double lastPhys = 0.0;
	double phaseStart = 0.0;    // when the current press/release phase began
	double jit = 1.0;           // humanization multiplier for the current phase
	double cps = 0.0;           // detected (smoothed) CPS
	double drift = 0.0;         // humanization CPS drift
	double driftTarget = 0.0;
	double driftTimer = 0.0;

	// ship straight-flight controller
	bool hasTarget = false;
	double targetY = 0.0;
	double lastY = 0.0;
	double bias = 0.5;          // learned hold/release balance
	bool shipLocked = false;

	long shownTenths = -1;
	bool shownShip = false;
	unsigned altCounter = 0;
	WeakRef<CCLabelBMFont> label;   // weak: the label dies with its PlayLayer
};

static SpamState g;

static void resetState() {
	auto weak = g.label;
	g = SpamState{};
	g.label = weak;
	if (auto label = weak.lock()) label->setVisible(false);
}

static void updateLabel() {
	auto label = g.label.lock();
	if (!label) return;
	bool show = g.active && S<bool>("show-indicator");
	label->setVisible(show);
	if (!show) {
		g.shownTenths = -1;
		return;
	}
	double cps = S<std::string>("mode") == "Fixed" ? S<double>("fixed-cps") : g.cps;
	cps = std::min(cps, S<double>("max-cps"));
	long tenths = std::lround(cps * 10.0);
	if (tenths != g.shownTenths || g.shownShip != g.shipLocked) {
		g.shownTenths = tenths;
		g.shownShip = g.shipLocked;
		label->setString(
			fmt::format("SPAM ASSIST  {:.1f} CPS{}", tenths / 10.0, g.shipLocked ? "  |  SHIP" : "").c_str()
		);
	}
}

static bool isActivePlayLayer(GJBaseGameLayer* layer) {
	auto pl = PlayLayer::get();
	return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
}

static bool inShip(GJBaseGameLayer* l) {
	return l->m_player1 && l->m_player1->m_isShip;
}

// Send a synthetic press/release into the game.
static void sendInput(GJBaseGameLayer* l, bool down) {
	g.injecting = true;
	bool dual = l->m_gameState.m_isDualMode;
	if (down) {
		bool alt = dual && S<bool>("alternate-players");
		g.pressWasAlt = alt;
		if (alt) {
			g.targetP1 = (g.altCounter++ % 2 == 0);
			l->handleButton(true, 1, g.targetP1);
		} else {
			l->handleButton(true, 1, true);
			if (dual) l->handleButton(true, 1, false);
		}
	} else {
		if (g.pressWasAlt) {
			l->handleButton(false, 1, g.targetP1);
		} else {
			l->handleButton(false, 1, true);
			if (dual) l->handleButton(false, 1, false);
		}
	}
	g.injecting = false;
	g.gameDown = down;
}

// Stop driving input and make the game match what the real button is doing.
static void deactivate(GJBaseGameLayer* l) {
	if (!g.active) return;
	g.active = false;
	if (g.physDown) {
		if (!g.gameDown) {
			sendInput(l, true);
		} else if (g.pressWasAlt) {
			sendInput(l, false);
			sendInput(l, true);
		}
	} else if (g.gameDown) {
		sendInput(l, false);
	}
	g.downs.clear();
	g.hasTarget = false;
	g.shipLocked = false;
	updateLabel();
}

// ---------------------------------------------------------------------------
// Trigger keys
// ---------------------------------------------------------------------------

// Key mode is on when "Only Use Trigger Keys" is enabled: clicks are then
// measured from the trigger keybinds instead of from every jump input.
static bool keyMode() {
	return S<bool>("trigger-keys-only");
}

// Record a real press / release (from the game's input or from a trigger key).
static void noteDown(double t) {
	// Both players receive the same click in dual mode: ignore the duplicate.
	if (!g.downs.empty() && t - g.downs.back() < 4.0) {
		g.physDown = true;
		g.lastPhys = t;
		return;
	}
	// Smooth the detected CPS while active ("Match" mode).
	if (g.active && !g.downs.empty()) {
		double gap = t - g.downs.back();
		if (gap > 1.0) {
			double inst = std::clamp(1000.0 / gap, 2.0, 60.0);
			g.cps = g.cps * 0.7 + inst * 0.3;
		}
	}
	g.physDown = true;
	g.physDownAt = t;
	g.lastPhys = t;
	g.downs.push_back(t);
	if (g.downs.size() > 64) g.downs.pop_front();
}

static void noteUp(GJBaseGameLayer* l, double t) {
	if (!g.physDown) return;
	g.physDown = false;
	g.lastPhys = t;
	double held = t - g.physDownAt;
	if (held > static_cast<double>(S<int64_t>("hold-release-ms"))) {
		// A deliberate hold is not spam.
		g.downs.clear();
		deactivate(l);
	}
}

// Decide whether the player's recent clicks count as "spam".
static bool shouldEngage(GJBaseGameLayer* l) {
	if (l->m_player1 && l->m_player1->m_isDead) return false;

	double now = nowMs();
	double window = static_cast<double>(S<int64_t>("window-ms"));
	while (!g.downs.empty() && now - g.downs.front() > window) g.downs.pop_front();

	size_t n = g.downs.size();
	if (n < static_cast<size_t>(S<int64_t>("min-clicks"))) return false;

	double span = g.downs.back() - g.downs.front();
	if (span <= 0.0) return false;

	double cps = (n - 1) * 1000.0 / span;
	if (cps < S<double>("cps-threshold")) return false;

	if (S<bool>("smart-detect")) {
		std::vector<double> iv;
		iv.reserve(n - 1);
		for (size_t i = 1; i < n; i++) iv.push_back(g.downs[i] - g.downs[i - 1]);

		double mean = span / static_cast<double>(n - 1);
		double var = 0.0;
		for (double v : iv) var += (v - mean) * (v - mean);
		var /= static_cast<double>(iv.size());
		double cv = std::sqrt(var) / mean;

		// Timing must be even enough to look like rhythmic spam.
		if (cv > S<double>("consistency")) return false;
		// Player is slowing down / ending the spam: don't take over.
		if (iv.back() > mean * 1.8) return false;
	}

	g.cps = cps;
	return true;
}

// Ship straight-flight: a small PID-style controller that picks the share of each
// click cycle the button is held, so the ship's height stays locked in place.
static double shipDuty(GJBaseGameLayer* l, double dt) {
	auto p = l->m_player1;
	double y = p->getPositionY();
	double sdt = std::clamp(dt, 1e-4, 0.05);

	if (!g.hasTarget) {
		g.targetY = y;
		g.lastY = y;
		g.bias = 0.5;
		g.hasTarget = true;
		return 0.5;
	}

	double vy = (y - g.lastY) / sdt;
	g.lastY = y;

	// Big jump (portal, gravity change, etc.): lock onto the new height.
	if (std::abs(g.targetY - y) > 150.0) {
		g.targetY = y;
		g.bias = 0.5;
		return 0.5;
	}

	double k = S<double>("ship-correction");
	double err = g.targetY - y;
	double sign = p->m_isUpsideDown ? -1.0 : 1.0;

	g.bias += sign * 0.002 * k * err * sdt;
	g.bias = std::clamp(g.bias, 0.2, 0.8);

	double u = g.bias + sign * (0.006 * k * err - 0.003 * k * vy);
	return std::clamp(u, 0.12, 0.88);
}

// Random timing multiplier for the next press/release phase.
static double phaseJitter(bool humanizeActive) {
	if (!humanizeActive) return 1.0;
	double sigma = S<double>("humanize-timing") / 100.0 / 2.0;
	if (sigma <= 0.0) return 1.0;
	std::normal_distribution<double> nd(0.0, sigma);
	return std::clamp(1.0 + nd(rng()), 0.5, 1.6);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

class $modify(SpamAssistLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		if (g.injecting || button != 1 || !S<bool>("enabled") || !isActivePlayLayer(this)) {
			return GJBaseGameLayer::handleButton(down, button, isPlayer1);
		}

		bool byKeys = keyMode();

		// In "Any" mode every real jump input (either player) counts as a click.
		// In key mode, clicks are measured from the trigger keys instead.
		if (!byKeys) {
			double t = nowMs();
			if (down) noteDown(t);
			else noteUp(this, t);
		}

		// While the assist drives, real input is swallowed.
		if (g.active) return;

		GJBaseGameLayer::handleButton(down, button, isPlayer1);
		g.gameDown = down;

		if (!byKeys && down && shouldEngage(this)) {
			g.active = true;
			g.phaseStart = nowMs();
			g.jit = 1.0;
			g.hasTarget = false;
			updateLabel();
		}
	}

	void update(float dt) {
		if (g.active && isActivePlayLayer(this)) {
			double t = nowMs();
			double hold = static_cast<double>(S<int64_t>("hold-release-ms"));
			double idle = static_cast<double>(S<int64_t>("idle-timeout-ms"));

			if (!S<bool>("enabled")) {
				deactivate(this);
			} else if (g.physDown && t - g.physDownAt > hold) {
				deactivate(this);          // user is holding: give control back
			} else if (!g.physDown && t - g.lastPhys > idle) {
				deactivate(this);          // user stopped spamming
			} else if (this->m_player1 && this->m_player1->m_isDead) {
				deactivate(this);
			} else {
				bool ship = inShip(this) && S<bool>("ship-straight");
				g.shipLocked = ship;
				if (!ship) g.hasTarget = false;

				// Humanization is skipped in ship mode unless explicitly allowed,
				// so the ship stays perfectly straight.
				bool human = S<bool>("humanize") && (!ship || S<bool>("humanize-ship"));

				// Slow CPS drift, like a real hand speeding up and slowing down.
				double drift = 0.0;
				if (human) {
					g.driftTimer -= dt;
					if (g.driftTimer <= 0.0) {
						g.driftTarget = (rand01() * 2.0 - 1.0) * S<double>("humanize-drift");
						g.driftTimer = 0.4 + rand01() * 0.9;
					}
					g.drift += (g.driftTarget - g.drift) * std::min(1.0, static_cast<double>(dt) * 3.0);
					drift = g.drift;
				} else {
					g.drift = g.driftTarget = 0.0;
				}

				double cps = S<std::string>("mode") == "Fixed" ? S<double>("fixed-cps") : g.cps;
				cps = std::clamp(cps + drift, 2.0, std::max(2.0, S<double>("max-cps")));

				double period = 1000.0 / cps;
				double duty = std::clamp<int64_t>(S<int64_t>("duty-cycle"), 10, 90) / 100.0;
				if (ship) duty = shipDuty(this, dt);

				double downDur = period * duty;
				double upDur = period - downDur;

				// If we fell far behind (lag spike), resync instead of burst-clicking.
				if (t - g.phaseStart > period * 2.0) g.phaseStart = t;

				double dur = (g.gameDown ? downDur : upDur) * g.jit;
				if (t - g.phaseStart >= dur) {
					g.phaseStart += dur;
					g.jit = phaseJitter(human);

					if (g.gameDown) {
						sendInput(this, false);
					} else {
						// Optional human "missed click" (never in ship mode).
						bool miss = human && !ship &&
							rand01() < S<double>("humanize-miss") / 100.0;
						if (!miss) sendInput(this, true);
					}
				}
				updateLabel();
			}
		}
		GJBaseGameLayer::update(dt);
	}
};

class $modify(SpamAssistPlayLayer, PlayLayer) {
	void setupHasCompleted() {
		// Drop any label from a previous level before the original runs,
		// because it calls resetLevel (our hook) internally.
		g.label = WeakRef<CCLabelBMFont>();
		resetState();
		PlayLayer::setupHasCompleted();
		resetState();

		auto label = CCLabelBMFont::create("", "bigFont.fnt");
		label->setID("indicator"_spr);
		label->setScale(0.35f);
		label->setOpacity(170);
		label->setAnchorPoint({0.f, 1.f});
		auto win = CCDirector::get()->getWinSize();
		label->setPosition({6.f, win.height - 6.f});
		label->setVisible(false);
		label->setZOrder(100);
		this->m_uiLayer->addChild(label);
		g.label = label;
	}

	void resetLevel() {
		resetState();
		PlayLayer::resetLevel();
	}

	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		resetState();
		PlayLayer::destroyPlayer(player, obj);
	}

	void pauseGame(bool p0) {
		resetState();
		PlayLayer::pauseGame(p0);
	}

	void onQuit() {
		resetState();
		g.label = WeakRef<CCLabelBMFont>();
		PlayLayer::onQuit();
	}
};

// ---------------------------------------------------------------------------
// Trigger keybinds (Geode's built-in keybind settings)
// ---------------------------------------------------------------------------

static void onTriggerKey(bool down) {
	if (!S<bool>("enabled") || !keyMode()) return;

	auto pl = PlayLayer::get();
	auto scene = CCScene::get();
	bool paused = scene && scene->getChildByType<PauseLayer>(0);
	if (!pl || paused) return;

	auto layer = static_cast<GJBaseGameLayer*>(pl);
	double t = nowMs();

	if (down) {
		noteDown(t);
		if (!g.active && shouldEngage(layer)) {
			g.active = true;
			g.phaseStart = t;
			g.jit = 1.0;
			g.hasTarget = false;
			// The game may not have seen this press yet: make sure it is down.
			if (!g.gameDown) sendInput(layer, true);
			updateLabel();
		}
	} else {
		noteUp(layer, t);
	}
}

$on_game(Loaded) {
	listenForKeybindSettingPresses("trigger-keybinds",
		[](Keybind const& keybind, bool down, bool repeat, double timestamp) {
			if (repeat) return;
			onTriggerKey(down);
		}
	);
}
