// 06_breakout: everything composed — batched 2D canvas for the playfield,
// a CSS-styled HUD, and the audio mixer for effects. ~60 lines of engine
// API, the rest is game.

#include <rendy/rendy.hpp>

#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>

using namespace rendy;

namespace {

audio::SoundRef makeTone(audio::Mixer& mixer, float frequency, float seconds) {
    const int rate = 48000;
    const auto frames = static_cast<size_t>(seconds * rate);
    std::vector<float> pcm(frames);
    for (size_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / rate;
        const float envelope = std::min(1.0f, t * 60.0f) * std::exp(-6.0f * t / seconds);
        pcm[i] = 0.4f * envelope * std::sin(t * frequency * TwoPi);
    }
    return mixer.createSound(pcm.data(), frames, 1, rate).valueOr(audio::SoundRef{});
}

struct Brick {
    Rect rect;
    int hp = 1;
};

const Color kBrickColors[] = {Color::rgb(0x89B4FA), Color::rgb(0xA6E3A1),
                              Color::rgb(0xF9E2AF), Color::rgb(0xF38BA8)};

} // namespace

int main() {
    auto appResult = App::create({.title = "rendy — breakout", .size = {900, 700}});
    if (!appResult) {
        log::error("failed to start: {}", appResult.error().message);
        return 1;
    }
    auto app = std::move(appResult).value();

    audio::Mixer mixer;
    auto bounceSound = makeTone(mixer, 440.0f, 0.08f);
    auto brickSound = makeTone(mixer, 880.0f, 0.1f);
    auto loseSound = makeTone(mixer, 120.0f, 0.5f);
    auto winSound = makeTone(mixer, 660.0f, 0.4f);

    // ---- HUD -----------------------------------------------------------
    ui::Context hud(app);
    hud.addStylesheet(R"css(
        root { padding: 10px 16px; color: #cdd6f4; }
        .bar { flex-direction: row; justify-content: space-between; font-size: 16px; }
        .score { color: #f9e2af; }
        .lives { color: #f38ba8; }
        .banner { position: absolute; left: 0; right: 0; top: 45%; align-items: center; }
        .banner .big { font-size: 34px; color: #f5e0dc; }
        .banner .small { font-size: 15px; color: #a6adc8; margin-top: 6px; }
    )css");
    auto bar = hud.root().addChild("div", {.classes = "bar"});
    auto scoreLabel = bar.addChild("div", {.classes = "score", .text = "0 poäng"});
    auto fpsLabel = bar.addChild("div", {.text = ""});
    auto livesLabel = bar.addChild("div", {.classes = "lives", .text = "♥♥♥"});
    auto banner = hud.root().addChild("div", {.classes = "banner"});
    auto bannerBig = banner.addChild("div", {.classes = "big", .text = "BREAKOUT"});
    auto bannerSmall =
        banner.addChild("div", {.classes = "small", .text = "space eller klick för att börja"});

    // ---- game state ----------------------------------------------------
    enum class State { Ready, Playing, GameOver, Won };
    State state = State::Ready;
    int score = 0;
    int lives = 3;
    Vec2 ballPos{0.0f};
    Vec2 ballVel{0.0f};
    float paddleX = 0.5f;
    std::vector<Brick> bricks;
    std::mt19937 rng{12345};

    const float ballSpeed = 520.0f;
    const Vec2 paddleSize{110.0f, 14.0f};
    const float ballRadius = 8.0f;

    auto resetBricks = [&](Vec2 canvasSize) {
        bricks.clear();
        const int columns = 10;
        const int rows = 6;
        const float margin = 40.0f;
        const float gap = 6.0f;
        const float width = (canvasSize.x - 2.0f * margin - (columns - 1) * gap) / columns;
        const float height = 24.0f;
        for (int row = 0; row < rows; ++row)
            for (int col = 0; col < columns; ++col)
                bricks.push_back(
                    {{{margin + static_cast<float>(col) * (width + gap),
                       70.0f + static_cast<float>(row) * (height + gap)},
                      {width, height}},
                     rows - row <= 2 ? 2 : 1});
    };

    auto serve = [&](Vec2 canvasSize) {
        ballPos = {canvasSize.x * 0.5f, canvasSize.y - 120.0f};
        std::uniform_real_distribution<float> angleDist(-0.6f, 0.6f);
        const float angle = angleDist(rng);
        ballVel = Vec2{std::sin(angle), -std::cos(angle)} * ballSpeed;
    };

    double lastFpsUpdate = 0.0;
    while (app.pollEvents()) {
        const Input& input = app.input();
        if (input.keyPressed(Key::Escape)) app.quit();
        if (std::getenv("RENDY_AUTOQUIT") != nullptr && app.time() > 2.5) app.quit();

        auto frame = app.beginFrame({.clear = 0x11111BFF_rgba});
        auto canvas = frame.canvas();
        const Vec2 size = canvas.size();
        const float dt = std::min(app.dt(), 1.0f / 30.0f);

        // ---- update ----------------------------------------------------
        const bool startPressed =
            input.keyPressed(Key::Space) || input.mousePressed(MouseButton::Left);
        if (state != State::Playing && startPressed) {
            if (state != State::Ready || bricks.empty()) resetBricks(size);
            if (state == State::GameOver || state == State::Won) {
                score = 0;
                lives = 3;
                resetBricks(size);
            }
            serve(size);
            state = State::Playing;
        }

        // Paddle follows mouse; arrows work too.
        if (input.mouseDelta() != Vec2{0.0f}) paddleX = input.mousePos().x / size.x;
        if (input.keyDown(Key::Left)) paddleX -= 1.2f * dt;
        if (input.keyDown(Key::Right)) paddleX += 1.2f * dt;
        paddleX = std::clamp(paddleX, 0.0f, 1.0f);
        const Rect paddle{{paddleX * (size.x - paddleSize.x), size.y - 50.0f}, paddleSize};

        if (state == State::Playing) {
            ballPos += ballVel * dt;

            // Walls.
            if (ballPos.x < ballRadius || ballPos.x > size.x - ballRadius) {
                ballVel.x = -ballVel.x;
                ballPos.x = std::clamp(ballPos.x, ballRadius, size.x - ballRadius);
                mixer.play(bounceSound, {.volume = 0.5f});
            }
            if (ballPos.y < ballRadius) {
                ballVel.y = -ballVel.y;
                ballPos.y = ballRadius;
                mixer.play(bounceSound, {.volume = 0.5f});
            }

            // Paddle: reflect angle by hit position.
            if (ballVel.y > 0.0f &&
                paddle.expanded(ballRadius).contains(ballPos)) {
                const float hit =
                    std::clamp((ballPos.x - paddle.center().x) / (paddleSize.x * 0.5f), -1.0f, 1.0f);
                const float angle = hit * radians(60.0f);
                ballVel = Vec2{std::sin(angle), -std::cos(angle)} * ballSpeed;
                ballPos.y = paddle.top() - ballRadius;
                mixer.play(bounceSound, {.pan = hit * 0.6f});
            }

            // Bricks.
            for (auto it = bricks.begin(); it != bricks.end(); ++it) {
                if (!it->rect.expanded(ballRadius).contains(ballPos)) continue;
                // Reflect on the axis of least penetration.
                const Vec2 center = it->rect.center();
                const Vec2 delta = ballPos - center;
                const Vec2 halfSize = it->rect.size * 0.5f + Vec2{ballRadius};
                const Vec2 overlap = halfSize - glm::abs(delta);
                if (overlap.x < overlap.y)
                    ballVel.x = std::copysign(ballVel.x, delta.x);
                else
                    ballVel.y = std::copysign(ballVel.y, delta.y);
                const float pan = (center.x / size.x) * 2.0f - 1.0f;
                mixer.play(brickSound, {.volume = 0.7f, .pan = pan * 0.6f});
                score += 10;
                if (--it->hp <= 0) bricks.erase(it);
                scoreLabel.setText(fmt::format("{} poäng", score));
                break;
            }

            // Miss.
            if (ballPos.y > size.y + ballRadius) {
                lives--;
                std::string hearts;
                for (int i = 0; i < lives; ++i) hearts += "♥";
                livesLabel.setText(std::move(hearts));
                if (lives <= 0) {
                    state = State::GameOver;
                    mixer.play(loseSound, {});
                } else {
                    serve(size);
                    mixer.play(loseSound, {.volume = 0.5f});
                }
            }
            if (bricks.empty()) {
                state = State::Won;
                mixer.play(winSound, {});
            }
        }

        // ---- paint -----------------------------------------------------
        for (const Brick& brick : bricks) {
            const Color color = kBrickColors[std::min<int>(brick.hp, 3)];
            canvas.drawRect(brick.rect,
                            {.color = brick.hp > 1 ? color : color.fade(0.85f),
                             .cornerRadius = 5.0f});
        }
        canvas.drawRect(paddle, {.color = 0xCDD6F4FF_rgba, .cornerRadius = 7.0f});
        if (state == State::Playing)
            canvas.drawRect({ballPos - Vec2{ballRadius}, Vec2{2.0f * ballRadius}},
                            {.color = 0xF5E0DCFF_rgba, .cornerRadius = ballRadius});

        // Banner text per state.
        const bool showBanner = state != State::Playing;
        bannerBig.setStyle(ui::Style{}.display(showBanner ? ui::Display::Flex : ui::Display::None));
        bannerSmall.setStyle(
            ui::Style{}.display(showBanner ? ui::Display::Flex : ui::Display::None));
        if (state == State::GameOver) bannerBig.setText("GAME OVER");
        else if (state == State::Won) bannerBig.setText(fmt::format("DU VANN! {} poäng", score));
        else bannerBig.setText("BREAKOUT");

        if (app.time() - lastFpsUpdate > 0.5) {
            lastFpsUpdate = app.time();
            fpsLabel.setText(fmt::format("{:.0f} fps", app.fps()));
        }

        hud.update();
        hud.paint(canvas);
        frame.present();
    }
    return 0;
}
