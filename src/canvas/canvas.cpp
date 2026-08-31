#include "rendy/canvas/canvas.hpp"

#include "canvas/canvas_data.hpp"

namespace rendy {

using detail::CanvasData;
using detail::Quad2D;

void Canvas::drawRect(const Rect& rect, const DrawRectOptions& options) {
    if (rect.empty()) return;
    Quad2D quad{};
    quad.rect = {rect.pos.x, rect.pos.y, rect.size.x, rect.size.y};
    quad.uvRect = {0.0f, 0.0f, 1.0f, 1.0f};
    quad.color = {options.color.r, options.color.g, options.color.b, options.color.a};
    quad.radii = options.cornerRadii.x >= 0.0f ? options.cornerRadii
                                               : Vec4{options.cornerRadius};
    quad.borderColor = {options.borderColor.r, options.borderColor.g, options.borderColor.b,
                        options.borderColor.a};
    quad.info = {options.borderWidth, 0.0f, static_cast<float>(data_->currentClip()),
                 detail::kQuadKindSolid};
    data_->quads.push_back(quad);
}

void Canvas::drawImage(TextureRef texture, const Rect& rect, const DrawImageOptions& options) {
    if (rect.empty()) return;
    Quad2D quad{};
    quad.rect = {rect.pos.x, rect.pos.y, rect.size.x, rect.size.y};
    quad.uvRect = {options.uv.left(), options.uv.top(), options.uv.right(),
                   options.uv.bottom()};
    quad.color = {options.tint.r, options.tint.g, options.tint.b, options.tint.a};
    quad.radii = Vec4{options.cornerRadius};
    quad.borderColor = Vec4{0.0f};
    quad.info = {0.0f, static_cast<float>(texture.index),
                 static_cast<float>(data_->currentClip()), detail::kQuadKindImage};
    data_->quads.push_back(quad);
}

void Canvas::pushClip(const Rect& rect) {
    const Vec4 current = data_->clips[data_->currentClip()];
    const Rect currentRect{{current.x, current.y}, {current.z - current.x, current.w - current.y}};
    const Rect clipped = currentRect.intersect(rect);
    data_->clips.push_back(
        Vec4{clipped.left(), clipped.top(), clipped.right(), clipped.bottom()});
    data_->clipStack.push_back(static_cast<uint32_t>(data_->clips.size() - 1));
}

void Canvas::popClip() {
    if (data_->clipStack.size() > 1) data_->clipStack.pop_back();
}

Vec2 Canvas::size() const { return data_->viewport; }

} // namespace rendy
