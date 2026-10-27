#pragma once
#include "imgui.h"
#include <cmath>
#include <cstdint>
#include <vector>

class NeuronField {
public:

    void init(float w, float h, int count = 58, uint32_t seed = 0x5EED01u) {
        w_ = (w > 1.0f) ? w : 1.0f;
        h_ = (h > 1.0f) ? h : 1.0f;
        nodes_.clear();
        nodes_.reserve((size_t)count);
        uint32_t s = seed ? seed : 1u;
        auto rnd = [&s]() {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return (float)(s & 0xFFFFFF) / (float)0x1000000;
        };
        for (int i = 0; i < count; ++i) {
            Node n;
            n.x  = rnd() * w_;
            n.y  = rnd() * h_;
            float ang  = rnd() * 6.2831853f;
            float vel  = 9.0f + rnd() * 24.0f;
            n.vx = cosf(ang) * vel;
            n.vy = sinf(ang) * vel;
            n.r     = 1.4f + rnd() * 2.6f;
            n.phase = rnd() * 6.2831853f;
            n.speed = 0.6f + rnd() * 1.6f;
            nodes_.push_back(n);
        }
        ready_ = true;
    }

    void set_extent(float w, float h) {
        if (w > 1.0f) w_ = w;
        if (h > 1.0f) h_ = h;
    }

    void update(float dt) {
        if (!ready_) return;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        time_ += dt;
        const float mx = w_ + 40.0f, my = h_ + 40.0f;
        for (auto& n : nodes_) {
            n.x += n.vx * dt;
            n.y += n.vy * dt;
            if (n.x < -20.0f) n.x += mx; else if (n.x > mx) n.x -= mx;
            if (n.y < -20.0f) n.y += my; else if (n.y > my) n.y -= my;
        }
    }

    void draw(ImDrawList* dl, float w, float h) const {
        if (!dl || !ready_ || nodes_.empty()) return;
        const ImU32 top = IM_COL32(5, 13, 5, 255);
        const ImU32 bot = IM_COL32(0, 0, 0, 255);
        dl->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(w, h), top, top, bot, bot);

        const float kMax  = 168.0f;
        const float kMax2 = kMax * kMax;
        const size_t n = nodes_.size();
        for (size_t i = 0; i < n; ++i) {
            const Node& a = nodes_[i];
            float ga = 0.55f + 0.45f * sinf(time_ * a.speed + a.phase);
            for (size_t j = i + 1; j < n; ++j) {
                const Node& b = nodes_[j];
                float dx = a.x - b.x, dy = a.y - b.y;
                float d2 = dx * dx + dy * dy;
                if (d2 >= kMax2) continue;
                float f = 1.0f - d2 / kMax2;
                float gb = 0.55f + 0.45f * sinf(time_ * b.speed + b.phase);
                float g = ga * gb;
                int alpha = (int)(66.0f * f * f * g);
                if (alpha < 6) continue;
                dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y),
                            IM_COL32(150, 235, 64, alpha), 1.0f);
            }
        }
        for (const auto& n2 : nodes_) {
            float g = 0.5f + 0.5f * sinf(time_ * n2.speed + n2.phase);
            float r = n2.r * (0.9f + 0.3f * g);
            int alpha = (int)(70.0f + 150.0f * g);
            if (alpha > 210) alpha = 210;
            dl->AddCircleFilled(ImVec2(n2.x, n2.y), r, IM_COL32(173, 255, 47, alpha), 14);
            if (g > 0.985f)
                dl->AddCircleFilled(ImVec2(n2.x, n2.y), r * 3.2f, IM_COL32(173, 255, 47, 22), 20);
        }
    }

    void draw_now(float dt, float w, float h) {
        if (!ready_) init(w, h);
        else set_extent(w, h);
        update(dt);
        draw(ImGui::GetBackgroundDrawList(), w_, h_);
    }

private:
    struct Node { float x, y, vx, vy, r, phase, speed; };
    std::vector<Node> nodes_;
    float w_ = 1280.0f, h_ = 720.0f;
    float time_ = 0.0f;
    bool  ready_ = false;
};
