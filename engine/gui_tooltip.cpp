#include "gui_tooltip.h"

#include "gui.h"
#include "gui_richtext.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StyleSheetContainer.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace kast {
namespace {

// The tooltip's document is only a layer over the context: whatever a sheet's body rule says, it fills the area, draws
// nothing itself and never takes the mouse.
constexpr const char* kLayerStyle = "display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; "
                                    "margin: 0; padding: 0; border-width: 0; background-color: transparent; "
                                    "decorator: none; overflow: visible; pointer-events: none;";

bool blank(const Rml::String& text) { return text.find_first_not_of(" \t\r\n") == Rml::String::npos; }

} // namespace

GuiTooltipSpot guiTooltipSpot(float mouseX, float mouseY, float w, float h, float areaW, float areaH, float offset)
{
    const auto axis = [offset](float mouse, float size, float area) {
        float at = mouse + offset;
        if (at + size > area)
            at = mouse - offset - size; // flipped to the other side of the cursor
        return std::max(0.0f, std::min(at, area - size));
    };
    return {axis(mouseX, w, areaW), axis(mouseY, h, areaH)};
}

Rml::Element* guiTooltipSource(Rml::Element* element)
{
    for (; element; element = element->GetParentNode())
        if (const Rml::Variant* text = element->GetAttribute(kGuiTooltipAttribute); text && !blank(text->Get<Rml::String>()))
            return element;
    return nullptr;
}

void GuiTooltip::watch(Watch& watch, Rml::Element* element)
{
    if (element)
        watch.emplace(element->GetObserverPtr());
    else
        watch.reset();
}

void GuiTooltip::mouseMove(float x, float y)
{
    mouseX_ = x;
    mouseY_ = y;
}

void GuiTooltip::press()
{
    held_ = true;
    watch(suppressed_, get(source_));
}

void GuiTooltip::release() { held_ = false; }

void GuiTooltip::reset()
{
    document_ = nullptr;
    box_ = nullptr;
    source_.reset();
    suppressed_.reset();
    shown_ = false;
}

void GuiTooltip::update(Rml::Context& context)
{
    update(context, clock ? clock() : static_cast<double>(SDL_GetTicksNS()) * 1e-9);
}

void GuiTooltip::update(Rml::Context& context, double now)
{
    Rml::Element* source = held_ ? nullptr : guiTooltipSource(context.GetHoverElement());
    if (source && (!source->IsVisible(true) || source->GetOwnerDocument() == document_))
        source = nullptr;
    if (!held_ && source != get(suppressed_))
        suppressed_.reset();
    if (source && source == get(suppressed_))
        source = nullptr;
    if (source != get(source_)) {
        hide();
        watch(source_, source);
        since_ = now;
    }
    if (!source) {
        hide();
        return;
    }
    const Rml::String text = source->GetAttribute<Rml::String>(kGuiTooltipAttribute, "");
    if (!shown_ && now - since_ >= kGuiTooltipDelay) {
        text_ = text;
        show(context);
    } else if (shown_ && text != text_) { // kast.gui.setTooltip while it shows
        text_ = text;
        show(context);
    }
    if (shown_)
        place(context);
}

void GuiTooltip::show(Rml::Context& context)
{
    if (!document_) {
        document_ = context.CreateDocument();
        if (!document_)
            return;
        document_->SetAttribute("style", kLayerStyle);
        box_ = document_->AppendChild(document_->CreateElement("div"));
        box_->SetId(kGuiTooltipId);
    }
    // The source document's sheets (its own over the default widget style), so a project rule on #kast-tooltip wins.
    static const Rml::StyleSheetContainer none;
    Rml::ElementDocument* owner = get(source_)->GetOwnerDocument();
    const Rml::StyleSheetContainer* sheets = owner ? owner->GetStyleSheetContainer() : nullptr;
    document_->SetStyleSheetContainer(sheets ? sheets->CombineStyleSheetContainer(none) : nullptr);
    if (!sheets)
        gui::addWidgetStyle(*document_);
    box_->SetInnerRML(guiRichTextRml(text_, nullptr, gui::fontFamilyLoaded));
    // Laid out at the area's top left with no width set, it wraps at its max-width only; that width is then fixed, so
    // moving it near the right edge doesn't wrap it narrower.
    box_->SetProperty("position", "absolute");
    box_->SetProperty("pointer-events", "none");
    box_->RemoveProperty("width");
    box_->SetProperty("left", "0px");
    box_->SetProperty("top", "0px");
    document_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    document_->UpdateDocument();
    const float width = box_->GetBox().GetSize(Rml::BoxArea::Content).x;
    box_->SetProperty(Rml::PropertyId::Width, Rml::Property(std::ceil(width) + 1.0f, Rml::Unit::PX));
    shown_ = true;
}

void GuiTooltip::hide()
{
    if (shown_ && document_)
        document_->Hide();
    shown_ = false;
}

void GuiTooltip::place(Rml::Context& context)
{
    if (context.GetDocument(context.GetNumDocuments() - 1) != document_)
        document_->PullToFront(); // a document loaded or restacked since: the tooltip stays on top
    const Rml::Vector2f size = box_->GetBox().GetSize(Rml::BoxArea::Border);
    const Rml::Vector2i area = context.GetDimensions();
    const GuiTooltipSpot spot =
        guiTooltipSpot(mouseX_, mouseY_, size.x, size.y, static_cast<float>(area.x), static_cast<float>(area.y),
                       kGuiTooltipOffsetDp * context.GetDensityIndependentPixelRatio());
    box_->SetProperty(Rml::PropertyId::Left, Rml::Property(spot.x, Rml::Unit::PX));
    box_->SetProperty(Rml::PropertyId::Top, Rml::Property(spot.y, Rml::Unit::PX));
    document_->UpdateDocument();
}

} // namespace kast
